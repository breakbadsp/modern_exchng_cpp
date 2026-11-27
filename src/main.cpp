#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
#include "market_data.hpp"
#include "replicated_exchange.hpp"
#include "spsc_queue.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <latch>
#include <memory>
#include <optional>
#include <print>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

// The exchange binary (prep/io-design-spec.md, step 7). Each engine runs on its
// own pinned thread, the gateway on another thread, and the main thread waits
// for SIGINT or SIGTERM, then stops and joins them all.
namespace
{

using mex::Command;
using mex::Config;
using mex::Engine;
using mex::EngineQueues;
using mex::Gateway;
using mex::MarketDataPublisher;
using mex::Report;
using mex::SpscQueue;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr auto kStopPollInterval = std::chrono::milliseconds{50};
// Linux permits 15 name bytes and a terminating NUL.
constexpr std::size_t kThreadNameBytes = 16;

// Set by the signal handler. A lock-free atomic store is signal-safe.
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void OnStopSignal(int) { stop_requested.store(true, std::memory_order_relaxed); }

[[nodiscard]] std::expected<void, int> InstallStopHandler()
{
  struct sigaction action{};
  action.sa_handler = OnStopSignal;
  sigemptyset(&action.sa_mask);
  for (const int signal_number : {SIGINT, SIGTERM})
  {
    if (::sigaction(signal_number, &action, nullptr) != 0)
    {
      return std::unexpected(errno);
    }
  }
  return {};
}

// Pins the calling thread. Returns the error number on failure.
[[nodiscard]] std::expected<void, int> PinToCore(std::uint32_t p_core)
{
  if (p_core >= CPU_SETSIZE)
  {
    return std::unexpected(EINVAL);
  }
  cpu_set_t cpus{};
  CPU_ZERO(&cpus);
  CPU_SET(p_core, &cpus);
  const int result = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpus), &cpus);
  if (result != 0)
  {
    return std::unexpected(result);
  }
  return {};
}

// What main owns for one engine. The Engine itself lives on its thread.
struct EngineSlot
{
  EngineSlot(const mex::EngineConfig &p_config, const Config &p_exchange,
             MarketDataPublisher *p_market_data)
      : config_{p_config}, session_count_{p_exchange.sessions_.size()},
        inbound_{p_exchange.gateway_.inbound_queue_capacity_},
        outbound_{p_exchange.gateway_.outbound_queue_capacity_}, market_data_{p_market_data}
  {
  }

  const mex::EngineConfig &config_;
  std::size_t session_count_;
  SpscQueue<Command> inbound_;
  SpscQueue<Report> outbound_;
  MarketDataPublisher *market_data_;
  std::uint64_t stalls_ = 0; // written by the engine thread before it ends
};

// Engine thread. It builds its Engine so that its Books are first touched here.
void RunEngine(std::stop_token p_stop, EngineSlot &p_slot, std::latch &p_ready, std::size_t p_index)
{
  std::array<char, kThreadNameBytes> name{"mex-engine-"};
  const auto converted =
      std::to_chars(name.data() + std::strlen(name.data()), name.data() + name.size() - 1, p_index);
  const int named =
      converted.ec == std::errc{} ? ::pthread_setname_np(::pthread_self(), name.data()) : ERANGE;
  if (named != 0)
  {
    std::println(stderr, "mex_exchange: warning: cannot name engine {} thread: {}", p_index,
                 std::strerror(named));
  }
  if (const auto pinned = PinToCore(p_slot.config_.core_); !pinned.has_value())
  {
    std::println(stderr, "mex_exchange: warning: engine thread not pinned to core {}: {}",
                 p_slot.config_.core_, std::strerror(pinned.error()));
  }
  Engine engine{p_slot.config_.books_, p_slot.session_count_, p_slot.inbound_, p_slot.outbound_};
  if (p_slot.market_data_ != nullptr)
  {
    engine.SetMarketData(*p_slot.market_data_);
  }
  // The callback runs on the thread that requests the stop. The destructor of
  // stop_engine waits for a running callback, so engine outlives it.
  const std::stop_callback stop_engine{p_stop, [&engine] { engine.Stop(); }};
  p_ready.count_down();
  engine.Run();
  p_slot.stalls_ = engine.OutboundStalls();
}

[[nodiscard]] int RunExchange(const Config &p_config, MarketDataPublisher *p_market_data)
{
  if (const auto installed = InstallStopHandler(); !installed.has_value())
  {
    std::println(stderr, "mex_exchange: sigaction: {}", std::strerror(installed.error()));
    return kExitFailure;
  }
  auto sockets = mex::OpenGatewaySockets(p_config.gateway_);
  if (!sockets.has_value())
  {
    std::println(stderr, "mex_exchange: {}", sockets.error());
    return kExitFailure;
  }
  const std::uint16_t port = sockets->port_;

  std::vector<std::unique_ptr<EngineSlot>> slots;
  std::vector<EngineQueues> queues;
  for (const mex::EngineConfig &engine_config : p_config.engines_)
  {
    slots.push_back(std::make_unique<EngineSlot>(engine_config, p_config, p_market_data));
    queues.push_back({slots.back()->inbound_, slots.back()->outbound_});
  }

  std::latch engines_ready{static_cast<std::ptrdiff_t>(slots.size())};
  std::vector<std::jthread> engine_threads;
  for (std::size_t index = 0; index < slots.size(); ++index)
  {
    engine_threads.emplace_back(RunEngine, std::ref(*slots[index]), std::ref(engines_ready), index);
  }
  engines_ready.wait();
  const auto market_thread = p_market_data != nullptr ? p_market_data->Start() : std::jthread{};

  Gateway gateway{p_config, std::move(*sockets), queues};
  std::jthread gateway_thread{
      [gateway_ptr = &gateway]
      {
        if (const int named = ::pthread_setname_np(::pthread_self(), "mex-gateway"); named != 0)
        {
          std::println(stderr, "mex_exchange: warning: cannot name gateway thread: {}",
                       std::strerror(named));
        }
        gateway_ptr->Run();
      }};
  std::println("mex_exchange: listening on {}:{} with {} engines",
               p_config.gateway_.listen_address_, port, slots.size());
  std::fflush(stdout);

  while (!stop_requested.load(std::memory_order_relaxed))
  {
    std::this_thread::sleep_for(kStopPollInterval);
  }

  // Engines first: an engine can spin on a full outbound queue, and only the
  // running gateway drains it.
  for (std::jthread &thread : engine_threads)
  {
    thread.request_stop();
    thread.join();
  }
  gateway.Stop();
  gateway_thread.join();

  for (std::size_t index = 0; index < slots.size(); ++index)
  {
    std::println("mex_exchange: engine {} outbound stalls: {}", index, slots[index]->stalls_);
  }
  std::println("mex_exchange: gateway dropped reports: {}", gateway.DroppedReports());
  return 0;
}

struct Arguments
{
  std::optional<std::uint16_t> market_port_{};
  std::size_t next_ = 2;
};

[[nodiscard]] std::optional<Arguments> ParseOptions(std::span<char *const> p_args)
{
  Arguments options;
  if (p_args.size() > options.next_ &&
      std::string_view{p_args[options.next_]} == "--market-data-port")
  {
    if (p_args.size() <= options.next_ + 1)
    {
      return std::nullopt;
    }
    const std::string_view number{p_args[options.next_ + 1]};
    std::uint16_t port = 0;
    const auto result = std::from_chars(number.data(), number.data() + number.size(), port);
    if (result.ec != std::errc{} || result.ptr != number.data() + number.size())
    {
      return std::nullopt;
    }
    options.market_port_ = port;
    options.next_ += 2;
  }
  // Without this check, an unknown option and its value pass as a replica number and data path.
  if (p_args.size() > options.next_ && std::string_view{p_args[options.next_]}.starts_with("--"))
  {
    return std::nullopt;
  }
  if (p_args.size() != options.next_ && p_args.size() != options.next_ + 2)
  {
    return std::nullopt;
  }
  return options;
}

} // namespace

int main(int p_argc, char **p_argv)
{
  const std::span<char *const> args{p_argv, static_cast<std::size_t>(p_argc)};
  const auto options = ParseOptions(args);
  if (!options)
  {
    std::println(stderr, "usage: mex_exchange <config path> "
                         "[--market-data-port <port>] [<replica number> <data path>]");
    return kExitUsage;
  }
  const char *const path = args[1];
  const auto config = mex::LoadConfig(path);
  if (!config.has_value())
  {
    const mex::ConfigError &error = config.error();
    if (error.line_ == 0)
    {
      std::println(stderr, "mex_exchange: {}: {}", path, error.reason_);
    }
    else
    {
      std::println(stderr, "mex_exchange: {}:{}: {}", path, error.line_, error.reason_);
    }
    return kExitFailure;
  }
  const bool replicated = args.size() == options->next_ + 2;
  if (replicated != !config->replicas_.empty())
  {
    std::println(stderr, "mex_exchange: a config with replica records needs <replica number> "
                         "<data path>, and a config without them takes neither");
    return kExitUsage;
  }
  std::optional<MarketDataPublisher> market_data;
  if (options->market_port_)
  {
    auto publisher = MarketDataPublisher::Create(*config, *options->market_port_);
    if (!publisher)
    {
      std::println(stderr, "mex_exchange: market data: {}", publisher.error());
      return kExitFailure;
    }
    market_data.emplace(std::move(*publisher));
  }
  auto *const market_publisher = market_data ? &*market_data : nullptr;
  if (!replicated)
  {
    return RunExchange(*config, market_publisher);
  }
  const std::size_t next = options->next_;
  const std::string_view replica_text{args[next]};
  std::uint8_t replica = 0;
  const auto parsed =
      std::from_chars(replica_text.data(), replica_text.data() + replica_text.size(), replica);
  if (parsed.ec != std::errc{} || parsed.ptr != replica_text.data() + replica_text.size() ||
      replica >= config->replicas_.size())
  {
    std::println(stderr, "mex_exchange: the replica number must be 0 to {}",
                 config->replicas_.size() - 1);
    return kExitUsage;
  }
  if (const auto installed = InstallStopHandler(); !installed.has_value())
  {
    std::println(stderr, "mex_exchange: sigaction: {}", std::strerror(installed.error()));
    return kExitFailure;
  }
  return mex::RunReplicatedExchange(*config, {.replica_ = replica, .data_path_ = args[next + 1]},
                                    stop_requested, market_publisher);
}
