#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <span>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <vector>

namespace
{

constexpr std::int64_t kSecondNs = 1'000'000'000;
constexpr std::int64_t kTickNs = 250'000;
constexpr std::int64_t kBurstStartNs = 400'000'000;
constexpr std::int64_t kBurstEndNs = 600'000'000;
constexpr std::size_t kBurstFraction = 20;
constexpr mex::wire::SymbolId kFirstSymbol = 101;
constexpr std::int64_t kWorkerCount = 3;
constexpr int kOrderPrefixShift = 32;
constexpr std::size_t kReceiveBytes = 512;
constexpr int kEventLimit = 256;
constexpr std::int64_t kDrainTimeoutNs = 2 * kSecondNs;
constexpr std::int64_t kPendingLimit = 2'000'000;
constexpr int kInvalidReport = 1;
constexpr int kSocketError = 2;
constexpr int kTimeout = 3;
constexpr int kMissingReport = 4;
constexpr int kSystemBusy = 5;
constexpr int kPendingOverflow = 6;

struct Options
{
  std::int64_t start_ns_;
  std::int64_t seconds_;
  std::int64_t base_rate_;
  std::int64_t extra_rate_;
  std::int64_t worker_;
  std::int64_t hot_symbol_;
};

struct Metrics
{
  std::int64_t base_orders_ = 0;
  std::int64_t burst_orders_ = 0;
  std::int64_t burst_subset_ = 0;
  std::int64_t min_orders_ = 0;
  std::int64_t send_lag_ns_ = 0;
  std::int64_t scheduled_ns_ = 0;
  std::int64_t steady_rtt_ns_ = 0;
  std::int64_t burst_rtt_ns_ = 0;
  std::int64_t duration_ns_ = 0;
};

struct Stamp
{
  std::int64_t due_ns_;
  std::int64_t sent_ns_;
  bool burst_;
};

struct Session
{
  int fd_;
  mex::wire::SymbolId symbol_;
  mex::wire::ClientOrderId order_prefix_;
  std::uint64_t sent_ = 0;
  std::uint64_t received_ = 0;
  std::size_t partial_size_ = 0;
  std::array<std::byte, mex::wire::kMaxFrameSize> partial_{};
  std::deque<Stamp> pending_{};
};

[[nodiscard]] std::int64_t NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Divide cumulative operation counts so rate rounding cannot lose requests.
[[nodiscard]] constexpr std::int64_t WorkerShare(std::int64_t p_total, std::int64_t p_worker)
{
  return (p_total + kWorkerCount - 1 - p_worker) / kWorkerCount;
}

[[nodiscard]] constexpr mex::wire::ClientOrderId ClientOrderIdFor(const Session &p_session,
                                                                  std::uint64_t p_ordinal)
{
  return p_session.order_prefix_ | p_ordinal;
}

[[nodiscard]] std::expected<void, int> Send(Session &p_session, Stamp p_stamp, Metrics &p_metrics)
{
  const std::uint64_t ordinal = p_session.sent_ + 1;
  std::array<std::byte, mex::wire::kMaxFrameSize> frame{};
  const std::size_t size =
      ordinal % 2 != 0
          ? mex::wire::Encode(
                mex::wire::NewOrder{.request_number_ = {ordinal},
                                    .client_order_id_ = ClientOrderIdFor(p_session, ordinal),
                                    .symbol_id_ = p_session.symbol_,
                                    .side_ = mex::Side::Buy,
                                    .type_ = mex::wire::OrderType::Limit,
                                    .price_ = {100},
                                    .qty_ = {1}},
                frame)
          : mex::wire::Encode(
                mex::wire::Cancel{.request_number_ = {ordinal},
                                  .client_order_id_ = ClientOrderIdFor(p_session, ordinal - 1),
                                  .symbol_id_ = p_session.symbol_},
                frame);
  p_stamp.sent_ns_ = NowNs();
  if (::send(p_session.fd_, frame.data(), size, MSG_NOSIGNAL) != static_cast<ssize_t>(size))
  {
    return std::unexpected(kSocketError);
  }
  p_session.pending_.push_back(p_stamp);
  p_session.sent_ = ordinal;
  p_metrics.send_lag_ns_ = std::max(p_metrics.send_lag_ns_, p_stamp.sent_ns_ - p_stamp.due_ns_);
  return {};
}

[[nodiscard]] bool ValidReport(const Session &p_session, const mex::wire::ExchangeMessage &p_report)
{
  const std::uint64_t ordinal = p_session.received_ + 1;
  if (ordinal % 2 != 0)
  {
    const auto *const ack = std::get_if<mex::wire::Ack>(&p_report);
    return ack != nullptr && ack->client_order_id_ == ClientOrderIdFor(p_session, ordinal) &&
           ack->symbol_id_ == p_session.symbol_ && ack->leaves_.units_ == 1;
  }
  const auto *const ack = std::get_if<mex::wire::CancelAck>(&p_report);
  return ack != nullptr && ack->client_order_id_ == ClientOrderIdFor(p_session, ordinal - 1) &&
         ack->symbol_id_ == p_session.symbol_;
}

[[nodiscard]] std::expected<std::int64_t, int> Receive(Session &p_session, Metrics &p_metrics)
{
  std::array<std::byte, kReceiveBytes> bytes{};
  std::copy_n(p_session.partial_.begin(), p_session.partial_size_, bytes.begin());
  const auto tail = std::span{bytes}.subspan(p_session.partial_size_);
  const ssize_t received = ::recv(p_session.fd_, tail.data(), tail.size(), 0);
  if (received <= 0)
  {
    return std::unexpected(kSocketError);
  }
  const std::int64_t arrived_ns = NowNs();
  auto unread = std::span<const std::byte>{bytes}.first(p_session.partial_size_ +
                                                        static_cast<std::size_t>(received));
  std::int64_t count = 0;
  while (!unread.empty())
  {
    const auto decoded = mex::wire::DecodeExchangeFrame(unread);
    if (decoded.status_ == mex::wire::DecodeStatus::NeedMore)
    {
      break;
    }
    const auto *const reject = std::get_if<mex::wire::Reject>(&decoded.message_);
    if (decoded.status_ == mex::wire::DecodeStatus::Complete && reject != nullptr &&
        reject->reason_ == mex::wire::Reason::SystemBusy)
    {
      return std::unexpected(kSystemBusy);
    }
    if (decoded.status_ != mex::wire::DecodeStatus::Complete || p_session.pending_.empty() ||
        decoded.seq_ != p_session.received_ + 2 || !ValidReport(p_session, decoded.message_))
    {
      return std::unexpected(kInvalidReport);
    }
    const Stamp stamp = p_session.pending_.front();
    p_session.pending_.pop_front();
    p_metrics.scheduled_ns_ = std::max(p_metrics.scheduled_ns_, arrived_ns - stamp.due_ns_);
    auto &rtt_ns = stamp.burst_ ? p_metrics.burst_rtt_ns_ : p_metrics.steady_rtt_ns_;
    rtt_ns = std::max(rtt_ns, arrived_ns - stamp.sent_ns_);
    ++p_session.received_;
    ++count;
    unread = unread.subspan(decoded.frame_size_);
  }
  if (unread.size() > p_session.partial_.size())
  {
    return std::unexpected(kInvalidReport);
  }
  std::ranges::copy(unread, p_session.partial_.begin());
  p_session.partial_size_ = unread.size();
  return count;
}

[[nodiscard]] std::expected<void, int> SendTick(std::span<Session> p_sessions,
                                                const Options &p_options, std::int64_t p_tick_ns,
                                                Metrics &p_metrics)
{
  const std::int64_t elapsed_ns = p_tick_ns - p_options.start_ns_;
  const std::int64_t phase_ns = elapsed_ns % kSecondNs;
  const bool burst =
      p_options.extra_rate_ > 0 && kBurstStartNs <= phase_ns && phase_ns < kBurstEndNs;
  const auto count = static_cast<std::int64_t>(p_sessions.size());
  const std::int64_t offered_base = (elapsed_ns + kTickNs) * p_options.base_rate_ / kSecondNs;
  const std::int64_t target = WorkerShare(offered_base, p_options.worker_);
  for (; p_metrics.base_orders_ < target; ++p_metrics.base_orders_)
  {
    const auto index = static_cast<std::size_t>(p_metrics.base_orders_ % count);
    if (const auto sent = Send(p_sessions[index], {p_tick_ns, 0, burst}, p_metrics); !sent)
    {
      return sent;
    }
  }
  if (burst)
  {
    const std::int64_t offset_ns = phase_ns - kBurstStartNs;
    const std::int64_t epoch = elapsed_ns / kSecondNs;
    const std::int64_t worker =
        (p_options.worker_ + kWorkerCount - epoch % kWorkerCount) % kWorkerCount;
    // Carry rate fractions through successive burst windows.
    const std::int64_t burst_start_ns = epoch * (kBurstEndNs - kBurstStartNs);
    const std::int64_t burst_elapsed_ns = burst_start_ns + offset_ns;
    const std::int64_t initial =
        WorkerShare(burst_start_ns * p_options.extra_rate_ / kSecondNs, worker);
    const std::int64_t before =
        WorkerShare(burst_elapsed_ns * p_options.extra_rate_ / kSecondNs, worker) - initial;
    const std::int64_t after =
        WorkerShare((burst_elapsed_ns + kTickNs) * p_options.extra_rate_ / kSecondNs, worker) -
        initial;
    const std::int64_t first = (epoch * p_metrics.burst_subset_) % count;
    for (std::int64_t ordinal = before; ordinal < after; ++ordinal)
    {
      const auto index =
          static_cast<std::size_t>((first + ordinal % p_metrics.burst_subset_) % count);
      if (const auto sent = Send(p_sessions[index], {p_tick_ns, 0, true}, p_metrics); !sent)
      {
        return sent;
      }
      ++p_metrics.burst_orders_;
    }
  }
  return {};
}

[[nodiscard]] std::expected<std::int64_t, int> ReadReady(std::span<Session *const> p_by_fd,
                                                         int p_epoll, Metrics &p_metrics)
{
  std::array<epoll_event, kEventLimit> events{};
  const int count = ::epoll_wait(p_epoll, events.data(), kEventLimit, 0);
  if (count < 0)
  {
    return std::unexpected(kSocketError);
  }
  std::int64_t received = 0;
  for (const auto &event : std::span{events}.first(static_cast<std::size_t>(count)))
  {
    const auto fd = static_cast<std::size_t>(event.data.fd);
    if (fd >= p_by_fd.size() || p_by_fd[fd] == nullptr ||
        (event.events & (EPOLLERR | EPOLLHUP)) != 0)
    {
      return std::unexpected(kSocketError);
    }
    const auto reports = Receive(*p_by_fd[fd], p_metrics);
    if (!reports)
    {
      return std::unexpected(reports.error());
    }
    received += *reports;
  }
  return received;
}

[[nodiscard]] std::expected<Metrics, int> Run(std::span<Session> p_sessions,
                                              std::span<Session *const> p_by_fd, int p_epoll,
                                              const Options &p_options)
{
  Metrics result{};
  if (p_options.extra_rate_ > 0)
  {
    result.burst_subset_ =
        static_cast<std::int64_t>(std::max(std::size_t{1}, p_sessions.size() / kBurstFraction));
  }
  std::int64_t next_tick_ns = p_options.start_ns_;
  const std::int64_t finish_ns = p_options.start_ns_ + p_options.seconds_ * kSecondNs;
  std::int64_t received = 0;
  while (next_tick_ns < finish_ns || received < result.base_orders_ + result.burst_orders_)
  {
    const std::int64_t now_ns = NowNs();
    if (now_ns > finish_ns + kDrainTimeoutNs)
    {
      return std::unexpected(kTimeout);
    }
    if (result.base_orders_ + result.burst_orders_ - received > kPendingLimit)
    {
      return std::unexpected(kPendingOverflow);
    }
    if (next_tick_ns < finish_ns && now_ns >= next_tick_ns)
    {
      if (const auto tick = SendTick(p_sessions, p_options, next_tick_ns, result); !tick)
      {
        return std::unexpected(tick.error());
      }
      next_tick_ns += kTickNs;
    }
    const auto ready = ReadReady(p_by_fd, p_epoll, result);
    if (!ready)
    {
      return std::unexpected(ready.error());
    }
    received += *ready;
  }

  result.min_orders_ = result.base_orders_ + result.burst_orders_;
  for (const auto &session : p_sessions)
  {
    if (session.received_ == 0 || session.received_ != session.sent_)
    {
      return std::unexpected(kMissingReport);
    }
    result.min_orders_ = std::min(result.min_orders_, static_cast<std::int64_t>(session.received_));
  }
  result.duration_ns_ = std::max(finish_ns, NowNs()) - p_options.start_ns_;
  return result;
}

} // namespace

// Python keeps the owning sockets open and supplies an epoll descriptor.
extern "C" int MexActiveLoop(const int *p_fds, std::size_t p_count, int p_epoll,
                             const Options *p_options, Metrics *p_result)
{
  const std::span<const int> fds{p_fds, p_count};
  std::vector<Session> sessions;
  sessions.reserve(fds.size());
  for (const int fd : fds)
  {
    const auto index = static_cast<std::size_t>(p_options->worker_) +
                       static_cast<std::size_t>(kWorkerCount) * sessions.size();
    const auto symbol_offset =
        static_cast<mex::wire::SymbolId>(p_options->hot_symbol_ != 0 ? 0 : index % 2);
    const auto prefix = (static_cast<mex::wire::ClientOrderId>(index) + 1) << kOrderPrefixShift;
    sessions.emplace_back(fd, kFirstSymbol + symbol_offset, prefix);
  }
  std::vector<Session *> by_fd(static_cast<std::size_t>(*std::ranges::max_element(fds)) + 1);
  for (auto &session : sessions)
  {
    by_fd[static_cast<std::size_t>(session.fd_)] = &session;
  }
  const auto run = Run(sessions, by_fd, p_epoll, *p_options);
  if (!run)
  {
    return run.error();
  }
  *p_result = *run;
  return 0;
}
