#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "protocol.hpp"
#include "spsc_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/epoll.h>
#include <utility>
#include <vector>

// Gateway thread (prep/io-design-spec.md, step 6). It owns every socket: it
// accepts clients, checks logon, reassembles and validates frames, routes each
// order to its engine with the next op number, and writes the engines' reports
// back to the sessions.
// It never blocks: a full inbound queue is a SystemBusy reject, and a client
// whose send buffer is full is disconnected.
//
// Two client I/O backends share all of this logic and differ only in how bytes
// move: level-triggered epoll with recv/send syscalls, or io_uring with one
// outstanding recv and at most one outstanding send per connection.

struct io_uring;

namespace mex
{

// Owns one file descriptor; -1 means none.
class UniqueFd final
{
public:
  UniqueFd() = default;
  explicit UniqueFd(int p_fd) : fd_(p_fd) {}
  ~UniqueFd() { Reset(); }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&p_other) noexcept : fd_(std::exchange(p_other.fd_, -1)) {}
  UniqueFd &operator=(UniqueFd &&p_other) noexcept;

  [[nodiscard]] int Get() const { return fd_; }
  [[nodiscard]] bool IsOpen() const { return fd_ >= 0; }
  void Reset();

private:
  int fd_ = -1;
};

// Owns one io_uring instance; empty means none. io_uring is complete only in
// gateway.cpp, so the members that destroy it are defined there.
class UniqueRing final
{
public:
  UniqueRing();
  ~UniqueRing();
  UniqueRing(const UniqueRing &) = delete;
  UniqueRing &operator=(const UniqueRing &) = delete;
  UniqueRing(UniqueRing &&p_other) noexcept;
  UniqueRing &operator=(UniqueRing &&p_other) noexcept;

  // Startup only. p_flags are IORING_SETUP_* flags. Returns the error number on failure.
  [[nodiscard]] static std::expected<UniqueRing, int>
  Create(std::uint32_t p_sq_entries, std::uint32_t p_cq_entries, std::uint32_t p_flags);

  [[nodiscard]] io_uring *Get() const { return ring_.get(); }
  void Reset();

private:
  std::unique_ptr<io_uring> ring_{}; // set only after a successful setup
};

enum class GatewayIo : std::uint8_t
{
  Epoll = 0,
  Uring,
};

// The listening socket and the poller of the chosen backend: an epoll set that
// already watches the listener, or a ring that the gateway thread enables.
struct GatewaySockets
{
  UniqueFd listener_{};
  UniqueFd epoll_{};       // GatewayIo::Epoll only
  UniqueRing ring_{};      // GatewayIo::Uring only
  std::uint16_t port_ = 0; // the bound port; differs from the config when it asks for 0
  GatewayIo io_ = GatewayIo::Epoll;
};

// Startup only. Binds and listens on the configured address and port.
[[nodiscard]] std::expected<GatewaySockets, std::string>
OpenGatewaySockets(const GatewayConfig &p_config, GatewayIo p_io);

// The queue pair of one engine. The gateway is the producer of inbound_ and
// the consumer of outbound_.
struct EngineQueues
{
  SpscQueue<Command> &inbound_;
  SpscQueue<Report> &outbound_;
};

// Per-connection buffers, fixed at startup. A frame that does not fit in the
// send buffer, after a write attempt, disconnects that client. With epoll the
// kernel socket buffer comes first, so this buffer only holds what the kernel
// refused. With io_uring it also holds what was appended while a send is in flight.
constexpr std::size_t kReadBufferSize = 64 * wire::kMaxFrameSize;
constexpr std::size_t kSendBufferSize = 1024 * wire::kMaxFrameSize;

// clang-format off
class Gateway final
{
public:
  // p_queues[i] belongs to p_config.engines_[i]. All storage is allocated here.
  Gateway(const Config &p_config, GatewaySockets p_sockets, std::span<const EngineQueues> p_queues)
    pre (p_queues.size() == p_config.engines_.size())
    pre (p_config.gateway_.max_connections_ > 0)
    pre (p_sockets.io_ != GatewayIo::Uring || p_sockets.ring_.Get() != nullptr);
  Gateway(const Gateway &) = delete;
  Gateway &operator=(const Gateway &) = delete;
  Gateway(Gateway &&) = delete;
  Gateway &operator=(Gateway &&) = delete;
  ~Gateway();

  // Gateway thread only, called once. Busy-polls sockets and outbound queues
  // until Stop(). With io_uring it then cancels its I/O and waits for it.
  void Run();

  // Any thread.
  void Stop();

  // Any thread. Reports dropped because their session had no connection.
  [[nodiscard]] std::uint64_t DroppedReports() const;

private:
  struct Connection;
  struct Session
  {
    std::optional<std::uint32_t> connection_{}; // index into connections_
    std::uint64_t next_seq_ = 1;
  };

  void PollOnce();
  void PollEpoll();
  void Accept();
  // Sets up an accepted socket in a free slot, or closes it if there is none.
  void Adopt(UniqueFd p_fd);
  void OnReadable(std::uint32_t p_index);
  // Handles every complete frame in the read buffer, then keeps the partial rest.
  void ProcessFrames(std::uint32_t p_index);
  void OnFrame(std::uint32_t p_index, const wire::ClientMessage &p_message);
  void OnLogon(std::uint32_t p_index, const wire::Logon &p_logon);
  template <typename O> void OnOrder(SessionId p_session, const O &p_order);
  void DrainOutbound();

  // Encodes p_message with the session's next seq, or drops it if the session
  // has no connection. Disconnects the client if the frame does not fit.
  template <typename M> void SendToSession(SessionId p_session, const M &p_message);

  // Encodes p_message; returns false if it does not fit even after a write.
  template <typename M> [[nodiscard]] bool Append(std::uint32_t p_index, std::uint64_t p_seq, const M &p_message);

  // Epoll: writes what the kernel takes and waits for EPOLLOUT for the rest.
  // io_uring: submits a send of the unsent bytes unless one is in flight.
  // Returns false if it closed the connection.
  bool Flush(std::uint32_t p_index);
  bool FlushEpoll(std::uint32_t p_index);
  // Writes what the kernel takes now. Returns false if it closed the connection.
  bool Write(std::uint32_t p_index);
  // For a frame that does not fit: writes what the kernel takes now unless an
  // io_uring send is in flight, then drops the sent bytes. Returns false if it
  // closed the connection.
  bool MakeRoom(std::uint32_t p_index);
  // Moves the unsent bytes to the start of the send buffer. No send may be in flight.
  void Compact(std::uint32_t p_index);
  void FlushPending();
  void MarkPendingFlush(std::uint32_t p_index);
  [[nodiscard]] bool SetWriteInterest(std::uint32_t p_index, bool p_want_write);

  // io_uring backend. The kernel owns a connection's read buffer while its recv
  // is in flight, and the in-flight range of its send buffer while its send is.
  void StartUring();
  void PollUring();
  void StopUring();
  void OnCompletion(std::uint64_t p_user_data, std::int32_t p_result, std::uint32_t p_flags);
  void OnAccepted(std::int32_t p_result, std::uint32_t p_flags);
  void OnReceived(std::uint32_t p_index, std::int32_t p_result);
  void OnSent(std::uint32_t p_index, std::int32_t p_result);
  void ArmAccept();
  void ArmRecv(std::uint32_t p_index);
  void SubmitSend(std::uint32_t p_index);
  void SubmitCancel(std::uint64_t p_user_data, int p_flags);

  // Frees the connection and its session. Does nothing if already closed. The
  // slot is reused only after the kernel has returned all of its buffers.
  void Close(std::uint32_t p_index);
  void ReleaseIfIdle(std::uint32_t p_index);

  std::vector<SymbolConfig> routes_{}; // sorted by symbol_id_
  std::vector<SessionConfig> session_configs_{};
  std::vector<Session> sessions_{};
  std::vector<EngineQueues> queues_{};
  // The last op number given to a command, over all engines. This counter
  // stands in for the replicated log, which will assign the numbers later.
  OpNumber last_op_{};
  std::unique_ptr<Connection[]> connections_;
  std::vector<std::uint32_t> free_connections_{};
  std::vector<std::uint32_t> pending_flush_{}; // connections with unsent bytes to try
  GatewaySockets sockets_;
  static constexpr std::size_t kMaxEvents = 64;
  std::array<epoll_event, kMaxEvents> events_{};
  std::uint32_t uring_ops_ = 0; // accept, recv, and send ops not yet completed
  std::uint32_t uring_cq_entries_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> dropped_reports_{0};
};
// clang-format on

} // namespace mex
