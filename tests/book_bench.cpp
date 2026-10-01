#include "book.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t Nanoseconds(Clock::duration p_elapsed)
{
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(p_elapsed).count());
}

// Throughput pass: no clock calls inside the loop, so ops/s is not diluted by timer cost.
struct NoTiming
{
  [[nodiscard]] static std::vector<std::uint64_t> MakeSamples(std::size_t) { return {}; }

  static auto Op(std::vector<std::uint64_t> &, std::size_t, auto &&p_op) { return p_op(); }
};

// Latency pass: one steady_clock sample per operation. The result check stays outside
// the sample. Sample storage is sized before the timed loop.
struct PerOpTiming
{
  [[nodiscard]] static std::vector<std::uint64_t> MakeSamples(std::size_t p_count)
  {
    return std::vector<std::uint64_t>(p_count);
  }

  static auto Op(std::vector<std::uint64_t> &p_samples, std::size_t p_index, auto &&p_op)
  {
    const Clock::time_point start = Clock::now();
    auto result = p_op();
    p_samples[p_index] = Nanoseconds(Clock::now() - start);
    return result;
  }
};

struct Result
{
  std::uint64_t operations_ = 0;
  Clock::duration elapsed_{};
  std::vector<std::uint64_t> samples_ns_;
};

[[nodiscard]] std::uint64_t PercentileNs(const std::vector<std::uint64_t> &p_sorted_ns,
                                         std::uint32_t p_percent)
{
  if (p_sorted_ns.empty())
  {
    return 0;
  }
  const std::size_t index = (static_cast<std::size_t>(p_percent) * (p_sorted_ns.size() - 1)) / 100U;
  return p_sorted_ns[index];
}

[[nodiscard]] Clock::duration Time(auto &&p_function)
{
  const Clock::time_point start = Clock::now();
  p_function();
  return Clock::now() - start;
}

template <typename Timing> [[nodiscard]] Result BenchAppendToOneLevel()
{
  constexpr std::uint32_t kOrders = 200000;
  mex::Book book(kOrders, 4);
  std::vector<std::uint64_t> samples = Timing::MakeSamples(kOrders);
  const Clock::duration elapsed = Time(
      [&]()
      {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
          const mex::SubmitResult result = Timing::Op(
              samples, i, [&]()
              { return book.SubmitLimitOrder(mex::Side::Buy, mex::Price{100}, mex::Quantity{1}); });
          if (result.status_ != mex::SubmitStatus::Accepted)
          {
            std::cerr << "append rejected\n";
            std::exit(1);
          }
        }
      });
  return Result{kOrders, elapsed, std::move(samples)};
}

template <typename Timing> [[nodiscard]] Result BenchRestAcrossLevels()
{
  constexpr std::uint32_t kOrders = 200000;
  constexpr std::uint32_t kLevels = 64;
  mex::Book book(kOrders, kLevels);
  std::vector<std::uint64_t> samples = Timing::MakeSamples(kOrders);
  const Clock::duration elapsed = Time(
      [&]()
      {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
          const mex::Price price{static_cast<std::int64_t>(i % kLevels)};
          const mex::SubmitResult result = Timing::Op(
              samples, i,
              [&]() { return book.SubmitLimitOrder(mex::Side::Buy, price, mex::Quantity{1}); });
          if (result.status_ != mex::SubmitStatus::Accepted)
          {
            std::cerr << "rest rejected\n";
            std::exit(1);
          }
        }
      });
  return Result{kOrders, elapsed, std::move(samples)};
}

template <typename Timing> [[nodiscard]] Result BenchCancel()
{
  constexpr std::uint32_t kOrders = 100000;
  mex::Book book(kOrders, 32);
  std::vector<mex::OrderId> ids;
  ids.reserve(kOrders);
  for (std::uint32_t i = 0; i < kOrders; ++i)
  {
    const mex::Price price{1000 + static_cast<std::int64_t>(i % 32)};
    const mex::SubmitResult result =
        book.SubmitLimitOrder(mex::Side::Sell, price, mex::Quantity{1});
    ids.push_back(result.order_id_);
  }

  std::uint64_t state = 0x123456789abcdefULL;
  auto next = [&state]()
  {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
  };
  for (std::uint32_t i = kOrders - 1; i > 0; --i)
  {
    const std::uint32_t swap_with =
        static_cast<std::uint32_t>(next() % (static_cast<std::uint64_t>(i) + 1));
    std::swap(ids[i], ids[swap_with]);
  }

  std::vector<std::uint64_t> samples = Timing::MakeSamples(kOrders);
  const Clock::duration elapsed = Time(
      [&]()
      {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
          const bool cancelled = Timing::Op(samples, i, [&]() { return book.CancelOrder(ids[i]); });
          if (!cancelled)
          {
            std::cerr << "cancel failed\n";
            std::exit(1);
          }
        }
      });
  return Result{kOrders, elapsed, std::move(samples)};
}

template <typename Timing> [[nodiscard]] Result BenchTakeBest()
{
  constexpr std::uint32_t kOrders = 100000;
  constexpr std::uint32_t kLevels = 32;
  mex::Book book(kOrders, kLevels);
  for (std::uint32_t i = 0; i < kOrders; ++i)
  {
    const mex::Price price{10 + static_cast<std::int64_t>(i % kLevels)};
    const mex::SubmitResult result =
        book.SubmitLimitOrder(mex::Side::Sell, price, mex::Quantity{1});
    if (result.order_id_ == mex::kInvalidOrderId)
    {
      std::cerr << "seed ask rejected\n";
      std::exit(1);
    }
  }

  std::vector<std::uint64_t> samples = Timing::MakeSamples(kOrders);
  const Clock::duration elapsed = Time(
      [&]()
      {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
          const mex::SubmitResult result = Timing::Op(
              samples, i,
              [&]() {
                return book.SubmitLimitOrder(mex::Side::Buy, mex::Price{1000}, mex::Quantity{1});
              });
          if (result.filled_qty_ != mex::Quantity{1} || result.order_id_ != mex::kInvalidOrderId)
          {
            std::cerr << "take did not fill one lot\n";
            std::exit(1);
          }
        }
      });
  return Result{kOrders, elapsed, std::move(samples)};
}

template <typename Timing> [[nodiscard]] Result BenchMixedBook()
{
  constexpr std::uint32_t kCapacity = 100000;
  constexpr std::uint32_t kLevels = 64;
  constexpr std::uint32_t kOps = 300000;
  mex::Book book(kCapacity, kLevels);
  std::vector<mex::OrderId> live;
  live.reserve(kCapacity);

  std::uint64_t state = 0xcafef00d1234ULL;
  auto next = [&state]()
  {
    state ^= state << 7U;
    state ^= state >> 9U;
    state ^= state << 8U;
    return state;
  };

  std::vector<std::uint64_t> samples = Timing::MakeSamples(kOps);
  const Clock::duration elapsed = Time(
      [&]()
      {
        for (std::uint32_t i = 0; i < kOps; ++i)
        {
          const bool full = live.size() >= kCapacity;
          const bool cancel = !live.empty() && (full || (next() % 5U) == 0U);
          if (cancel)
          {
            const std::size_t index = static_cast<std::size_t>(next() % live.size());
            const bool cancelled =
                Timing::Op(samples, i, [&]() { return book.CancelOrder(live[index]); });
            if (!cancelled)
            {
              std::cerr << "mixed cancel failed\n";
              std::exit(1);
            }
            live[index] = live.back();
            live.pop_back();
            continue;
          }

          const mex::Price price{static_cast<std::int64_t>(next() % kLevels)};
          const mex::SubmitResult result = Timing::Op(
              samples, i,
              [&]() { return book.SubmitLimitOrder(mex::Side::Buy, price, mex::Quantity{1}); });
          if (result.order_id_ == mex::kInvalidOrderId)
          {
            std::cerr << "mixed insert rejected\n";
            std::exit(1);
          }
          live.push_back(result.order_id_);
        }
      });
  return Result{kOps, elapsed, std::move(samples)};
}

// Each workload runs twice on a fresh book: once without per-op clock calls for
// throughput, once with them for the latency percentiles.
void Run(std::string_view p_name, Result (*p_throughput)(), Result (*p_latency)())
{
  const Result throughput = p_throughput();
  Result latency = p_latency();

  const double seconds = std::chrono::duration<double>(throughput.elapsed_).count();
  const double per_second = static_cast<double>(throughput.operations_) / seconds;
  std::sort(latency.samples_ns_.begin(), latency.samples_ns_.end());
  const std::uint64_t max_ns = latency.samples_ns_.empty() ? 0 : latency.samples_ns_.back();
  std::cout << p_name << "  " << static_cast<std::uint64_t>(per_second) << " ops/s  p50 "
            << PercentileNs(latency.samples_ns_, 50) << " ns  p99 "
            << PercentileNs(latency.samples_ns_, 99) << " ns  max " << max_ns << " ns\n";
}

} // namespace

int main()
{
  std::cout << "Order book benchmark, compiled optimized\n";
  std::cout << "ops/s: timed loop with no per-op clock calls. p50/p99/max: separate pass, one\n"
               "steady_clock sample per op (includes timer cost).\n";
  Run("append one price level", BenchAppendToOneLevel<NoTiming>,
      BenchAppendToOneLevel<PerOpTiming>);
  Run("rest across 64 levels", BenchRestAcrossLevels<NoTiming>, BenchRestAcrossLevels<PerOpTiming>);
  Run("cancel random live orders", BenchCancel<NoTiming>, BenchCancel<PerOpTiming>);
  Run("take one lot from the touch", BenchTakeBest<NoTiming>, BenchTakeBest<PerOpTiming>);
  Run("mixed insert and cancel", BenchMixedBook<NoTiming>, BenchMixedBook<PerOpTiming>);
  std::cout << "Target from the design spec: 100000 to 500000 orders/s\n";
  return 0;
}
