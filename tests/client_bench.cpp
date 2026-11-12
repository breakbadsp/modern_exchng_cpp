#include "book.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace mex
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t kSteps = 8;
constexpr std::uint64_t kWarmupCycles = 20000;
constexpr std::uint64_t kMaxBatchCycles = 100000000;
constexpr std::uint64_t kMaxLatencyCycles = 1000000;
constexpr std::uint64_t kMaxLevels = 4096;
constexpr std::int64_t kBestBid = 9000;
constexpr std::int64_t kTouch = 10000;
constexpr std::int64_t kBestAsk = 11000;
constexpr std::array<std::string_view, kSteps> kNames{
    "rest_existing", "cancel",      "insert_touch", "partial_limit",
    "market_buy",    "market_sell", "refill_bid",   "reject_cancel"};

struct FillTotals
{
  std::uint64_t count_{};
  std::uint64_t quantity_{};
  std::uint64_t notional_{};
};

class ClientFlow final
{
public:
  explicit ClientFlow(std::uint32_t p_levels)
      : book_(2 * p_levels + 16, p_levels + 1), levels_(p_levels)
  {
    for (std::uint32_t index = 0; index < p_levels; ++index)
    {
      Consume(book_.SubmitLimitOrder(Side::Buy, Price{kBestBid - index}, Quantity{1}));
      Consume(book_.SubmitLimitOrder(Side::Sell, Price{kBestAsk + index}, Quantity{1}));
    }
  }

  void Step(std::size_t p_step, std::uint64_t p_cycle)
  {
    switch (p_step)
    {
    case 0:
      resting_id_ = Consume(book_.SubmitLimitOrder(
          Side::Buy, Price{kBestBid - static_cast<std::int64_t>(p_cycle % levels_)}, Quantity{2}));
      break;
    case 1:
      cancelled_ += book_.CancelOrder(resting_id_);
      break;
    case 2:
      Consume(book_.SubmitLimitOrder(Side::Sell, Price{kTouch}, Quantity{3}));
      break;
    case 3:
      Consume(book_.SubmitLimitOrder(Side::Buy, Price{kTouch}, Quantity{1}));
      break;
    case 4:
      Consume(book_.SubmitMarketOrder(Side::Buy, Quantity{2}));
      break;
    case 5:
      Consume(book_.SubmitMarketOrder(Side::Sell, Quantity{1}));
      break;
    case 6:
      Consume(book_.SubmitLimitOrder(Side::Buy, Price{kBestBid}, Quantity{1}));
      break;
    case 7:
      rejected_ += !book_.CancelOrder(kInvalidOrderId);
      break;
    }
  }

  // Each cycle: fills of 1 and 2 at kTouch, then 1 at kBestBid.
  [[nodiscard]] bool Valid(std::uint64_t p_cycles) const
  {
    return accepted_ == 2 * levels_ + 6 * p_cycles && cancelled_ == p_cycles &&
           rejected_ == p_cycles && totals_.count_ == 3 * p_cycles &&
           totals_.quantity_ == 4 * p_cycles &&
           totals_.notional_ == static_cast<std::uint64_t>(3 * kTouch + kBestBid) * p_cycles &&
           book_.Bids().size() == levels_ && book_.Asks().size() == levels_ &&
           book_.FreeSlotCount() == 16 && book_.Bids().back().total_qty_ == 1 &&
           book_.Asks().back().total_qty_ == 1;
  }

  [[nodiscard]] std::uint64_t Checksum() const { return checksum_ + totals_.notional_; }

private:
  // A client reads the fills of each submit before the next submit clears them.
  OrderId Consume(SubmitResult p_result)
  {
    accepted_ += p_result.status_ == SubmitStatus::Accepted;
    checksum_ += p_result.filled_qty_.units_ + p_result.remaining_.units_;
    for (const Fill &fill : book_.Fills())
    {
      ++totals_.count_;
      totals_.quantity_ += fill.quantity_.units_;
      totals_.notional_ += static_cast<std::uint64_t>(fill.price_.ticks_) * fill.quantity_.units_;
    }
    return p_result.order_id_;
  }

  Book book_;
  std::uint32_t levels_;
  FillTotals totals_{};
  std::uint64_t accepted_{};
  std::uint64_t cancelled_{};
  std::uint64_t rejected_{};
  std::uint64_t checksum_{};
  OrderId resting_id_ = kInvalidOrderId;
};

[[nodiscard]] std::optional<std::uint64_t> ParsePositive(std::string_view p_text)
{
  std::uint64_t value{};
  const auto result = std::from_chars(p_text.data(), p_text.data() + p_text.size(), value);
  if (result.ec != std::errc{} || result.ptr != p_text.data() + p_text.size() || value == 0)
  {
    return std::nullopt;
  }
  return value;
}

struct PerfControl
{
  int command_fd_{};
  int acknowledgement_fd_{};
};

[[nodiscard]] std::optional<PerfControl> ParsePerfControl()
{
  const char *const command_text = std::getenv("MEX_PERF_CONTROL_FD");
  const char *const acknowledgement_text = std::getenv("MEX_PERF_ACK_FD");
  if (command_text == nullptr || acknowledgement_text == nullptr)
  {
    return std::nullopt;
  }
  const auto command = ParsePositive(command_text);
  const auto acknowledgement = ParsePositive(acknowledgement_text);
  if (!command || !acknowledgement || *command > std::numeric_limits<int>::max() ||
      *acknowledgement > std::numeric_limits<int>::max())
  {
    return std::nullopt;
  }
  return PerfControl{static_cast<int>(*command), static_cast<int>(*acknowledgement)};
}

[[nodiscard]] std::expected<void, std::string_view> ControlPerf(PerfControl p_control,
                                                                std::string_view p_command)
{
  ssize_t written{};
  do
  {
    written = write(p_control.command_fd_, p_command.data(), p_command.size());
  } while (written < 0 && errno == EINTR);
  if (written < 0 || static_cast<std::size_t>(written) != p_command.size())
  {
    return std::unexpected("Cannot write perf control command");
  }
  // perf includes the terminating null byte in its acknowledgement.
  constexpr std::array kAcknowledgement{'a', 'c', 'k', '\n', '\0'};
  std::array<char, kAcknowledgement.size()> acknowledgement{};
  std::size_t received{};
  while (received < acknowledgement.size())
  {
    const auto count = read(p_control.acknowledgement_fd_, acknowledgement.data() + received,
                            acknowledgement.size() - received);
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      return std::unexpected("Cannot read perf acknowledgement");
    }
    received += static_cast<std::size_t>(count);
  }
  if (acknowledgement != kAcknowledgement)
  {
    return std::unexpected("Invalid perf acknowledgement");
  }
  return {};
}

void Report(std::string_view p_name, std::vector<std::int64_t> &p_samples)
{
  std::sort(p_samples.begin(), p_samples.end());
  const auto percentile = [&](std::size_t p_percent)
  { return p_samples[((p_samples.size() * p_percent + 99) / 100) - 1]; };
  std::println("latency,{},min_ns,{},p50_ns,{},p95_ns,{},p99_ns,{},max_ns,{}", p_name,
               p_samples.front(), percentile(50), percentile(95), percentile(99), p_samples.back());
}

[[nodiscard]] std::optional<rusage> ThreadUsage()
{
  rusage usage{};
  if (getrusage(RUSAGE_THREAD, &usage) != 0)
  {
    return std::nullopt;
  }
  return usage;
}

[[nodiscard]] double CpuTimeNs(timeval p_time)
{
  return static_cast<double>(p_time.tv_sec) * 1e9 + static_cast<double>(p_time.tv_usec) * 1000;
}

int Run(std::span<char *> p_args)
{
  const std::optional<std::uint64_t> cycles =
      p_args.size() == 4 ? ParsePositive(p_args[2]) : std::nullopt;
  const std::optional<std::uint64_t> levels =
      p_args.size() == 4 ? ParsePositive(p_args[3]) : std::nullopt;
  const std::string_view mode = p_args.size() == 4 ? p_args[1] : "";
  const bool measure_latency = mode == "latency";
  const bool measure_perf = mode == "perf";
  const std::uint64_t max_cycles = measure_latency ? kMaxLatencyCycles : kMaxBatchCycles;
  if ((mode != "latency" && mode != "batch" && !measure_perf) || !cycles || !levels ||
      *cycles > max_cycles || *levels > kMaxLevels)
  {
    std::println(stderr, "Usage: bench_client batch|latency|perf CYCLES LEVELS");
    std::println(stderr, "CYCLES: 1..{} (batch), 1..{} (latency). LEVELS: 1..{}.", kMaxBatchCycles,
                 kMaxLatencyCycles, kMaxLevels);
    return 2;
  }
  const auto perf_control = measure_perf ? ParsePerfControl() : std::nullopt;
  if (measure_perf && !perf_control)
  {
    std::println(stderr, "perf mode requires the scripts/perf_control.py control channel");
    return 2;
  }
  ClientFlow flow(static_cast<std::uint32_t>(*levels));
  for (std::uint64_t cycle = 0; cycle < kWarmupCycles; ++cycle)
  {
    for (std::size_t step = 0; step < kSteps; ++step)
    {
      flow.Step(step, cycle);
    }
  }
  std::array<std::vector<std::int64_t>, kSteps> samples;
  if (measure_latency)
  {
    for (auto &values : samples)
    {
      values.resize(*cycles);
    }
  }
  const auto usage_start = ThreadUsage();
  if (!usage_start)
  {
    std::println(stderr, "Cannot read thread CPU usage");
    return 1;
  }
  // Wait for perf to acknowledge the boundary before the measured requests.
  if (measure_perf)
  {
    const auto enabled = ControlPerf(*perf_control, "enable\n");
    if (!enabled)
    {
      std::println(stderr, "{}", enabled.error());
      return 1;
    }
  }
  const auto cpu_start = std::clock();
  const auto start = Clock::now();
  for (std::uint64_t cycle = 0; cycle < *cycles; ++cycle)
  {
    for (std::size_t step = 0; step < kSteps; ++step)
    {
      if (measure_latency)
      {
        const auto before = Clock::now();
        flow.Step(step, cycle);
        const auto after = Clock::now();
        samples[step][cycle] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
      }
      else
      {
        flow.Step(step, cycle);
      }
    }
  }
  const double elapsed_ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  const double cpu_ns = static_cast<double>(std::clock() - cpu_start) * 1e9 / CLOCKS_PER_SEC;
  if (measure_perf)
  {
    const auto disabled = ControlPerf(*perf_control, "disable\n");
    if (!disabled)
    {
      std::println(stderr, "{}", disabled.error());
      return 1;
    }
  }
  const auto usage_end = ThreadUsage();
  if (!usage_end)
  {
    std::println(stderr, "Cannot read thread CPU usage");
    return 1;
  }
  const double user_cpu_ns = CpuTimeNs(usage_end->ru_utime) - CpuTimeNs(usage_start->ru_utime);
  const double system_cpu_ns = CpuTimeNs(usage_end->ru_stime) - CpuTimeNs(usage_start->ru_stime);
  if (!flow.Valid(*cycles + kWarmupCycles))
  {
    std::println(stderr, "Flow validation failed");
    return 1;
  }
  const auto operations = static_cast<double>(*cycles * kSteps);
  std::println("summary,{},levels,{},operations,{},wall_ns_per_op,{:.3f},cpu_ns_per_op,{:.3f},"
               "user_cpu_ns,{:.0f},system_cpu_ns,{:.0f},checksum,{}",
               mode, *levels, *cycles * kSteps, elapsed_ns / operations, cpu_ns / operations,
               user_cpu_ns, system_cpu_ns, flow.Checksum());
  if (measure_latency)
  {
    for (std::size_t step = 0; step < kSteps; ++step)
    {
      Report(kNames[step], samples[step]);
    }
    std::vector<std::int64_t> timer_samples(*cycles);
    for (auto &value : timer_samples)
    {
      const auto before = Clock::now();
      value = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - before).count();
    }
    Report("timer_pair", timer_samples);
  }
  return 0;
}
} // namespace
} // namespace mex

int main(int p_argc, char **p_argv)
{
  return mex::Run(std::span<char *>(p_argv, static_cast<std::size_t>(p_argc)));
}
