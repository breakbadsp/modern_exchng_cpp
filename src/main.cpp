#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
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
#include <print>
#include <pthread.h>
#include <sched.h>
#include <stop_token>
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
  EngineSlot(const mex::EngineConfig &p_config, const mex::GatewayConfig &p_gateway)
      : config_{p_config}, inbound_{p_gateway.inbound_queue_capacity_},
        outbound_{p_gateway.outbound_queue_capacity_}
  {
  }

  const mex::EngineConfig &config_;
  SpscQueue<Command> inbound_;
  SpscQueue<Report> outbound_;
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
  Engine engine{p_slot.config_.books_, p_slot.inbound_, p_slot.outbound_};
  // The callback runs on the thread that requests the stop. The destructor of
  // stop_engine waits for a running callback, so engine outlives it.
  const std::stop_callback stop_engine{p_stop, [&engine] { engine.Stop(); }};
  p_ready.count_down();
  engine.Run();
  p_slot.stalls_ = engine.OutboundStalls();
}

[[nodiscard]] int RunExchange(const Config &p_config)
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
    slots.push_back(std::make_unique<EngineSlot>(engine_config, p_config.gateway_));
    queues.push_back({slots.back()->inbound_, slots.back()->outbound_});
  }

  std::latch engines_ready{static_cast<std::ptrdiff_t>(slots.size())};
  std::vector<std::jthread> engine_threads;
  for (std::size_t index = 0; index < slots.size(); ++index)
  {
    engine_threads.emplace_back(RunEngine, std::ref(*slots[index]), std::ref(engines_ready), index);
  }
  engines_ready.wait();

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

} // namespace

int main(int p_argc, char **p_argv)
{
  if (p_argc != 2)
  {
    std::println(stderr, "usage: mex_exchange <config path>");
    return kExitUsage;
  }
  const auto config = mex::LoadConfig(p_argv[1]);
  if (!config.has_value())
  {
    const mex::ConfigError &error = config.error();
    if (error.line_ == 0)
    {
      std::println(stderr, "mex_exchange: {}: {}", p_argv[1], error.reason_);
    }
    else
    {
      std::println(stderr, "mex_exchange: {}:{}: {}", p_argv[1], error.line_, error.reason_);
    }
    return kExitFailure;
  }
  return RunExchange(*config);
}
