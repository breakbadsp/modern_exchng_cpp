#include "gateway.hpp"
#include "protocol.hpp"
#include "types.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// Load generator for the exchange (prep/io-design-spec.md, step 7). One session
// sends a mix of NewOrder, Cancel, and Replace while a reader thread decodes
// the reports. At the end it prints throughput and NewOrder -> first report
// latency. Request numbers start at 1 on each run and it never retries, so a
// DuplicateRequest reject means that the session was used before; the run fails.
// It is a test tool: it blocks on its socket and is not on the exchange hot path.
namespace
{

namespace wire = mex::wire;
using Clock = std::chrono::steady_clock;
using mex::Price;
using mex::Quantity;
using mex::Side;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// A send or receive that waits longer than this fails the run.
constexpr timeval kSocketTimeout{.tv_sec = 5, .tv_usec = 0};
constexpr std::size_t kReadBufferSize = 1024 * wire::kMaxFrameSize;
constexpr std::size_t kSendBatchFrames = 64;

// Order mix, in percent of operations, while there are live orders to touch.
constexpr std::uint32_t kReplacePercent = 20;
constexpr std::uint32_t kCancelPercent = 30;
// Limit prices: up to kMaxDepthTicks away from mid on the passive side, and up
// to kCrossTicks through mid, so some orders cross.
constexpr std::int64_t kMaxDepthTicks = 20;
constexpr std::int64_t kCrossTicks = 3;
constexpr std::int64_t kReplaceStepTicks = 2;
constexpr std::uint32_t kMaxQty = 100;
// Orders the sender still treats as live. The oldest is cancelled when full,
// so each book holds at most this many of our orders.
constexpr std::size_t kMaxLiveOrders = 1024;
constexpr std::uint64_t kSeed = 42;

constexpr std::string_view kUsage = R"(usage: mex_loadgen [options]
  --host <ipv4>        exchange address (default 127.0.0.1)
  --port <n>           exchange port (default 9000)
  --session <name>     session name (default alice)
  --token <token>      session token (default alice-secret)
  --symbols <a,b,...>  symbol ids to trade (default 1)
  --orders <n>         operations to send: NewOrder, Cancel, and Replace (default 100000)
  --rate <n>           operations per second; 0 sends as fast as the window allows (default 0)
  --window <n>         operations sent without a final report, at most (default 4096)
  --mid <ticks>        mid price; keep mid +/- 22 ticks inside each symbol's band (default 10000))";

[[nodiscard]] constexpr wire::Name NameOf(std::string_view p_text)
    pre(p_text.size() <= wire::kNameSize)
{
  wire::Name name{};
  std::ranges::copy(p_text, name.begin());
  return name;
}

struct Options
{
  std::string host_ = "127.0.0.1";
  std::uint16_t port_ = 9000;
  wire::Name session_ = NameOf("alice");
  wire::Name token_ = NameOf("alice-secret");
  std::vector<wire::SymbolId> symbols_{1};
  std::uint64_t orders_ = 100'000;
  std::uint64_t rate_ = 0;
  std::uint64_t window_ = 4096;
  std::int64_t mid_ = 10'000;
};

template <typename T>
[[nodiscard]] std::expected<T, std::string> ParseNumber(std::string_view p_name,
                                                        std::string_view p_text)
{
  T value{};
  const auto [end, error] = std::from_chars(p_text.data(), p_text.data() + p_text.size(), value);
  if (error != std::errc{} || end != p_text.data() + p_text.size())
  {
    return std::unexpected(std::format("{}: bad number '{}'", p_name, p_text));
  }
  return value;
}

[[nodiscard]] std::expected<wire::Name, std::string> ParseName(std::string_view p_name,
                                                               std::string_view p_text)
{
  if (p_text.empty() || p_text.size() > wire::kNameSize)
  {
    return std::unexpected(std::format("{}: must be 1 to {} bytes", p_name, wire::kNameSize));
  }
  return NameOf(p_text);
}

[[nodiscard]] std::expected<std::vector<wire::SymbolId>, std::string>
ParseSymbols(std::string_view p_text)
{
  std::vector<wire::SymbolId> symbols;
  symbols.reserve(1 + static_cast<std::size_t>(std::ranges::count(p_text, ',')));
  std::size_t begin = 0;
  while (begin <= p_text.size())
  {
    const std::size_t comma = std::min(p_text.find(',', begin), p_text.size());
    const auto symbol =
        ParseNumber<wire::SymbolId>("--symbols", p_text.substr(begin, comma - begin));
    if (!symbol.has_value())
    {
      return std::unexpected(symbol.error());
    }
    symbols.push_back(*symbol);
    begin = comma + 1;
  }
  return symbols;
}

// Applies one "--name value" pair.
[[nodiscard]] std::expected<void, std::string>
ApplyOption(Options &p_options, std::string_view p_name, std::string_view p_value)
{
  const auto assign = [](auto &p_field, auto p_parsed) -> std::expected<void, std::string>
  {
    if (!p_parsed.has_value())
    {
      return std::unexpected(p_parsed.error());
    }
    p_field = std::move(*p_parsed);
    return {};
  };
  if (p_name == "--host")
  {
    p_options.host_ = p_value;
    return {};
  }
  if (p_name == "--port")
  {
    return assign(p_options.port_, ParseNumber<std::uint16_t>(p_name, p_value));
  }
  if (p_name == "--session")
  {
    return assign(p_options.session_, ParseName(p_name, p_value));
  }
  if (p_name == "--token")
  {
    return assign(p_options.token_, ParseName(p_name, p_value));
  }
  if (p_name == "--symbols")
  {
    return assign(p_options.symbols_, ParseSymbols(p_value));
  }
  if (p_name == "--orders")
  {
    return assign(p_options.orders_, ParseNumber<std::uint64_t>(p_name, p_value));
  }
  if (p_name == "--rate")
  {
    return assign(p_options.rate_, ParseNumber<std::uint64_t>(p_name, p_value));
  }
  if (p_name == "--window")
  {
    return assign(p_options.window_, ParseNumber<std::uint64_t>(p_name, p_value));
  }
  if (p_name == "--mid")
  {
    return assign(p_options.mid_, ParseNumber<std::int64_t>(p_name, p_value));
  }
  return std::unexpected(std::format("unknown option {}", p_name));
}

[[nodiscard]] std::expected<Options, std::string> ParseOptions(std::span<char *const> p_args)
{
  Options options{};
  for (std::size_t index = 1; index < p_args.size(); index += 2)
  {
    if (index + 1 == p_args.size())
    {
      return std::unexpected(std::format("{} needs a value", p_args[index]));
    }
    const auto applied = ApplyOption(options, p_args[index], p_args[index + 1]);
    if (!applied.has_value())
    {
      return std::unexpected(applied.error());
    }
  }
  if (options.orders_ == 0 || options.window_ == 0)
  {
    return std::unexpected("--orders and --window must be at least 1");
  }
  if (options.mid_ <= kMaxDepthTicks + kReplaceStepTicks)
  {
    return std::unexpected(
        std::format("--mid must be above {}", kMaxDepthTicks + kReplaceStepTicks));
  }
  return options;
}

[[nodiscard]] std::string SystemError(std::string_view p_what)
{
  return std::format("{}: {}", p_what, std::strerror(errno));
}

[[nodiscard]] std::expected<mex::UniqueFd, std::string> Connect(const Options &p_options)
{
  mex::UniqueFd fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (!fd.IsOpen())
  {
    return std::unexpected(SystemError("socket"));
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(p_options.port_);
  if (::inet_pton(AF_INET, p_options.host_.c_str(), &address.sin_addr) != 1)
  {
    return std::unexpected(std::format("bad IPv4 address {}", p_options.host_));
  }
  // The socket API takes every address family through sockaddr.
  if (::connect(fd.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
  {
    return std::unexpected(
        SystemError(std::format("connect {}:{}", p_options.host_, p_options.port_)));
  }
  const int one = 1;
  if (::setsockopt(fd.Get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0 ||
      ::setsockopt(fd.Get(), SOL_SOCKET, SO_RCVTIMEO, &kSocketTimeout, sizeof(kSocketTimeout)) !=
          0 ||
      ::setsockopt(fd.Get(), SOL_SOCKET, SO_SNDTIMEO, &kSocketTimeout, sizeof(kSocketTimeout)) != 0)
  {
    return std::unexpected(SystemError("setsockopt"));
  }
  return fd;
}

// Exchange frames from a blocking socket.
class FrameReader final
{
public:
  explicit FrameReader(int p_fd) : fd_{p_fd} {}

  // Blocks until a whole frame arrives. An error is the reason to stop.
  [[nodiscard]] std::expected<wire::Decoded<wire::ExchangeMessage>, std::string> Next()
  {
    while (true)
    {
      const auto decoded =
          wire::DecodeExchangeFrame(std::span{buffer_}.subspan(begin_, end_ - begin_));
      if (decoded.status_ == wire::DecodeStatus::Complete)
      {
        begin_ += decoded.frame_size_;
        return decoded;
      }
      if (decoded.status_ == wire::DecodeStatus::Malformed)
      {
        return std::unexpected("malformed frame from the exchange");
      }
      std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
      end_ -= begin_;
      begin_ = 0;
      const ssize_t received = ::recv(fd_, buffer_.data() + end_, buffer_.size() - end_, 0);
      if (received == 0)
      {
        return std::unexpected("the exchange closed the connection");
      }
      if (received < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return std::unexpected("no frame from the exchange for 5 s");
        }
        return std::unexpected(SystemError("recv"));
      }
      end_ += static_cast<std::size_t>(received);
    }
  }

private:
  int fd_;
  std::size_t begin_ = 0; // unread bytes are [begin_, end_)
  std::size_t end_ = 0;
  std::array<std::byte, kReadBufferSize> buffer_{};
};

// Batches client frames into one send call.
class FrameWriter final
{
public:
  explicit FrameWriter(int p_fd) : fd_{p_fd} {}

  template <typename M> [[nodiscard]] std::expected<void, std::string> Append(const M &p_message)
  {
    if (buffer_.size() - size_ < wire::kFrameSize<M>)
    {
      const auto flushed = Flush();
      if (!flushed.has_value())
      {
        return flushed;
      }
    }
    size_ += wire::Encode(p_message, std::span{buffer_}.subspan(size_));
    return {};
  }

  [[nodiscard]] std::expected<void, std::string> Flush()
  {
    std::size_t sent = 0;
    while (sent < size_)
    {
      const ssize_t result = ::send(fd_, buffer_.data() + sent, size_ - sent, MSG_NOSIGNAL);
      if (result < 0)
      {
        if (errno == EINTR)
        {
          continue;
        }
        return std::unexpected(SystemError("send"));
      }
      sent += static_cast<std::size_t>(result);
    }
    size_ = 0;
    return {};
  }

private:
  int fd_;
  std::size_t size_ = 0;
  std::array<std::byte, kSendBatchFrames * wire::kMaxFrameSize> buffer_{};
};

// Written by the sender, read by the reader thread, and the reverse for finals_.
struct Shared
{
  explicit Shared(std::uint64_t p_orders)
      : sent_at_ns_{std::make_unique<std::atomic<std::int64_t>[]>(p_orders + 1)}
  {
  }

  // By client_order_id: steady_clock time of the NewOrder send, 0 once the first
  // report came. steady_clock counts from boot, so a real time is never 0.
  std::unique_ptr<std::atomic<std::int64_t>[]> sent_at_ns_;
  std::atomic<std::uint64_t> finals_{0}; // final reports received
  std::atomic<bool> failed_{false};
};

[[nodiscard]] std::int64_t NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

constexpr std::array<std::string_view, std::variant_size_v<wire::ExchangeMessage>> kMessageNames{
    "LogonAck",  "LogonReject",  "Ack",        "Reject",        "Fill",
    "CancelAck", "CancelReject", "ReplaceAck", "ReplaceReject", "Done"};

struct ReaderResult
{
  std::array<std::uint64_t, std::variant_size_v<wire::ExchangeMessage>> by_type_{};
  std::uint64_t system_busy_ = 0;
  std::vector<std::int64_t> latencies_ns_{}; // NewOrder -> first report
  Clock::time_point last_final_{};
  std::string error_{};
};

[[nodiscard]] bool IsFinal(const wire::ExchangeMessage &p_message)
{
  return !std::holds_alternative<wire::Fill>(p_message) &&
         !std::holds_alternative<wire::LogonAck>(p_message) &&
         !std::holds_alternative<wire::LogonReject>(p_message);
}

// Reader thread. Stops after p_expected finals, or at the first error.
void ReadReports(FrameReader &p_reader, Shared &p_shared, std::uint64_t p_expected,
                 ReaderResult &p_result)
{
  std::uint64_t next_seq = 2; // LogonAck was seq 1
  std::uint64_t finals = 0;
  while (finals < p_expected)
  {
    const auto frame = p_reader.Next();
    if (!frame.has_value())
    {
      p_result.error_ = frame.error();
      break;
    }
    const std::int64_t now_ns = NowNs();
    if (frame->seq_ != next_seq)
    {
      p_result.error_ = std::format("seq gap: expected {}, got {}", next_seq, frame->seq_);
      break;
    }
    ++next_seq;
    ++p_result.by_type_[frame->message_.index()];
    if (const auto *reject = std::get_if<wire::Reject>(&frame->message_);
        reject != nullptr && reject->reason_ == wire::Reason::SystemBusy)
    {
      ++p_result.system_busy_;
    }
    bool duplicate_request = false;
    std::visit(
        [&](const auto &p_message)
        {
          if constexpr (requires { p_message.reason_; })
          {
            duplicate_request = p_message.reason_ == wire::Reason::DuplicateRequest;
          }
          if constexpr (requires { p_message.client_order_id_; })
          {
            const wire::ClientOrderId id = p_message.client_order_id_;
            if (id <= p_expected)
            {
              const std::int64_t sent_ns =
                  p_shared.sent_at_ns_[id].exchange(0, std::memory_order_acquire);
              if (sent_ns != 0)
              {
                p_result.latencies_ns_.push_back(now_ns - sent_ns);
              }
            }
          }
        },
        frame->message_);
    if (duplicate_request)
    {
      p_result.error_ = "DuplicateRequest reject: this session already sent requests to the "
                        "exchange; restart the exchange or use another session";
      break;
    }
    if (IsFinal(frame->message_))
    {
      ++finals;
      p_result.last_final_ = Clock::now();
      p_shared.finals_.store(finals, std::memory_order_release);
    }
  }
  if (!p_result.error_.empty())
  {
    p_shared.failed_.store(true, std::memory_order_relaxed);
  }
}

struct LiveOrder
{
  wire::ClientOrderId client_order_id_ = 0;
  wire::SymbolId symbol_id_ = 0;
  Price price_{};
};

struct SenderResult
{
  std::uint64_t new_orders_ = 0;
  std::uint64_t cancels_ = 0;
  std::uint64_t replaces_ = 0;
  std::string error_{};
};

// Builds the operation mix. The live orders are a FIFO ring: Cancel takes the
// oldest, Replace a random one.
class OrderFlow final
{
public:
  explicit OrderFlow(const Options &p_options) : options_{p_options} {}

  // Appends the next operation to p_writer.
  [[nodiscard]] std::expected<void, std::string> Next(FrameWriter &p_writer, Shared &p_shared,
                                                      SenderResult &p_result)
  {
    const std::uint32_t roll = percent_(rng_);
    if (live_count_ > 0 && roll < kReplacePercent)
    {
      ++p_result.replaces_;
      return p_writer.Append(NextReplace());
    }
    if (live_count_ == kMaxLiveOrders ||
        (live_count_ > 0 && roll < kReplacePercent + kCancelPercent))
    {
      const LiveOrder &oldest = live_[live_begin_];
      live_begin_ = (live_begin_ + 1) % kMaxLiveOrders;
      --live_count_;
      ++p_result.cancels_;
      return p_writer.Append(wire::Cancel{.request_number_ = NextRequestNumber(),
                                          .client_order_id_ = oldest.client_order_id_,
                                          .symbol_id_ = oldest.symbol_id_});
    }
    ++p_result.new_orders_;
    const wire::NewOrder order = NextNewOrder();
    live_[(live_begin_ + live_count_) % kMaxLiveOrders] = {.client_order_id_ =
                                                               order.client_order_id_,
                                                           .symbol_id_ = order.symbol_id_,
                                                           .price_ = order.price_};
    ++live_count_;
    p_shared.sent_at_ns_[order.client_order_id_].store(NowNs(), std::memory_order_release);
    return p_writer.Append(order);
  }

private:
  // One sequence for all symbols, so each engine sees increasing numbers with gaps.
  [[nodiscard]] wire::RequestNumber NextRequestNumber()
  {
    ++last_request_number_.value_;
    return last_request_number_;
  }

  [[nodiscard]] Price Clamp(std::int64_t p_ticks) const
  {
    return Price{
        std::clamp(p_ticks, options_.mid_ - kMaxDepthTicks, options_.mid_ + kMaxDepthTicks)};
  }

  [[nodiscard]] wire::NewOrder NextNewOrder()
  {
    const Side side = coin_(rng_) == 0 ? Side::Buy : Side::Sell;
    const std::int64_t away = depth_(rng_); // negative crosses mid
    return {.request_number_ = NextRequestNumber(),
            .client_order_id_ = ++last_client_order_id_,
            .symbol_id_ = options_.symbols_[symbol_(rng_)],
            .side_ = side,
            .type_ = wire::OrderType::Limit,
            .price_ = Clamp(side == Side::Buy ? options_.mid_ - away : options_.mid_ + away),
            .qty_ = Quantity{qty_(rng_)}};
  }

  [[nodiscard]] wire::Replace NextReplace()
  {
    std::uniform_int_distribution<std::size_t> pick{0, live_count_ - 1};
    LiveOrder &order = live_[(live_begin_ + pick(rng_)) % kMaxLiveOrders];
    order.price_ = Clamp(order.price_.ticks_ + step_(rng_));
    return {.request_number_ = NextRequestNumber(),
            .client_order_id_ = order.client_order_id_,
            .symbol_id_ = order.symbol_id_,
            .new_price_ = order.price_,
            .new_total_qty_ = Quantity{qty_(rng_)}};
  }

  const Options &options_;
  std::mt19937_64 rng_{kSeed};
  std::uniform_int_distribution<std::uint32_t> percent_{0, 99};
  std::uniform_int_distribution<std::uint32_t> coin_{0, 1};
  std::uniform_int_distribution<std::int64_t> depth_{-kCrossTicks, kMaxDepthTicks};
  std::uniform_int_distribution<std::int64_t> step_{-kReplaceStepTicks, kReplaceStepTicks};
  std::uniform_int_distribution<std::uint32_t> qty_{1, kMaxQty};
  std::uniform_int_distribution<std::size_t> symbol_{0, options_.symbols_.size() - 1};
  std::array<LiveOrder, kMaxLiveOrders> live_{};
  std::size_t live_begin_ = 0;
  std::size_t live_count_ = 0;
  wire::ClientOrderId last_client_order_id_ = 0;
  wire::RequestNumber last_request_number_{};
};

// Sends p_options.orders_ operations, paced by --rate and limited by --window.
[[nodiscard]] SenderResult SendOrders(const Options &p_options, int p_fd, Shared &p_shared,
                                      Clock::time_point p_start)
{
  SenderResult result{};
  FrameWriter writer{p_fd};
  OrderFlow flow{p_options};
  const auto fail = [&result](std::string p_error)
  {
    result.error_ = std::move(p_error);
    return result;
  };
  for (std::uint64_t sent = 0; sent < p_options.orders_; ++sent)
  {
    if (p_shared.failed_.load(std::memory_order_relaxed))
    {
      return fail("stopped: the reader failed");
    }
    if (p_options.rate_ > 0)
    {
      const auto due = p_start + std::chrono::nanoseconds{sent * 1'000'000'000 / p_options.rate_};
      if (Clock::now() < due)
      {
        if (const auto flushed = writer.Flush(); !flushed.has_value())
        {
          return fail(flushed.error());
        }
        std::this_thread::sleep_until(due);
      }
    }
    if (sent - p_shared.finals_.load(std::memory_order_acquire) >= p_options.window_)
    {
      // The reports we wait for need the buffered frames sent first.
      if (const auto flushed = writer.Flush(); !flushed.has_value())
      {
        return fail(flushed.error());
      }
      while (sent - p_shared.finals_.load(std::memory_order_acquire) >= p_options.window_)
      {
        if (p_shared.failed_.load(std::memory_order_relaxed))
        {
          return fail("stopped: the reader failed");
        }
        std::this_thread::yield();
      }
    }
    if (const auto appended = flow.Next(writer, p_shared, result); !appended.has_value())
    {
      return fail(appended.error());
    }
  }
  if (const auto flushed = writer.Flush(); !flushed.has_value())
  {
    return fail(flushed.error());
  }
  return result;
}

[[nodiscard]] std::expected<void, std::string> LogOn(const Options &p_options, int p_fd,
                                                     FrameReader &p_reader)
{
  FrameWriter writer{p_fd};
  const auto appended =
      writer.Append(wire::Logon{.session_name_ = p_options.session_, .token_ = p_options.token_});
  const auto flushed = appended.has_value() ? writer.Flush() : appended;
  if (!flushed.has_value())
  {
    return flushed;
  }
  const auto frame = p_reader.Next();
  if (!frame.has_value())
  {
    return std::unexpected(frame.error());
  }
  if (const auto *reject = std::get_if<wire::LogonReject>(&frame->message_); reject != nullptr)
  {
    return std::unexpected(
        std::format("logon rejected, reason {}", std::to_underlying(reject->reason_)));
  }
  if (!std::holds_alternative<wire::LogonAck>(frame->message_) || frame->seq_ != 1)
  {
    return std::unexpected("expected LogonAck with seq 1");
  }
  return {};
}

[[nodiscard]] double Micros(std::int64_t p_ns) { return static_cast<double>(p_ns) / 1000.0; }

void PrintSummary(const SenderResult &p_sent, ReaderResult &p_read, Clock::time_point p_start)
{
  const std::uint64_t total = p_sent.new_orders_ + p_sent.cancels_ + p_sent.replaces_;
  const double seconds = std::chrono::duration<double>(p_read.last_final_ - p_start).count();
  std::println("sent {} operations: NewOrder {}, Cancel {}, Replace {}", total, p_sent.new_orders_,
               p_sent.cancels_, p_sent.replaces_);
  std::print("reports:");
  for (std::size_t index = 0; index < kMessageNames.size(); ++index)
  {
    std::print(" {} {}", kMessageNames[index], p_read.by_type_[index]);
  }
  std::println(" (Reject SystemBusy {})", p_read.system_busy_);
  std::println("elapsed {:.3f} s, {:.0f} ops/s", seconds, static_cast<double>(total) / seconds);
  std::vector<std::int64_t> &latencies = p_read.latencies_ns_;
  if (latencies.empty())
  {
    return;
  }
  std::ranges::sort(latencies);
  const std::size_t p99 = (latencies.size() - 1) * 99 / 100;
  std::println("NewOrder -> first report latency: p50 {:.1f} us, p99 {:.1f} us, max {:.1f} us "
               "({} samples)",
               Micros(latencies[latencies.size() / 2]), Micros(latencies[p99]),
               Micros(latencies.back()), latencies.size());
}

[[nodiscard]] int RunLoad(const Options &p_options)
{
  auto fd = Connect(p_options);
  if (!fd.has_value())
  {
    std::println(stderr, "mex_loadgen: {}", fd.error());
    return kExitFailure;
  }
  FrameReader reader{fd->Get()};
  if (const auto logged_on = LogOn(p_options, fd->Get(), reader); !logged_on.has_value())
  {
    std::println(stderr, "mex_loadgen: logon: {}", logged_on.error());
    return kExitFailure;
  }

  Shared shared{p_options.orders_};
  ReaderResult read{};
  read.latencies_ns_.reserve(p_options.orders_);
  const Clock::time_point start = Clock::now();
  SenderResult sent{};
  {
    std::jthread reader_thread{
        [reader_ptr = &reader, shared_ptr = &shared, expected = p_options.orders_, read_ptr = &read]
        { ReadReports(*reader_ptr, *shared_ptr, expected, *read_ptr); }};
    sent = SendOrders(p_options, fd->Get(), shared, start);
    if (!sent.error_.empty())
    {
      // Wakes a reader blocked in recv.
      ::shutdown(fd->Get(), SHUT_RDWR);
    }
  }
  if (!sent.error_.empty() || !read.error_.empty())
  {
    std::println(stderr, "mex_loadgen: {}", read.error_.empty() ? sent.error_ : read.error_);
    return kExitFailure;
  }
  PrintSummary(sent, read, start);
  return 0;
}

} // namespace

int main(int p_argc, char **p_argv)
{
  const std::span<char *const> args{p_argv, static_cast<std::size_t>(p_argc)};
  if (args.size() == 2 && std::string_view{args[1]} == "--help")
  {
    std::println("{}", kUsage);
    return 0;
  }
  const auto options = ParseOptions(args);
  if (!options.has_value())
  {
    std::println(stderr, "mex_loadgen: {}\n{}", options.error(), kUsage);
    return kExitUsage;
  }
  return RunLoad(*options);
}
