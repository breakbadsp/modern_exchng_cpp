#include "gateway.hpp"

#include "order_check.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <liburing.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

namespace mex
{
namespace
{

// epoll user data of the listening socket; connections use their index.
constexpr std::uint64_t kListenerTag = ~std::uint64_t{0};

constexpr std::uint64_t kFirstSeq = 1;

// io_uring ring size. A connection queues about a recv, a send, and two cancels
// per loop iteration, so the SQ seldom fills before the next submit; GetSqe
// submits early when it does.
constexpr std::uint64_t kSqesPerConnection = 4;
constexpr std::uint64_t kSpareSqes = 2;        // accept and the final cancel
constexpr std::uint64_t kMaxSqEntries = 32768; // IORING_MAX_ENTRIES
constexpr std::uint32_t kCqPerSq = 2;          // room for a burst of accepts

// The SINGLE_ISSUER ring starts disabled, so that the gateway thread, not the
// thread that opens it, becomes its issuer. COOP_TASKRUN with TASKRUN_FLAG: the
// kernel does not interrupt the loop to post completions, but sets a flag that
// io_uring_peek_cqe checks, and only then enters the kernel to run that work.
constexpr std::uint32_t kRingFlags =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_R_DISABLED | IORING_SETUP_COOP_TASKRUN |
    IORING_SETUP_TASKRUN_FLAG | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;

// After Stop(): how long Run() waits for cancelled I/O before it gives up.
constexpr std::uint32_t kStopWaits = 100;
constexpr long kStopWaitNanoseconds = 10'000'000;

// io_uring user data: the operation, the slot generation, and the slot index.
// The listener's accept and the cancels use slot 0, generation 0.
enum class Op : std::uint8_t
{
  Accept = 0,
  Recv,
  Send,
  Cancel,
};

constexpr std::uint32_t kGenerationBits = 24;
constexpr std::uint32_t kGenerationMask = (std::uint32_t{1} << kGenerationBits) - 1;
constexpr std::uint32_t kOpShift = 56;
constexpr std::uint32_t kGenerationShift = 32;

[[nodiscard]] constexpr std::uint64_t UserData(Op p_op, std::uint32_t p_generation,
                                               std::uint32_t p_index)
{
  return (std::uint64_t{static_cast<std::uint8_t>(p_op)} << kOpShift) |
         (std::uint64_t{p_generation & kGenerationMask} << kGenerationShift) | p_index;
}

[[nodiscard]] constexpr Op OpOf(std::uint64_t p_user_data)
{
  return static_cast<Op>(p_user_data >> kOpShift);
}

[[nodiscard]] constexpr std::uint32_t GenerationOf(std::uint64_t p_user_data)
{
  return static_cast<std::uint32_t>(p_user_data >> kGenerationShift) & kGenerationMask;
}

[[nodiscard]] constexpr std::uint32_t IndexOf(std::uint64_t p_user_data)
{
  return static_cast<std::uint32_t>(p_user_data);
}

// True for the last completion of an operation; a multishot accept posts many.
[[nodiscard]] bool IsFinal(std::uint64_t p_user_data, std::uint32_t p_flags)
{
  switch (OpOf(p_user_data))
  {
  case Op::Accept:
    return (p_flags & IORING_CQE_F_MORE) == 0;
  case Op::Recv:
  case Op::Send:
    return true;
  case Op::Cancel:
    return false; // not counted in Gateway::uring_ops_
  }
  return false;
}

// A transient result: the same operation is submitted again.
[[nodiscard]] bool IsRetry(std::int32_t p_result)
{
  return p_result == -EAGAIN || p_result == -EINTR;
}

// An SQE; if the SQ is full, submits it first. With SUBMIT_ALL the kernel
// consumes every queued SQE, so the retry finds a free one.
[[nodiscard]] io_uring_sqe *GetSqe(io_uring *p_ring)
{
  io_uring_sqe *sqe = io_uring_get_sqe(p_ring);
  if (sqe == nullptr)
  {
    static_cast<void>(io_uring_submit(p_ring));
    sqe = io_uring_get_sqe(p_ring);
  }
  contract_assert(sqe != nullptr);
  return sqe;
}

[[nodiscard]] std::unexpected<std::string> SystemError(std::string_view p_what)
{
  return std::unexpected(std::format("{}: {}", p_what, std::system_category().message(errno)));
}

[[nodiscard]] bool WouldBlock(int p_errno) { return p_errno == EAGAIN || p_errno == EWOULDBLOCK; }

} // namespace

UniqueRing::UniqueRing() = default;

UniqueRing::~UniqueRing() { Reset(); }

UniqueRing::UniqueRing(UniqueRing &&p_other) noexcept : ring_(std::move(p_other.ring_)) {}

UniqueRing &UniqueRing::operator=(UniqueRing &&p_other) noexcept
{
  if (this != &p_other)
  {
    Reset();
    ring_ = std::move(p_other.ring_);
  }
  return *this;
}

std::expected<UniqueRing, int> UniqueRing::Create(std::uint32_t p_sq_entries,
                                                  std::uint32_t p_cq_entries, std::uint32_t p_flags)
{
  auto ring = std::make_unique<io_uring>();
  io_uring_params params{};
  params.flags = p_flags;
  params.cq_entries = p_cq_entries;
  // The thread that creates a ring stays linked to it until that thread ends.
  // When the ring is destroyed, the kernel interrupts every linked thread with
  // task work, so a blocking call with a timeout in that thread fails with
  // EINTR. A short-lived thread creates the ring, so that no caller is linked.
  int result = 0;
  std::jthread{[&] { result = io_uring_queue_init_params(p_sq_entries, ring.get(), &params); }}
      .join();
  if (result < 0)
  {
    return std::unexpected(-result);
  }
  UniqueRing owner{};
  owner.ring_ = std::move(ring);
  return owner;
}

void UniqueRing::Reset()
{
  if (ring_ != nullptr)
  {
    io_uring_queue_exit(ring_.get());
    ring_.reset();
  }
}

UniqueFd &UniqueFd::operator=(UniqueFd &&p_other) noexcept
{
  if (this != &p_other)
  {
    Reset();
    fd_ = std::exchange(p_other.fd_, -1);
  }
  return *this;
}

void UniqueFd::Reset()
{
  if (fd_ >= 0)
  {
    ::close(fd_);
    fd_ = -1;
  }
}

std::expected<GatewaySockets, std::string> OpenGatewaySockets(const GatewayConfig &p_config,
                                                              GatewayIo p_io)
{
  GatewaySockets sockets{};
  sockets.io_ = p_io;
  sockets.listener_ = UniqueFd{::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
  if (!sockets.listener_.IsOpen())
  {
    return SystemError("socket");
  }
  const int one = 1;
  if (::setsockopt(sockets.listener_.Get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
  {
    return SystemError("setsockopt SO_REUSEADDR");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(p_config.port_);
  if (::inet_pton(AF_INET, p_config.listen_address_.c_str(), &address.sin_addr) != 1)
  {
    return std::unexpected(std::format("bad listen address {}", p_config.listen_address_));
  }
  // The socket API takes every address family through sockaddr.
  if (::bind(sockets.listener_.Get(), reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0)
  {
    return SystemError(std::format("bind {}:{}", p_config.listen_address_, p_config.port_));
  }
  if (::listen(sockets.listener_.Get(), SOMAXCONN) != 0)
  {
    return SystemError("listen");
  }
  socklen_t length = sizeof(address);
  if (::getsockname(sockets.listener_.Get(), reinterpret_cast<sockaddr *>(&address), &length) != 0)
  {
    return SystemError("getsockname");
  }
  sockets.port_ = ntohs(address.sin_port);

  if (p_io == GatewayIo::Uring)
  {
    const std::uint64_t wanted =
        std::min(kSqesPerConnection * p_config.max_connections_ + kSpareSqes, kMaxSqEntries);
    const auto sq_entries = static_cast<std::uint32_t>(std::bit_ceil(wanted));
    auto ring = UniqueRing::Create(sq_entries, kCqPerSq * sq_entries, kRingFlags);
    if (!ring.has_value())
    {
      return std::unexpected(
          std::format("io_uring_queue_init: {}", std::system_category().message(ring.error())));
    }
    sockets.ring_ = std::move(*ring);
    return sockets;
  }
  sockets.epoll_ = UniqueFd{::epoll_create1(EPOLL_CLOEXEC)};
  if (!sockets.epoll_.IsOpen())
  {
    return SystemError("epoll_create1");
  }
  epoll_event event{.events = EPOLLIN, .data = {.u64 = kListenerTag}};
  if (::epoll_ctl(sockets.epoll_.Get(), EPOLL_CTL_ADD, sockets.listener_.Get(), &event) != 0)
  {
    return SystemError("epoll_ctl listener");
  }
  return sockets;
}

struct Gateway::Connection
{
  UniqueFd fd_{};
  std::optional<SessionId> session_{};
  std::size_t read_size_ = 0;
  std::size_t send_begin_ = 0; // unsent bytes are [send_begin_, send_end_)
  std::size_t send_end_ = 0;
  bool want_write_ = false;    // EPOLLOUT is registered
  bool pending_flush_ = false; // the index is in Gateway::pending_flush_
  // io_uring only. While recv_in_flight_ the kernel writes the free tail of
  // read_; while send_in_flight_ it reads send_ from send_begin_ on.
  std::uint32_t generation_ = 0; // in the user data of this slot's operations
  bool recv_in_flight_ = false;
  bool send_in_flight_ = false;
  std::array<std::byte, kReadBufferSize> read_{};
  std::array<std::byte, kSendBufferSize> send_{};
};

Gateway::Gateway(const Config &p_config, GatewaySockets p_sockets,
                 std::span<const EngineQueues> p_queues)
    : routes_(p_config.symbols_), session_configs_(p_config.sessions_),
      sessions_(p_config.sessions_.size()), queues_(p_queues.begin(), p_queues.end()),
      connections_(std::make_unique<Connection[]>(p_config.gateway_.max_connections_)),
      sockets_(std::move(p_sockets))
{
  std::ranges::sort(routes_, {}, &SymbolConfig::symbol_id_);
  for ([[maybe_unused]] const SymbolConfig &route : routes_)
  {
    contract_assert(route.engine_ < queues_.size());
  }
  const std::uint32_t max_connections = p_config.gateway_.max_connections_;
  free_connections_.reserve(max_connections);
  for (std::uint32_t index = max_connections; index > 0; --index)
  {
    free_connections_.push_back(index - 1);
  }
  pending_flush_.reserve(max_connections);
  if (sockets_.io_ == GatewayIo::Uring)
  {
    uring_cq_entries_ = sockets_.ring_.Get()->cq.ring_entries;
  }
}

Gateway::~Gateway() = default;

void Gateway::Run()
{
  if (sockets_.io_ == GatewayIo::Uring)
  {
    StartUring();
  }
  while (!stop_.load(std::memory_order_relaxed))
  {
    PollOnce();
  }
  if (sockets_.io_ == GatewayIo::Uring)
  {
    StopUring();
  }
}

void Gateway::Stop() { stop_.store(true, std::memory_order_relaxed); }

std::uint64_t Gateway::DroppedReports() const
{
  return dropped_reports_.load(std::memory_order_relaxed);
}

void Gateway::PollOnce()
{
  switch (sockets_.io_)
  {
  case GatewayIo::Epoll:
    PollEpoll();
    break;
  case GatewayIo::Uring:
    PollUring();
    break;
  }
  DrainOutbound();
  FlushPending();
}

void Gateway::PollEpoll()
{
  // Timeout 0: busy-poll, so the outbound queues are drained while sockets are idle.
  const int count = ::epoll_wait(sockets_.epoll_.Get(), events_.data(), kMaxEvents, 0);
  for (int i = 0; i < count; ++i)
  {
    const epoll_event &event = events_[static_cast<std::size_t>(i)];
    if (event.data.u64 == kListenerTag)
    {
      Accept();
      continue;
    }
    // A connection is closed only while its own event is handled, and an fd
    // appears once per batch, so no later event in this batch names a reused slot.
    const auto index = static_cast<std::uint32_t>(event.data.u64);
    if ((event.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0)
    {
      OnReadable(index);
    }
    if ((event.events & EPOLLOUT) != 0 && connections_[index].fd_.IsOpen())
    {
      static_cast<void>(FlushEpoll(index));
    }
  }
}

void Gateway::Accept()
{
  // A connection beyond max_connections, or one that cannot be set up, is
  // closed when fd goes out of scope.
  UniqueFd fd{::accept4(sockets_.listener_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
  if (fd.IsOpen())
  {
    Adopt(std::move(fd));
  }
}

void Gateway::Adopt(UniqueFd p_fd)
{
  if (free_connections_.empty())
  {
    return;
  }
  const int one = 1;
  if (::setsockopt(p_fd.Get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
  {
    return;
  }
  const std::uint32_t index = free_connections_.back();
  Connection &connection = connections_[index];
  if (sockets_.io_ == GatewayIo::Epoll)
  {
    epoll_event event{.events = EPOLLIN, .data = {.u64 = index}};
    if (::epoll_ctl(sockets_.epoll_.Get(), EPOLL_CTL_ADD, p_fd.Get(), &event) != 0)
    {
      return;
    }
  }
  free_connections_.pop_back();
  connection.fd_ = std::move(p_fd);
  if (sockets_.io_ == GatewayIo::Uring)
  {
    connection.generation_ = (connection.generation_ + 1) & kGenerationMask;
    ArmRecv(index);
  }
}

void Gateway::OnReadable(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  // Compaction leaves less than one frame, and the buffer holds several.
  contract_assert(connection.read_size_ < kReadBufferSize);
  const std::span<std::byte> space = std::span{connection.read_}.subspan(connection.read_size_);
  const ssize_t received = ::recv(connection.fd_.Get(), space.data(), space.size(), 0);
  if (received < 0 && (WouldBlock(errno) || errno == EINTR))
  {
    return;
  }
  if (received <= 0)
  {
    Close(p_index);
    return;
  }
  connection.read_size_ += static_cast<std::size_t>(received);
  ProcessFrames(p_index);
}

void Gateway::ProcessFrames(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  std::size_t offset = 0;
  while (connection.fd_.IsOpen())
  {
    const auto decoded = wire::DecodeClientFrame(
        std::span{connection.read_}.first(connection.read_size_).subspan(offset));
    if (decoded.status_ == wire::DecodeStatus::NeedMore)
    {
      break;
    }
    if (decoded.status_ == wire::DecodeStatus::Malformed)
    {
      Close(p_index);
      return;
    }
    offset += decoded.frame_size_;
    OnFrame(p_index, decoded.message_);
  }
  if (!connection.fd_.IsOpen())
  {
    return;
  }
  std::memmove(connection.read_.data(), connection.read_.data() + offset,
               connection.read_size_ - offset);
  connection.read_size_ -= offset;
}

void Gateway::OnFrame(std::uint32_t p_index, const wire::ClientMessage &p_message)
{
  const std::optional<SessionId> session = connections_[p_index].session_;
  const auto *logon = std::get_if<wire::Logon>(&p_message);
  if (!session.has_value())
  {
    // The first frame must be a Logon; anything else closes the connection.
    if (logon != nullptr)
    {
      OnLogon(p_index, *logon);
    }
    else
    {
      Close(p_index);
    }
    return;
  }
  if (const auto *order = std::get_if<wire::NewOrder>(&p_message))
  {
    OnOrder(*session, *order);
  }
  else if (const auto *cancel = std::get_if<wire::Cancel>(&p_message))
  {
    OnOrder(*session, *cancel);
  }
  else if (const auto *replace = std::get_if<wire::Replace>(&p_message))
  {
    OnOrder(*session, *replace);
  }
  else
  {
    Close(p_index); // a second Logon on a logged-on connection
  }
}

void Gateway::OnLogon(std::uint32_t p_index, const wire::Logon &p_logon)
{
  const auto config =
      std::ranges::find(session_configs_, p_logon.session_name_, &SessionConfig::session_name_);
  std::optional<wire::Reason> reason{};
  if (config == session_configs_.end() || config->token_ != p_logon.token_)
  {
    reason = wire::Reason::BadCredentials;
  }
  else if (sessions_[static_cast<std::size_t>(config - session_configs_.begin())]
               .connection_.has_value())
  {
    reason = wire::Reason::AlreadyLoggedOn;
  }
  if (reason.has_value())
  {
    if (Append(p_index, kFirstSeq, wire::LogonReject{.reason_ = *reason}))
    {
      static_cast<void>(Flush(p_index));
    }
    Close(p_index);
    return;
  }
  const auto session_id = static_cast<SessionId>(config - session_configs_.begin());
  sessions_[session_id] = Session{.connection_ = p_index, .next_seq_ = kFirstSeq};
  connections_[p_index].session_ = session_id;
  SendToSession(session_id, wire::LogonAck{});
}

template <typename O> void Gateway::OnOrder(SessionId p_session, const O &p_order)
{
  const auto route =
      std::ranges::lower_bound(routes_, p_order.symbol_id_, {}, &SymbolConfig::symbol_id_);
  if (route == routes_.end() || route->symbol_id_ != p_order.symbol_id_)
  {
    SendToSession(p_session, RejectOf(p_order, wire::Reason::UnknownSymbol));
    return;
  }
  if (const std::optional<wire::Reason> reason = Check(*route, p_order); reason.has_value())
  {
    SendToSession(p_session, RejectOf(p_order, *reason));
    return;
  }
  // A command that is not queued takes no place in the order.
  const Command command{.op_number_ = OpNumber{last_op_.value_ + 1},
                        .session_id_ = p_session,
                        .symbol_slot_ = route->slot_,
                        .order_ = p_order};
  if (!queues_[route->engine_].inbound_.TryPush(command))
  {
    SendToSession(p_session, RejectOf(p_order, wire::Reason::SystemBusy));
    return;
  }
  last_op_ = command.op_number_;
}

void Gateway::DrainOutbound()
{
  for (const EngineQueues &queues : queues_)
  {
    // At most one ring's worth per pass, so a busy engine cannot starve the sockets.
    for (std::size_t budget = queues.outbound_.Capacity(); budget > 0; --budget)
    {
      const std::optional<Report> report = queues.outbound_.TryPop();
      if (!report.has_value())
      {
        break;
      }
      std::visit([this, &report](const auto &p_message)
                 { SendToSession(report->session_id_, p_message); }, report->report_);
    }
  }
}

template <typename M> void Gateway::SendToSession(SessionId p_session, const M &p_message)
{
  contract_assert(p_session < sessions_.size());
  Session &session = sessions_[p_session];
  if (!session.connection_.has_value())
  {
    // Single writer, so a load and a store are enough.
    dropped_reports_.store(dropped_reports_.load(std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
    return;
  }
  const std::uint32_t index = *session.connection_;
  if (!Append(index, session.next_seq_, p_message))
  {
    Close(index); // slow consumer
    return;
  }
  ++session.next_seq_;
}

template <typename M>
bool Gateway::Append(std::uint32_t p_index, std::uint64_t p_seq, const M &p_message)
{
  constexpr std::size_t kSize = wire::kFrameSize<M>;
  Connection &connection = connections_[p_index];
  if (kSendBufferSize - connection.send_end_ < kSize)
  {
    if (!MakeRoom(p_index))
    {
      return false;
    }
    if (kSendBufferSize - connection.send_end_ < kSize)
    {
      return false;
    }
  }
  static_cast<void>(
      wire::Encode(p_seq, p_message, std::span{connection.send_}.subspan(connection.send_end_)));
  connection.send_end_ += kSize;
  MarkPendingFlush(p_index);
  return true;
}

void Gateway::MarkPendingFlush(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  // Close does not clear pending_flush_, so an index is in the list at most once.
  if (!connection.pending_flush_)
  {
    connection.pending_flush_ = true;
    pending_flush_.push_back(p_index);
  }
}

bool Gateway::MakeRoom(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  // io_uring: a send in flight means the socket buffer was full when the kernel
  // tried it, and its bytes must stay where they are.
  const bool open = sockets_.io_ == GatewayIo::Epoll
                        ? FlushEpoll(p_index)
                        : (connection.send_in_flight_ || Write(p_index));
  if (!open)
  {
    return false;
  }
  if (!connection.send_in_flight_)
  {
    Compact(p_index);
  }
  return true;
}

void Gateway::Compact(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  std::memmove(connection.send_.data(), connection.send_.data() + connection.send_begin_,
               connection.send_end_ - connection.send_begin_);
  connection.send_end_ -= connection.send_begin_;
  connection.send_begin_ = 0;
}

bool Gateway::Flush(std::uint32_t p_index)
{
  switch (sockets_.io_)
  {
  case GatewayIo::Epoll:
    return FlushEpoll(p_index);
  case GatewayIo::Uring:
    SubmitSend(p_index);
    return true;
  }
  return true;
}

bool Gateway::FlushEpoll(std::uint32_t p_index)
{
  if (!Write(p_index))
  {
    return false;
  }
  const Connection &connection = connections_[p_index];
  return SetWriteInterest(p_index, connection.send_begin_ < connection.send_end_);
}

bool Gateway::Write(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  while (connection.send_begin_ < connection.send_end_)
  {
    const std::span<const std::byte> unsent = std::span{connection.send_}.subspan(
        connection.send_begin_, connection.send_end_ - connection.send_begin_);
    const ssize_t sent = ::send(connection.fd_.Get(), unsent.data(), unsent.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR)
    {
      continue;
    }
    if (sent < 0 && WouldBlock(errno))
    {
      return true;
    }
    if (sent <= 0)
    {
      Close(p_index);
      return false;
    }
    connection.send_begin_ += static_cast<std::size_t>(sent);
  }
  connection.send_begin_ = 0;
  connection.send_end_ = 0;
  return true;
}

void Gateway::FlushPending()
{
  for (const std::uint32_t index : pending_flush_)
  {
    Connection &connection = connections_[index];
    connection.pending_flush_ = false;
    // A connection that waits for EPOLLOUT is flushed when the event comes.
    if (connection.fd_.IsOpen() && !connection.want_write_)
    {
      static_cast<void>(Flush(index));
    }
  }
  pending_flush_.clear();
}

bool Gateway::SetWriteInterest(std::uint32_t p_index, bool p_want_write)
{
  Connection &connection = connections_[p_index];
  if (connection.want_write_ == p_want_write)
  {
    return true;
  }
  epoll_event event{.events = EPOLLIN | (p_want_write ? EPOLLOUT : 0U), .data = {.u64 = p_index}};
  if (::epoll_ctl(sockets_.epoll_.Get(), EPOLL_CTL_MOD, connection.fd_.Get(), &event) != 0)
  {
    Close(p_index);
    return false;
  }
  connection.want_write_ = p_want_write;
  return true;
}

void Gateway::Close(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  if (!connection.fd_.IsOpen())
  {
    return;
  }
  // Orders stay with the session (no cancel-on-disconnect in v1).
  if (connection.session_.has_value())
  {
    sessions_[*connection.session_].connection_.reset();
    connection.session_.reset();
  }
  if (sockets_.io_ == GatewayIo::Uring)
  {
    // Issue a queued send of this fd before the fd closes, as epoll's Flush
    // writes before Close. Then cancel what is in flight; the fd may close
    // now, because each operation holds its own reference to the socket.
    io_uring *const ring = sockets_.ring_.Get();
    if (io_uring_sq_ready(ring) > 0)
    {
      static_cast<void>(io_uring_submit(ring));
    }
    if (connection.recv_in_flight_)
    {
      SubmitCancel(UserData(Op::Recv, connection.generation_, p_index), 0);
    }
    if (connection.send_in_flight_)
    {
      SubmitCancel(UserData(Op::Send, connection.generation_, p_index), 0);
    }
  }
  connection.fd_.Reset(); // closing the fd also removes it from the epoll set
  connection.read_size_ = 0;
  connection.send_begin_ = 0;
  connection.send_end_ = 0;
  connection.want_write_ = false;
  ReleaseIfIdle(p_index);
}

void Gateway::ReleaseIfIdle(std::uint32_t p_index)
{
  const Connection &connection = connections_[p_index];
  if (!connection.recv_in_flight_ && !connection.send_in_flight_)
  {
    free_connections_.push_back(p_index);
  }
}

void Gateway::StartUring()
{
  // The calling thread becomes the only thread that may submit. It fails only
  // if the ring was not created disabled.
  [[maybe_unused]] const int enabled = io_uring_enable_rings(sockets_.ring_.Get());
  contract_assert(enabled == 0);
  ArmAccept();
}

void Gateway::PollUring()
{
  io_uring *const ring = sockets_.ring_.Get();
  if (io_uring_sq_ready(ring) > 0)
  {
    static_cast<void>(io_uring_submit(ring));
  }
  // At most one CQ's worth per pass. io_uring_peek_cqe enters the kernel only
  // when completion work is pending (IORING_SQ_TASKRUN) or the CQ overflowed.
  for (std::uint32_t budget = uring_cq_entries_; budget > 0; --budget)
  {
    io_uring_cqe *cqe = nullptr;
    if (io_uring_peek_cqe(ring, &cqe) != 0)
    {
      break;
    }
    // Copy and release the entry first: a handler may submit, and the kernel
    // may then post more entries.
    const std::uint64_t user_data = io_uring_cqe_get_data64(cqe);
    const std::int32_t result = cqe->res;
    const std::uint32_t flags = cqe->flags;
    io_uring_cqe_seen(ring, cqe);
    OnCompletion(user_data, result, flags);
  }
}

void Gateway::StopUring()
{
  // Cancel every operation, then wait until the kernel has returned every
  // buffer. Completions are only counted; no protocol work runs.
  SubmitCancel(0, IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
  io_uring *const ring = sockets_.ring_.Get();
  for (std::uint32_t wait = 0; uring_ops_ > 0 && wait < kStopWaits; ++wait)
  {
    io_uring_cqe *cqe = nullptr;
    __kernel_timespec timeout{.tv_sec = 0, .tv_nsec = kStopWaitNanoseconds};
    static_cast<void>(io_uring_submit_and_wait_timeout(ring, &cqe, 1, &timeout, nullptr));
    while (io_uring_peek_cqe(ring, &cqe) == 0)
    {
      if (IsFinal(io_uring_cqe_get_data64(cqe), cqe->flags))
      {
        --uring_ops_;
      }
      io_uring_cqe_seen(ring, cqe);
    }
  }
}

void Gateway::OnCompletion(std::uint64_t p_user_data, std::int32_t p_result, std::uint32_t p_flags)
{
  if (IsFinal(p_user_data, p_flags))
  {
    --uring_ops_;
  }
  const Op op = OpOf(p_user_data);
  if (op == Op::Accept)
  {
    OnAccepted(p_result, p_flags);
    return;
  }
  if (op == Op::Cancel)
  {
    return; // the cancelled operation posts its own completion
  }
  const std::uint32_t index = IndexOf(p_user_data);
  const Connection &connection = connections_[index];
  // A slot is reused only after all its operations complete, so a completion
  // from an older generation is not expected; it is ignored all the same.
  if (GenerationOf(p_user_data) != connection.generation_)
  {
    return;
  }
  if (op == Op::Recv)
  {
    OnReceived(index, p_result);
  }
  else
  {
    OnSent(index, p_result);
  }
}

void Gateway::OnAccepted(std::int32_t p_result, std::uint32_t p_flags)
{
  if ((p_flags & IORING_CQE_F_MORE) == 0)
  {
    ArmAccept(); // the multishot accept has ended, for example on an error
  }
  if (p_result >= 0)
  {
    Adopt(UniqueFd{p_result});
  }
}

void Gateway::OnReceived(std::uint32_t p_index, std::int32_t p_result)
{
  Connection &connection = connections_[p_index];
  if (!connection.recv_in_flight_)
  {
    return;
  }
  connection.recv_in_flight_ = false;
  if (!connection.fd_.IsOpen())
  {
    ReleaseIfIdle(p_index);
    return;
  }
  if (IsRetry(p_result))
  {
    ArmRecv(p_index);
    return;
  }
  if (p_result <= 0)
  {
    Close(p_index);
    return;
  }
  connection.read_size_ += static_cast<std::size_t>(p_result);
  ProcessFrames(p_index);
  if (connection.fd_.IsOpen())
  {
    ArmRecv(p_index);
  }
}

void Gateway::OnSent(std::uint32_t p_index, std::int32_t p_result)
{
  Connection &connection = connections_[p_index];
  if (!connection.send_in_flight_)
  {
    return;
  }
  connection.send_in_flight_ = false;
  if (!connection.fd_.IsOpen())
  {
    ReleaseIfIdle(p_index);
    return;
  }
  if (p_result <= 0 && !IsRetry(p_result))
  {
    Close(p_index);
    return;
  }
  if (p_result > 0)
  {
    connection.send_begin_ += static_cast<std::size_t>(p_result);
  }
  // No send is in flight now, so the sent bytes can go. The rest, with what was
  // appended meanwhile, is sent at the end of this iteration; until then
  // Append may write and compact as well.
  Compact(p_index);
  if (connection.send_end_ > 0)
  {
    MarkPendingFlush(p_index);
  }
}

void Gateway::ArmAccept()
{
  if (stop_.load(std::memory_order_relaxed))
  {
    return;
  }
  io_uring_sqe *const sqe = GetSqe(sockets_.ring_.Get());
  io_uring_prep_multishot_accept(sqe, sockets_.listener_.Get(), nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
  io_uring_sqe_set_data64(sqe, UserData(Op::Accept, 0, 0));
  ++uring_ops_;
}

void Gateway::ArmRecv(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  // Compaction leaves less than one frame, and the buffer holds several.
  contract_assert(connection.read_size_ < kReadBufferSize);
  contract_assert(!connection.recv_in_flight_);
  const std::span<std::byte> space = std::span{connection.read_}.subspan(connection.read_size_);
  io_uring_sqe *const sqe = GetSqe(sockets_.ring_.Get());
  io_uring_prep_recv(sqe, connection.fd_.Get(), space.data(), space.size(), 0);
  io_uring_sqe_set_data64(sqe, UserData(Op::Recv, connection.generation_, p_index));
  connection.recv_in_flight_ = true;
  ++uring_ops_;
}

void Gateway::SubmitSend(std::uint32_t p_index)
{
  Connection &connection = connections_[p_index];
  if (connection.send_in_flight_ || connection.send_begin_ == connection.send_end_)
  {
    return;
  }
  const std::span<const std::byte> unsent = std::span{connection.send_}.subspan(
      connection.send_begin_, connection.send_end_ - connection.send_begin_);
  io_uring_sqe *const sqe = GetSqe(sockets_.ring_.Get());
  io_uring_prep_send(sqe, connection.fd_.Get(), unsent.data(), unsent.size(), MSG_NOSIGNAL);
  io_uring_sqe_set_data64(sqe, UserData(Op::Send, connection.generation_, p_index));
  connection.send_in_flight_ = true;
  ++uring_ops_;
}

void Gateway::SubmitCancel(std::uint64_t p_user_data, int p_flags)
{
  io_uring_sqe *const sqe = GetSqe(sockets_.ring_.Get());
  io_uring_prep_cancel64(sqe, p_user_data, p_flags);
  io_uring_sqe_set_data64(sqe, UserData(Op::Cancel, 0, 0));
}

} // namespace mex
