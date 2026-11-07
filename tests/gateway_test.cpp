#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
#include "protocol.hpp"
#include "spsc_queue.hpp"
#include "types.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <span>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <variant>
#include <vector>

// Every test talks to a real Gateway thread over loopback TCP, as a client would.
namespace
{

using mex::Command;
using mex::Config;
using mex::Engine;
using mex::EngineQueues;
using mex::Gateway;
using mex::Price;
using mex::Quantity;
using mex::Report;
using mex::SessionId;
using mex::Side;
using mex::SpscQueue;
using mex::UniqueFd;
using mex::wire::Reason;

namespace wire = mex::wire;

constexpr SessionId kAlice = 0;
constexpr SessionId kBob = 1;

// A test that waits longer than this has failed; ctest's TIMEOUT backs it up.
constexpr auto kWait = std::chrono::seconds{10};
constexpr timeval kSocketTimeout{.tv_sec = 5, .tv_usec = 0};

// Two engines. Symbols 7 and 9 share engine 0 (slots 0 and 1); 8 is on engine 1.
constexpr std::string_view kConfigFormat = R"(
engine 0
engine 1
symbol 7 0 64 16 100 200
symbol 8 1 64 16 100 200
symbol 9 0 64 16 100 200
session alice alice-token
session bob bob-token
gateway address 127.0.0.1
gateway port 9000
gateway max_connections {}
gateway inbound_queue_capacity {}
gateway outbound_queue_capacity 64
)";

[[nodiscard]] wire::Name NameOf(std::string_view p_text)
{
  wire::Name name{};
  std::ranges::copy(p_text, name.begin());
  return name;
}

[[nodiscard]] bool WaitFor(const std::function<bool()> &p_condition)
{
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (!p_condition())
  {
    if (std::chrono::steady_clock::now() > deadline)
    {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

struct Options
{
  std::uint32_t max_connections_ = 4;
  std::uint32_t inbound_capacity_ = 64;
  bool run_engines_ = true; // false: the test itself is the engine side of the queues
};

// Gateway and engines on their own threads, wired with real queues.
class Exchange final
{
public:
  explicit Exchange(const Options &p_options)
  {
    auto config = mex::ParseConfig(
        std::format(kConfigFormat, p_options.max_connections_, p_options.inbound_capacity_));
    if (!config.has_value())
    {
      ADD_FAILURE() << "config line " << config.error().line_ << ": " << config.error().reason_;
      return;
    }
    config->gateway_.port_ = 0; // ephemeral
    std::vector<EngineQueues> queues;
    for (std::size_t i = 0; i < config->engines_.size(); ++i)
    {
      inbound_.push_back(
          std::make_unique<SpscQueue<Command>>(config->gateway_.inbound_queue_capacity_));
      outbound_.push_back(
          std::make_unique<SpscQueue<Report>>(config->gateway_.outbound_queue_capacity_));
      queues.push_back({*inbound_.back(), *outbound_.back()});
      engines_.push_back(std::make_unique<Engine>(config->engines_[i].books_, *inbound_.back(),
                                                  *outbound_.back()));
    }
    auto sockets = mex::OpenGatewaySockets(config->gateway_);
    if (!sockets.has_value())
    {
      ADD_FAILURE() << sockets.error();
      return;
    }
    port_ = sockets->port_;
    gateway_ = std::make_unique<Gateway>(*config, std::move(*sockets), queues);
    threads_.emplace_back([gateway = gateway_.get()] { gateway->Run(); });
    if (p_options.run_engines_)
    {
      for (const std::unique_ptr<Engine> &engine : engines_)
      {
        threads_.emplace_back([engine = engine.get()] { engine->Run(); });
      }
    }
  }

  Exchange(const Exchange &) = delete;
  Exchange &operator=(const Exchange &) = delete;
  Exchange(Exchange &&) = delete;
  Exchange &operator=(Exchange &&) = delete;

  ~Exchange()
  {
    if (gateway_ != nullptr)
    {
      gateway_->Stop();
    }
    for (const std::unique_ptr<Engine> &engine : engines_)
    {
      engine->Stop();
    }
    threads_.clear(); // join before the queues and the gateway go away
  }

  [[nodiscard]] std::uint16_t Port() const { return port_; }
  [[nodiscard]] Gateway &Gate() const { return *gateway_; }
  // Only when run_engines_ is false: the test is then the engine side.
  [[nodiscard]] SpscQueue<Command> &Inbound(std::size_t p_engine) const
  {
    return *inbound_[p_engine];
  }
  [[nodiscard]] SpscQueue<Report> &Outbound(std::size_t p_engine) const
  {
    return *outbound_[p_engine];
  }

private:
  std::vector<std::unique_ptr<SpscQueue<Command>>> inbound_{};
  std::vector<std::unique_ptr<SpscQueue<Report>>> outbound_{};
  std::vector<std::unique_ptr<Engine>> engines_{};
  std::unique_ptr<Gateway> gateway_{};
  std::uint16_t port_ = 0;
  std::vector<std::jthread> threads_{};
};

using Frame = wire::Decoded<wire::ExchangeMessage>;

// Blocking test client. Every read times out, so a lost frame fails the test.
class Client final
{
public:
  explicit Client(std::uint16_t p_port, std::optional<int> p_receive_buffer = std::nullopt)
      : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
  {
    const int one = 1;
    EXPECT_EQ(
        ::setsockopt(fd_.Get(), SOL_SOCKET, SO_RCVTIMEO, &kSocketTimeout, sizeof(kSocketTimeout)),
        0);
    EXPECT_EQ(
        ::setsockopt(fd_.Get(), SOL_SOCKET, SO_SNDTIMEO, &kSocketTimeout, sizeof(kSocketTimeout)),
        0);
    EXPECT_EQ(::setsockopt(fd_.Get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), 0);
    if (p_receive_buffer.has_value())
    {
      EXPECT_EQ(::setsockopt(fd_.Get(), SOL_SOCKET, SO_RCVBUF, &*p_receive_buffer, sizeof(int)), 0);
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(p_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd_.Get(), reinterpret_cast<const sockaddr *>(&address), sizeof(address)),
              0)
        << std::strerror(errno);
  }

  void SendBytes(std::span<const std::byte> p_bytes) const
  {
    while (!p_bytes.empty())
    {
      const ssize_t sent = ::send(fd_.Get(), p_bytes.data(), p_bytes.size(), MSG_NOSIGNAL);
      if (sent <= 0)
      {
        ADD_FAILURE() << "send: " << std::strerror(errno);
        return;
      }
      p_bytes = p_bytes.subspan(static_cast<std::size_t>(sent));
    }
  }

  template <typename M> void Send(const M &p_message) const
  {
    std::array<std::byte, wire::kMaxFrameSize> buffer{};
    SendBytes(std::span{buffer}.first(wire::Encode(p_message, buffer)));
  }

  template <typename M> void SendByteByByte(const M &p_message) const
  {
    std::array<std::byte, wire::kMaxFrameSize> buffer{};
    const std::size_t size = wire::Encode(p_message, buffer);
    for (std::size_t i = 0; i < size; ++i)
    {
      SendBytes(std::span{buffer}.subspan(i, 1));
    }
  }

  // The next frame, or nullopt on timeout, close, or a frame that does not decode.
  [[nodiscard]] std::optional<Frame> Read() const
  {
    std::array<std::byte, wire::kMaxFrameSize> buffer{};
    if (!ReadExactly(std::span{buffer}.first(wire::kHeaderSize)))
    {
      return std::nullopt;
    }
    std::uint16_t length = 0;
    std::memcpy(&length, buffer.data(), sizeof(length));
    if (length < wire::kHeaderSize || length > buffer.size() ||
        !ReadExactly(std::span{buffer}.subspan(wire::kHeaderSize, length - wire::kHeaderSize)))
    {
      return std::nullopt;
    }
    const Frame frame = wire::DecodeExchangeFrame(std::span{buffer}.first(length));
    if (frame.status_ != wire::DecodeStatus::Complete)
    {
      return std::nullopt;
    }
    return frame;
  }

  // True if the peer closed the connection before sending any more bytes.
  [[nodiscard]] bool IsClosedByPeer() const
  {
    std::byte byte{};
    const ssize_t received = ::recv(fd_.Get(), &byte, 1, 0);
    return received == 0 || (received < 0 && errno == ECONNRESET);
  }

  void Close() { fd_.Reset(); }

private:
  [[nodiscard]] bool ReadExactly(std::span<std::byte> p_out) const
  {
    const ssize_t received = ::recv(fd_.Get(), p_out.data(), p_out.size(), MSG_WAITALL);
    return received == static_cast<ssize_t>(p_out.size());
  }

  UniqueFd fd_;
};

// The next frame, which must be an M with seq p_seq. Returns M{} after a failure.
template <typename M> [[nodiscard]] M Next(const Client &p_client, std::uint64_t p_seq)
{
  const std::optional<Frame> frame = p_client.Read();
  if (!frame.has_value())
  {
    ADD_FAILURE() << "no frame (timeout, close, or bad frame); wanted type "
                  << static_cast<int>(M::kType) << " seq " << p_seq;
    return M{};
  }
  EXPECT_EQ(frame->seq_, p_seq);
  const M *message = std::get_if<M>(&frame->message_);
  if (message == nullptr)
  {
    ADD_FAILURE() << "got variant index " << frame->message_.index() << "; wanted type "
                  << static_cast<int>(M::kType);
    return M{};
  }
  return *message;
}

[[nodiscard]] wire::Logon LogonOf(std::string_view p_name, std::string_view p_token)
{
  return {.session_name_ = NameOf(p_name), .token_ = NameOf(p_token)};
}

[[nodiscard]] wire::Logon AliceLogon() { return LogonOf("alice", "alice-token"); }
[[nodiscard]] wire::Logon BobLogon() { return LogonOf("bob", "bob-token"); }

[[nodiscard]] wire::NewOrder Limit(wire::ClientOrderId p_id, wire::SymbolId p_symbol, Side p_side,
                                   std::int64_t p_price, std::uint32_t p_qty)
{
  return {.client_order_id_ = p_id,
          .symbol_id_ = p_symbol,
          .side_ = p_side,
          .type_ = wire::OrderType::Limit,
          .price_ = Price{p_price},
          .qty_ = Quantity{p_qty}};
}

[[nodiscard]] Report RejectFor(SessionId p_session, wire::ClientOrderId p_id)
{
  return {.session_id_ = p_session,
          .report_ =
              wire::Reject{.client_order_id_ = p_id, .symbol_id_ = 7, .reason_ = Reason::BookFull}};
}

void Push(SpscQueue<Report> &p_queue, const Report &p_report)
{
  EXPECT_TRUE(WaitFor([&] { return p_queue.TryPush(p_report); }));
}

// ---------------------------------------------------------------------------
// Logon

TEST(GatewayTest, LogonIsAckedWithSeqOne)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};

  alice.Send(AliceLogon());

  static_cast<void>(Next<wire::LogonAck>(alice, 1));
}

TEST(GatewayTest, BadTokenIsRejectedAndClosed)
{
  const Exchange exchange{{}};
  const Client client{exchange.Port()};

  client.Send(LogonOf("alice", "wrong"));

  EXPECT_EQ(Next<wire::LogonReject>(client, 1).reason_, Reason::BadCredentials);
  EXPECT_TRUE(client.IsClosedByPeer());
}

TEST(GatewayTest, UnknownSessionIsRejectedAndClosed)
{
  const Exchange exchange{{}};
  const Client client{exchange.Port()};

  client.Send(LogonOf("mallory", "alice-token"));

  EXPECT_EQ(Next<wire::LogonReject>(client, 1).reason_, Reason::BadCredentials);
  EXPECT_TRUE(client.IsClosedByPeer());
}

TEST(GatewayTest, SecondLogonForAnActiveSessionIsRejected)
{
  const Exchange exchange{{}};
  const Client first{exchange.Port()};
  first.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(first, 1));
  const Client second{exchange.Port()};

  second.Send(AliceLogon());

  EXPECT_EQ(Next<wire::LogonReject>(second, 1).reason_, Reason::AlreadyLoggedOn);
  EXPECT_TRUE(second.IsClosedByPeer());
  // The first connection still owns the session.
  first.Send(Limit(1, 42, Side::Buy, 150, 10));
  EXPECT_EQ(Next<wire::Reject>(first, 2).reason_, Reason::UnknownSymbol);
}

TEST(GatewayTest, OrderBeforeLogonClosesWithoutReply)
{
  const Exchange exchange{{}};
  const Client client{exchange.Port()};

  client.Send(Limit(1, 7, Side::Buy, 150, 10));

  EXPECT_TRUE(client.IsClosedByPeer());
}

TEST(GatewayTest, LogonOnALoggedOnConnectionCloses)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));

  alice.Send(AliceLogon());

  EXPECT_TRUE(alice.IsClosedByPeer());
}

TEST(GatewayTest, ConnectionBeyondMaxConnectionsIsClosed)
{
  const Exchange exchange{{.max_connections_ = 1}};
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));

  const Client extra{exchange.Port()};

  EXPECT_TRUE(extra.IsClosedByPeer());
  alice.Send(Limit(1, 42, Side::Buy, 150, 10));
  EXPECT_EQ(Next<wire::Reject>(alice, 2).reason_, Reason::UnknownSymbol);
}

// ---------------------------------------------------------------------------
// Framing

TEST(GatewayTest, MalformedFrameClosesTheConnection)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  const Client bob{exchange.Port()};
  bob.Send(BobLogon());
  static_cast<void>(Next<wire::LogonAck>(bob, 1));

  // Unknown message type 99, length 3.
  alice.SendBytes(std::array{std::byte{3}, std::byte{0}, std::byte{99}});
  wire::NewOrder bad_side = Limit(1, 7, Side::Buy, 150, 10);
  bad_side.side_ = static_cast<Side>(7);
  bob.Send(bad_side);

  EXPECT_TRUE(alice.IsClosedByPeer());
  EXPECT_TRUE(bob.IsClosedByPeer());
}

TEST(GatewayTest, PartialAndCoalescedFramesAreReassembled)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};

  alice.SendByteByByte(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  alice.SendByteByByte(Limit(1, 7, Side::Buy, 150, 10));
  const wire::Ack ack = Next<wire::Ack>(alice, 2);
  EXPECT_EQ(ack.client_order_id_, 1U);
  EXPECT_EQ(ack.leaves_, Quantity{10});

  // A Cancel and a NewOrder in one write.
  std::array<std::byte, 2 * wire::kMaxFrameSize> both{};
  std::size_t size = wire::Encode(wire::Cancel{.client_order_id_ = 1, .symbol_id_ = 7}, both);
  size += wire::Encode(Limit(2, 7, Side::Buy, 140, 5), std::span{both}.subspan(size));
  alice.SendBytes(std::span{both}.first(size));

  EXPECT_EQ(Next<wire::CancelAck>(alice, 3),
            (wire::CancelAck{.client_order_id_ = 1, .symbol_id_ = 7}));
  EXPECT_EQ(Next<wire::Ack>(alice, 4).client_order_id_, 2U);
}

// ---------------------------------------------------------------------------
// Validation and routing

TEST(GatewayTest, InvalidOrdersAreRejectedBeforeTheEngine)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));

  alice.Send(Limit(1, 42, Side::Buy, 150, 10));
  EXPECT_EQ(
      Next<wire::Reject>(alice, 2),
      (wire::Reject{.client_order_id_ = 1, .symbol_id_ = 42, .reason_ = Reason::UnknownSymbol}));
  alice.Send(wire::Cancel{.client_order_id_ = 2, .symbol_id_ = 42});
  EXPECT_EQ(Next<wire::CancelReject>(alice, 3),
            (wire::CancelReject{
                .client_order_id_ = 2, .symbol_id_ = 42, .reason_ = Reason::UnknownSymbol}));
  alice.Send(wire::Replace{.client_order_id_ = 3,
                           .symbol_id_ = 42,
                           .new_price_ = Price{150},
                           .new_total_qty_ = Quantity{5}});
  EXPECT_EQ(Next<wire::ReplaceReject>(alice, 4),
            (wire::ReplaceReject{
                .client_order_id_ = 3, .symbol_id_ = 42, .reason_ = Reason::UnknownSymbol}));

  alice.Send(Limit(4, 7, Side::Buy, 150, 0));
  EXPECT_EQ(
      Next<wire::Reject>(alice, 5),
      (wire::Reject{.client_order_id_ = 4, .symbol_id_ = 7, .reason_ = Reason::InvalidQuantity}));
  alice.Send(wire::Replace{.client_order_id_ = 5,
                           .symbol_id_ = 7,
                           .new_price_ = Price{150},
                           .new_total_qty_ = Quantity{0}});
  EXPECT_EQ(Next<wire::ReplaceReject>(alice, 6).reason_, Reason::InvalidQuantity);

  alice.Send(Limit(6, 7, Side::Buy, 99, 10));
  EXPECT_EQ(
      Next<wire::Reject>(alice, 7),
      (wire::Reject{.client_order_id_ = 6, .symbol_id_ = 7, .reason_ = Reason::PriceOutOfRange}));
  alice.Send(Limit(7, 7, Side::Sell, 201, 10));
  EXPECT_EQ(Next<wire::Reject>(alice, 8).reason_, Reason::PriceOutOfRange);
  alice.Send(wire::Replace{.client_order_id_ = 8,
                           .symbol_id_ = 7,
                           .new_price_ = Price{201},
                           .new_total_qty_ = Quantity{5}});
  EXPECT_EQ(Next<wire::ReplaceReject>(alice, 9),
            (wire::ReplaceReject{
                .client_order_id_ = 8, .symbol_id_ = 7, .reason_ = Reason::PriceOutOfRange}));

  // Nothing above reached an engine: the next frames are this order's reports.
  alice.Send(Limit(9, 7, Side::Buy, 200, 10)); // the band is inclusive
  const wire::Ack ack = Next<wire::Ack>(alice, 10);
  EXPECT_EQ(ack.client_order_id_, 9U);
  EXPECT_EQ(ack.leaves_, Quantity{10});
  // A market order's price is not checked; on an empty book its remainder is Done.
  alice.Send(wire::NewOrder{.client_order_id_ = 10,
                            .symbol_id_ = 8,
                            .side_ = Side::Buy,
                            .type_ = wire::OrderType::Market,
                            .price_ = Price{0},
                            .qty_ = Quantity{5}});
  EXPECT_EQ(
      Next<wire::Done>(alice, 11),
      (wire::Done{.client_order_id_ = 10, .symbol_id_ = 8, .reason_ = Reason::MarketRemainder}));
}

TEST(GatewayTest, OrdersAreRoutedToTheirEngineAndSlot)
{
  const Exchange exchange{{.run_engines_ = false}};
  const Client alice{exchange.Port()};
  const Client bob{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  bob.Send(BobLogon());
  static_cast<void>(Next<wire::LogonAck>(bob, 1));

  const wire::NewOrder on_9 = Limit(1, 9, Side::Buy, 150, 10);
  const wire::Cancel on_8{.client_order_id_ = 2, .symbol_id_ = 8};
  alice.Send(on_9);
  bob.Send(on_8);

  std::optional<Command> engine_0{};
  std::optional<Command> engine_1{};
  EXPECT_TRUE(WaitFor([&] { return (engine_0 = exchange.Inbound(0).TryPop()).has_value(); }));
  EXPECT_TRUE(WaitFor([&] { return (engine_1 = exchange.Inbound(1).TryPop()).has_value(); }));
  ASSERT_TRUE(engine_0.has_value() && engine_1.has_value());
  EXPECT_EQ(engine_0->session_id_, kAlice);
  EXPECT_EQ(engine_0->symbol_slot_, 1U);
  EXPECT_EQ(engine_0->order_, mex::ClientOrder{on_9});
  EXPECT_EQ(engine_1->session_id_, kBob);
  EXPECT_EQ(engine_1->symbol_slot_, 0U);
  EXPECT_EQ(engine_1->order_, mex::ClientOrder{on_8});
}

TEST(GatewayTest, FullInboundQueueRejectsWithSystemBusy)
{
  const Exchange exchange{{.inbound_capacity_ = 1, .run_engines_ = false}};
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));

  alice.Send(Limit(1, 7, Side::Buy, 150, 10)); // fills engine 0's ring
  alice.Send(Limit(2, 9, Side::Buy, 150, 10));
  alice.Send(wire::Cancel{.client_order_id_ = 1, .symbol_id_ = 7});
  alice.Send(wire::Replace{.client_order_id_ = 1,
                           .symbol_id_ = 7,
                           .new_price_ = Price{150},
                           .new_total_qty_ = Quantity{5}});

  EXPECT_EQ(Next<wire::Reject>(alice, 2),
            (wire::Reject{.client_order_id_ = 2, .symbol_id_ = 9, .reason_ = Reason::SystemBusy}));
  EXPECT_EQ(
      Next<wire::CancelReject>(alice, 3),
      (wire::CancelReject{.client_order_id_ = 1, .symbol_id_ = 7, .reason_ = Reason::SystemBusy}));
  EXPECT_EQ(
      Next<wire::ReplaceReject>(alice, 4),
      (wire::ReplaceReject{.client_order_id_ = 1, .symbol_id_ = 7, .reason_ = Reason::SystemBusy}));
  // The first order is the one in the ring; engine 1's ring is still free.
  const std::optional<Command> queued = exchange.Inbound(0).TryPop();
  ASSERT_TRUE(queued.has_value());
  EXPECT_EQ(queued->order_, mex::ClientOrder{Limit(1, 7, Side::Buy, 150, 10)});
  EXPECT_FALSE(exchange.Inbound(0).TryPop().has_value());
}

// ---------------------------------------------------------------------------
// Outbound

TEST(GatewayTest, SlowConsumerIsDisconnectedWithoutStallingOthers)
{
  const Exchange exchange{{.run_engines_ = false}};
  constexpr int kSmallReceiveBuffer = 4096;
  const Client alice{exchange.Port(), kSmallReceiveBuffer};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  const Client bob{exchange.Port()};
  bob.Send(BobLogon());
  static_cast<void>(Next<wire::LogonAck>(bob, 1));

  // Alice does not read. Her reports fill the kernel buffers, then the
  // gateway's send buffer; then she is disconnected and the rest are dropped.
  wire::ClientOrderId pushed = 0;
  const bool dropped = WaitFor(
      [&]
      {
        if (exchange.Outbound(0).TryPush(RejectFor(kAlice, pushed + 1)))
        {
          ++pushed;
        }
        return exchange.Gate().DroppedReports() > 0;
      });
  ASSERT_TRUE(dropped);

  // Alice gets a gap-free prefix of her reports, then the close.
  std::uint64_t expected_seq = 2;
  while (const std::optional<Frame> frame = alice.Read())
  {
    ASSERT_EQ(frame->seq_, expected_seq);
    ++expected_seq;
  }
  EXPECT_LT(expected_seq - 2, pushed);
  EXPECT_TRUE(alice.IsClosedByPeer());

  Push(exchange.Outbound(1), RejectFor(kBob, 77));
  EXPECT_EQ(Next<wire::Reject>(bob, 2).client_order_id_, 77U);
}

TEST(GatewayTest, ReportForASessionWithoutConnectionIsDropped)
{
  const Exchange exchange{{.run_engines_ = false}};

  Push(exchange.Outbound(0), RejectFor(kBob, 1)); // bob never logged on
  EXPECT_TRUE(WaitFor([&] { return exchange.Gate().DroppedReports() == 1; }));

  {
    const Client alice{exchange.Port()};
    alice.Send(AliceLogon());
    static_cast<void>(Next<wire::LogonAck>(alice, 1));
    alice.SendBytes(std::array{std::byte{3}, std::byte{0}, std::byte{99}});
    ASSERT_TRUE(alice.IsClosedByPeer()); // the gateway has freed the session
  }
  Push(exchange.Outbound(0), RejectFor(kAlice, 2));
  EXPECT_TRUE(WaitFor([&] { return exchange.Gate().DroppedReports() == 2; }));

  // A new logon starts at seq 1 and does not get the dropped report.
  const Client alice{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  Push(exchange.Outbound(0), RejectFor(kAlice, 3));
  EXPECT_EQ(Next<wire::Reject>(alice, 2).client_order_id_, 3U);
}

TEST(GatewayTest, SessionKeepsItsRestingOrderAcrossReconnect)
{
  const Exchange exchange{{}};
  {
    const Client alice{exchange.Port()};
    alice.Send(AliceLogon());
    static_cast<void>(Next<wire::LogonAck>(alice, 1));
    alice.Send(Limit(1, 7, Side::Buy, 150, 10));
    EXPECT_EQ(Next<wire::Ack>(alice, 2).leaves_, Quantity{10});
  } // disconnect

  // The gateway may not have seen the close yet; retry until the session is free.
  std::optional<Client> alice{};
  EXPECT_TRUE(WaitFor(
      [&]
      {
        alice.emplace(exchange.Port());
        alice->Send(AliceLogon());
        const std::optional<Frame> frame = alice->Read();
        return frame.has_value() && std::holds_alternative<wire::LogonAck>(frame->message_) &&
               frame->seq_ == 1;
      }));

  alice->Send(wire::Cancel{.client_order_id_ = 1, .symbol_id_ = 7});
  EXPECT_EQ(Next<wire::CancelAck>(*alice, 2),
            (wire::CancelAck{.client_order_id_ = 1, .symbol_id_ = 7}));
}

TEST(GatewayTest, EndToEndTradeWithSeqPerSession)
{
  const Exchange exchange{{}};
  const Client alice{exchange.Port()};
  const Client bob{exchange.Port()};
  alice.Send(AliceLogon());
  static_cast<void>(Next<wire::LogonAck>(alice, 1));
  bob.Send(BobLogon());
  static_cast<void>(Next<wire::LogonAck>(bob, 1));

  alice.Send(Limit(1, 7, Side::Buy, 150, 10));
  const wire::Ack ack = Next<wire::Ack>(alice, 2);
  EXPECT_EQ(ack, (wire::Ack{.client_order_id_ = 1,
                            .symbol_id_ = 7,
                            .exchange_order_id_ = ack.exchange_order_id_, // informational
                            .leaves_ = Quantity{10}}));
  alice.Send(wire::Replace{.client_order_id_ = 1,
                           .symbol_id_ = 7,
                           .new_price_ = Price{150},
                           .new_total_qty_ = Quantity{6}});
  EXPECT_EQ(
      Next<wire::ReplaceAck>(alice, 3),
      (wire::ReplaceAck{
          .client_order_id_ = 1, .symbol_id_ = 7, .price_ = Price{150}, .leaves_ = Quantity{6}}));

  bob.Send(Limit(1, 7, Side::Sell, 140, 4));
  EXPECT_EQ(Next<wire::Fill>(bob, 2), (wire::Fill{.exchange_order_id_ = mex::kNoExchangeOrderId,
                                                  .client_order_id_ = 1,
                                                  .symbol_id_ = 7,
                                                  .price_ = Price{150},
                                                  .qty_ = Quantity{4},
                                                  .leaves_ = Quantity{0},
                                                  .liquidity_ = wire::Liquidity::Taker}));
  EXPECT_EQ(Next<wire::Ack>(bob, 3).leaves_, Quantity{0});
  EXPECT_EQ(Next<wire::Fill>(alice, 4), (wire::Fill{.exchange_order_id_ = ack.exchange_order_id_,
                                                    .client_order_id_ = 1,
                                                    .symbol_id_ = 7,
                                                    .price_ = Price{150},
                                                    .qty_ = Quantity{4},
                                                    .leaves_ = Quantity{2},
                                                    .liquidity_ = wire::Liquidity::Maker}));

  alice.Send(wire::Cancel{.client_order_id_ = 1, .symbol_id_ = 7});
  EXPECT_EQ(Next<wire::CancelAck>(alice, 5),
            (wire::CancelAck{.client_order_id_ = 1, .symbol_id_ = 7}));
  bob.Send(Limit(2, 8, Side::Sell, 120, 3));
  const wire::Ack on_engine_1 = Next<wire::Ack>(bob, 4);
  EXPECT_EQ(on_engine_1.symbol_id_, 8U);
  EXPECT_EQ(on_engine_1.leaves_, Quantity{3});
}

} // namespace
