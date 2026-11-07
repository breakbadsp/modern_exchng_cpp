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
// order to its engine, and writes the engines' reports back to the sessions.
// It never blocks: a full inbound queue is a SystemBusy reject, and a client
// whose send buffer is full is disconnected.
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

// The listening socket and the epoll set that already watches it.
struct GatewaySockets
{
  UniqueFd listener_{};
  UniqueFd epoll_{};
  std::uint16_t port_ = 0; // the bound port; differs from the config when it asks for 0
};

// Startup only. Binds and listens on the configured address and port.
[[nodiscard]] std::expected<GatewaySockets, std::string>
OpenGatewaySockets(const GatewayConfig &p_config);

// The queue pair of one engine. The gateway is the producer of inbound_ and
// the consumer of outbound_.
struct EngineQueues
{
  SpscQueue<Command> &inbound_;
  SpscQueue<Report> &outbound_;
};

// Per-connection buffers, fixed at startup. A frame that does not fit in the
// send buffer, after a write attempt, disconnects that client. The kernel
// socket buffer comes first, so this buffer only holds what the kernel refused.
constexpr std::size_t kReadBufferSize = 64 * wire::kMaxFrameSize;
constexpr std::size_t kSendBufferSize = 1024 * wire::kMaxFrameSize;

// clang-format off
class Gateway final
{
public:
  // p_queues[i] belongs to p_config.engines_[i]. All storage is allocated here.
  Gateway(const Config &p_config, GatewaySockets p_sockets, std::span<const EngineQueues> p_queues)
    pre (p_queues.size() == p_config.engines_.size())
    pre (p_config.gateway_.max_connections_ > 0);
  Gateway(const Gateway &) = delete;
  Gateway &operator=(const Gateway &) = delete;
  Gateway(Gateway &&) = delete;
  Gateway &operator=(Gateway &&) = delete;
  ~Gateway();

  // Gateway thread only. Busy-polls sockets and outbound queues until Stop().
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
  void Accept();
  void OnReadable(std::uint32_t p_index);
  void OnFrame(std::uint32_t p_index, const wire::ClientMessage &p_message);
  void OnLogon(std::uint32_t p_index, const wire::Logon &p_logon);
  template <typename O> void OnOrder(SessionId p_session, const O &p_order);
  void DrainOutbound();

  // Encodes p_message with the session's next seq, or drops it if the session
  // has no connection. Disconnects the client if the frame does not fit.
  template <typename M> void SendToSession(SessionId p_session, const M &p_message);

  // Encodes p_message; returns false if it does not fit even after a write.
  template <typename M> [[nodiscard]] bool Append(std::uint32_t p_index, std::uint64_t p_seq, const M &p_message);

  // Writes what the kernel takes and waits for EPOLLOUT for the rest. Returns
  // false if it closed the connection.
  bool Flush(std::uint32_t p_index);
  void FlushPending();
  [[nodiscard]] bool SetWriteInterest(std::uint32_t p_index, bool p_want_write);

  // Frees the connection and its session. Does nothing if already closed.
  void Close(std::uint32_t p_index);

  std::vector<SymbolConfig> routes_{}; // sorted by symbol_id_
  std::vector<SessionConfig> session_configs_{};
  std::vector<Session> sessions_{};
  std::vector<EngineQueues> queues_{};
  std::unique_ptr<Connection[]> connections_;
  std::vector<std::uint32_t> free_connections_{};
  std::vector<std::uint32_t> pending_flush_{}; // connections with unsent bytes to try
  GatewaySockets sockets_;
  static constexpr std::size_t kMaxEvents = 64;
  std::array<epoll_event, kMaxEvents> events_{};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> dropped_reports_{0};
};
// clang-format on

} // namespace mex
