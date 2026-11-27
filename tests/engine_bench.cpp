// Measures the engine thread alone: the time from a command in the inbound
// queue to its reports in the outbound queue, with the order mix of
// mex_loadgen. The end-to-end tests cannot show this cost, because the engine
// thread spins while it waits and the load generators saturate first.
#include "config.hpp"
#include "engine.hpp"
#include "market_data.hpp"
#include "spsc_queue.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string_view>

namespace mex
{
namespace
{
using Clock = std::chrono::steady_clock;

// The mex_loadgen mix (tools/loadgen.cpp, OrderFlow), with a cheaper generator.
constexpr std::uint32_t kReplacePercent = 20;
constexpr std::uint32_t kCancelPercent = 30;
constexpr std::int64_t kMid = 10'000;
constexpr std::int64_t kMaxDepthTicks = 20;
constexpr std::int64_t kCrossTicks = 3;
constexpr std::int64_t kReplaceStepTicks = 2;
constexpr std::uint32_t kMaxQty = 100;
constexpr std::size_t kMaxLiveOrders = 1024;

constexpr wire::SymbolId kSymbol = 1;
constexpr std::uint32_t kMaxPriceLevels = 4096;
constexpr std::size_t kQueueCapacity = 1024;
constexpr std::uint64_t kDefaultCommands = 4'000'000;
constexpr std::uint64_t kMaxCommands = 1'000'000'000;
constexpr std::size_t kRepeats = 5;
// The small index fits in L2. The large one is the size that the end-to-end
// tests configure, so its entries miss the cache as they do in the exchange.
constexpr std::array<std::uint32_t, 2> kMaxOrders{4096, 1'000'000};

class OrderFlow final
{
public:
  [[nodiscard]] ClientOrder Next()
  {
    const std::uint32_t roll = Below(100);
    if (live_count_ > 0 && roll < kReplacePercent)
    {
      LiveOrder &order = live_[(live_begin_ + Below(live_count_)) % kMaxLiveOrders];
      order.price_ = Clamp(order.price_.ticks_ + Between(-kReplaceStepTicks, kReplaceStepTicks));
      return wire::Replace{.request_number_ = NextRequest(),
                           .client_order_id_ = order.client_order_id_,
                           .symbol_id_ = kSymbol,
                           .new_price_ = order.price_,
                           .new_total_qty_ = Quantity{1 + Below(kMaxQty)}};
    }
    if (live_count_ == kMaxLiveOrders ||
        (live_count_ > 0 && roll < kReplacePercent + kCancelPercent))
    {
      const LiveOrder oldest = live_[live_begin_];
      live_begin_ = (live_begin_ + 1) % kMaxLiveOrders;
      --live_count_;
      return wire::Cancel{.request_number_ = NextRequest(),
                          .client_order_id_ = oldest.client_order_id_,
                          .symbol_id_ = kSymbol};
    }
    const Side side = Below(2) == 0 ? Side::Buy : Side::Sell;
    const std::int64_t away = Between(-kCrossTicks, kMaxDepthTicks); // negative crosses mid
    const wire::NewOrder order{.request_number_ = NextRequest(),
                               .client_order_id_ = ++last_client_order_id_,
                               .symbol_id_ = kSymbol,
                               .side_ = side,
                               .type_ = wire::OrderType::Limit,
                               .price_ = Clamp(side == Side::Buy ? kMid - away : kMid + away),
                               .qty_ = Quantity{1 + Below(kMaxQty)}};
    live_[(live_begin_ + live_count_) % kMaxLiveOrders] = {order.client_order_id_, order.price_};
    ++live_count_;
    return order;
  }

private:
  struct LiveOrder
  {
    wire::ClientOrderId client_order_id_ = 0;
    Price price_{};
  };

  // xorshift64: a few cycles, so the generator adds little to each command.
  [[nodiscard]] std::uint32_t Below(std::size_t p_bound)
  {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<std::uint32_t>(state_ % p_bound);
  }

  [[nodiscard]] std::int64_t Between(std::int64_t p_low, std::int64_t p_high)
  {
    return p_low + static_cast<std::int64_t>(Below(static_cast<std::size_t>(p_high - p_low + 1)));
  }

  [[nodiscard]] static Price Clamp(std::int64_t p_ticks)
  {
    return Price{std::clamp(p_ticks, kMid - kMaxDepthTicks, kMid + kMaxDepthTicks)};
  }

  [[nodiscard]] wire::RequestNumber NextRequest()
  {
    ++last_request_.value_;
    return last_request_;
  }

  std::uint64_t state_ = 0x9e3779b97f4a7c15ULL;
  std::array<LiveOrder, kMaxLiveOrders> live_{};
  std::size_t live_begin_ = 0;
  std::size_t live_count_ = 0;
  wire::ClientOrderId last_client_order_id_ = 0;
  wire::RequestNumber last_request_{};
};

// One engine with one book and one session, fed from this thread.
class EngineRig final
{
public:
  EngineRig(std::uint32_t p_max_orders, MarketDataPublisher *p_market_data)
      : books_{BookConfig{kSymbol, p_max_orders, kMaxPriceLevels}},
        engine_{std::make_unique<Engine>(books_, 1, inbound_, outbound_)}
  {
    if (p_market_data != nullptr)
    {
      engine_->SetMarketData(*p_market_data);
    }
  }

  // Returns false if the engine did not take a command.
  [[nodiscard]] bool Run(std::uint64_t p_commands)
  {
    for (std::uint64_t count = 0; count < p_commands; ++count)
    {
      const Command command{.op_number_ = OpNumber{++last_op_},
                            .session_id_ = 0,
                            .symbol_slot_ = 0,
                            .order_ = flow_.Next()};
      if (!inbound_.TryPush(command) || !engine_->ProcessOne())
      {
        return false;
      }
      while (outbound_.TryPop().has_value())
      {
      }
    }
    return true;
  }

private:
  std::array<BookConfig, 1> books_;
  SpscQueue<Command> inbound_{kQueueCapacity};
  SpscQueue<Report> outbound_{kQueueCapacity};
  std::unique_ptr<Engine> engine_;
  OrderFlow flow_{};
  std::uint64_t last_op_ = 0;
};

// The median over kRepeats fresh engines, in nanoseconds for each command.
[[nodiscard]] std::optional<double>
Measure(std::uint32_t p_max_orders, MarketDataPublisher *p_market_data, std::uint64_t p_commands)
{
  std::array<double, kRepeats> results{};
  for (double &result : results)
  {
    EngineRig rig{p_max_orders, p_market_data};
    // Warm up until the live orders reach their limit and the caches settle.
    if (!rig.Run(p_commands / 4))
    {
      return std::nullopt;
    }
    const auto start = Clock::now();
    if (!rig.Run(p_commands))
    {
      return std::nullopt;
    }
    const std::chrono::duration<double, std::nano> elapsed = Clock::now() - start;
    result = elapsed.count() / static_cast<double>(p_commands);
  }
  std::ranges::sort(results);
  return results[kRepeats / 2];
}

[[nodiscard]] std::optional<std::uint64_t> ParseCommands(std::string_view p_text)
{
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(p_text.begin(), p_text.end(), value);
  if (error != std::errc{} || end != p_text.end() || value < 4 || value > kMaxCommands)
  {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] int Main(std::span<char *const> p_args)
{
  const std::optional<std::uint64_t> commands =
      p_args.size() > 1 ? ParseCommands(p_args[1]) : kDefaultCommands;
  if (p_args.size() > 2 || !commands.has_value())
  {
    std::println(stderr, "usage: bench_engine [commands, 4 to {}]", kMaxCommands);
    return EXIT_FAILURE;
  }
  Config config{};
  config.symbols_.push_back(
      SymbolConfig{.symbol_id_ = kSymbol, .min_price_ = Price{1}, .max_price_ = Price{2 * kMid}});
  config.gateway_.listen_address_ = "127.0.0.1";
  // Port 0: the kernel picks a free port. No thread reads the feed, so this
  // measures only the work that market data adds to the engine thread.
  auto publisher = MarketDataPublisher::Create(config, 0);
  if (!publisher.has_value())
  {
    std::println(stderr, "bench_engine: {}", publisher.error());
    return EXIT_FAILURE;
  }
  std::println("{} commands, median of {} runs", *commands, kRepeats);
  for (const std::uint32_t max_orders : kMaxOrders)
  {
    for (MarketDataPublisher *const market_data :
         {static_cast<MarketDataPublisher *>(nullptr), &*publisher})
    {
      const std::optional<double> ns = Measure(max_orders, market_data, *commands);
      if (!ns.has_value())
      {
        std::println(stderr, "bench_engine: the engine did not take a command");
        return EXIT_FAILURE;
      }
      std::println("max_orders {:>9}  market_data {:<3}  {:7.1f} ns/command", max_orders,
                   market_data != nullptr ? "on" : "off", *ns);
    }
  }
  return EXIT_SUCCESS;
}

} // namespace
} // namespace mex

int main(int p_argc, char **p_argv)
{
  return mex::Main(std::span<char *const>{p_argv, static_cast<std::size_t>(p_argc)});
}
