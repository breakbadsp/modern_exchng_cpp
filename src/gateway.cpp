#include "gateway.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
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

[[nodiscard]] std::unexpected<std::string> SystemError(std::string_view p_what)
{
  return std::unexpected(std::format("{}: {}", p_what, std::system_category().message(errno)));
}

[[nodiscard]] bool WouldBlock(int p_errno) { return p_errno == EAGAIN || p_errno == EWOULDBLOCK; }

[[nodiscard]] bool InBand(const SymbolConfig &p_route, Price p_price)
{
  return p_route.min_price_ <= p_price && p_price <= p_route.max_price_;
}

// Gateway checks that need no order state. nullopt: the order may go to the engine.
[[nodiscard]] std::optional<wire::Reason> Check(const SymbolConfig &p_route,
                                                const wire::NewOrder &p_order)
{
  if (p_order.qty_.units_ == 0)
  {
    return wire::Reason::InvalidQuantity;
  }
  if (p_order.type_ == wire::OrderType::Limit && !InBand(p_route, p_order.price_))
  {
    return wire::Reason::PriceOutOfRange;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<wire::Reason> Check(const SymbolConfig &, const wire::Cancel &)
{
  return std::nullopt;
}

[[nodiscard]] std::optional<wire::Reason> Check(const SymbolConfig &p_route,
                                                const wire::Replace &p_replace)
{
  if (p_replace.new_total_qty_.units_ == 0)
  {
    return wire::Reason::InvalidQuantity;
  }
  if (!InBand(p_route, p_replace.new_price_))
  {
    return wire::Reason::PriceOutOfRange;
  }
  return std::nullopt;
}

// The reject the engine would send for each order type.
[[nodiscard]] wire::Reject RejectOf(const wire::NewOrder &p_order, wire::Reason p_reason)
{
  return {.client_order_id_ = p_order.client_order_id_,
          .symbol_id_ = p_order.symbol_id_,
          .reason_ = p_reason};
}

[[nodiscard]] wire::CancelReject RejectOf(const wire::Cancel &p_cancel, wire::Reason p_reason)
{
  return {.client_order_id_ = p_cancel.client_order_id_,
          .symbol_id_ = p_cancel.symbol_id_,
          .reason_ = p_reason};
}

[[nodiscard]] wire::ReplaceReject RejectOf(const wire::Replace &p_replace, wire::Reason p_reason)
{
  return {.client_order_id_ = p_replace.client_order_id_,
          .symbol_id_ = p_replace.symbol_id_,
          .reason_ = p_reason};
}

} // namespace

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

std::expected<GatewaySockets, std::string> OpenGatewaySockets(const GatewayConfig &p_config)
{
  GatewaySockets sockets{};
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
}

Gateway::~Gateway() = default;

void Gateway::Run()
{
  while (!stop_.load(std::memory_order_relaxed))
  {
    PollOnce();
  }
}

void Gateway::Stop() { stop_.store(true, std::memory_order_relaxed); }

std::uint64_t Gateway::DroppedReports() const
{
  return dropped_reports_.load(std::memory_order_relaxed);
}

void Gateway::PollOnce()
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
      static_cast<void>(Flush(index));
    }
  }
  DrainOutbound();
  FlushPending();
}

void Gateway::Accept()
{
  // A connection beyond max_connections, or one that cannot be set up, is
  // closed when fd goes out of scope.
  UniqueFd fd{::accept4(sockets_.listener_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
  if (!fd.IsOpen() || free_connections_.empty())
  {
    return;
  }
  const int one = 1;
  if (::setsockopt(fd.Get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
  {
    return;
  }
  const std::uint32_t index = free_connections_.back();
  epoll_event event{.events = EPOLLIN, .data = {.u64 = index}};
  if (::epoll_ctl(sockets_.epoll_.Get(), EPOLL_CTL_ADD, fd.Get(), &event) != 0)
  {
    return;
  }
  free_connections_.pop_back();
  connections_[index].fd_ = std::move(fd);
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
  const Command command{.session_id_ = p_session, .symbol_slot_ = route->slot_, .order_ = p_order};
  if (!queues_[route->engine_].inbound_.TryPush(command))
  {
    SendToSession(p_session, RejectOf(p_order, wire::Reason::SystemBusy));
  }
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
    if (!Flush(p_index))
    {
      return false;
    }
    std::memmove(connection.send_.data(), connection.send_.data() + connection.send_begin_,
                 connection.send_end_ - connection.send_begin_);
    connection.send_end_ -= connection.send_begin_;
    connection.send_begin_ = 0;
    if (kSendBufferSize - connection.send_end_ < kSize)
    {
      return false;
    }
  }
  static_cast<void>(
      wire::Encode(p_seq, p_message, std::span{connection.send_}.subspan(connection.send_end_)));
  connection.send_end_ += kSize;
  // Close does not clear pending_flush_, so an index is in the list at most once.
  if (!connection.pending_flush_)
  {
    connection.pending_flush_ = true;
    pending_flush_.push_back(p_index);
  }
  return true;
}

bool Gateway::Flush(std::uint32_t p_index)
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
      return SetWriteInterest(p_index, true);
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
  return SetWriteInterest(p_index, false);
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
  connection.fd_.Reset(); // closing the fd also removes it from the epoll set
  connection.read_size_ = 0;
  connection.send_begin_ = 0;
  connection.send_end_ = 0;
  connection.want_write_ = false;
  free_connections_.push_back(p_index);
}

} // namespace mex
