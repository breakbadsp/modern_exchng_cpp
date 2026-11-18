#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
#include "protocol.hpp"
#include "replication.hpp"
#include "spsc_queue.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/io/client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using mex::Command;
using mex::Config;
using mex::ExchangeStateMachine;
using mex::OrderBatch;
using mex::Report;
using mex::SessionId;
using mex::SpscQueue;
namespace wire = mex::wire;

constexpr std::string_view kConfigText = R"(engine 0
engine 1
symbol 1 0 64 16 1 1000
symbol 2 1 64 16 1 1000
symbol 3 0 32 8 1 1000
session alice a
session bob b
gateway address 127.0.0.1
gateway port 9000
gateway max_connections 4
gateway inbound_queue_capacity 1024
gateway outbound_queue_capacity 1024
replica 127.0.0.1:7001 9001
replica 127.0.0.1:7002 9002
replica 127.0.0.1:7003 9003
)";

constexpr std::size_t kBodyBytesMax = 4096;
constexpr std::size_t kReportQueueCapacity = 4096;

[[nodiscard]] Config TestConfig()
{
  auto config = mex::ParseConfig(kConfigText);
  EXPECT_TRUE(config.has_value());
  return *config;
}

// One state machine with its report queues, as one replica holds them.
struct Node
{
  explicit Node(const Config &p_config)
  {
    for (std::size_t index = 0; index < p_config.engines_.size(); ++index)
    {
      queues_.push_back(std::make_unique<SpscQueue<Report>>(kReportQueueCapacity));
      queue_pointers_.push_back(queues_.back().get());
    }
    machine_ = std::make_unique<ExchangeStateMachine>(p_config, queue_pointers_);
  }

  // Applies one operation. Returns the number of entries that it applied.
  [[nodiscard]] std::uint32_t Execute(std::uint64_t p_op, std::span<const std::byte> p_body,
                                      std::uint64_t p_timestamp_ns = 0)
  {
    std::array<std::byte, mex::kBatchReplyBytes> reply{};
    const std::size_t reply_bytes =
        machine_->Execute({.op_ = p_op, .timestamp_ns_ = p_timestamp_ns}, p_body, reply);
    EXPECT_EQ(reply_bytes, mex::kBatchReplyBytes);
    std::uint32_t applied_count = 0;
    std::memcpy(&applied_count, reply.data(), sizeof(applied_count));
    return applied_count;
  }

  [[nodiscard]] std::vector<Report> TakeReports()
  {
    std::vector<Report> reports;
    for (const auto &queue : queues_)
    {
      while (const std::optional<Report> report = queue->TryPop())
      {
        reports.push_back(*report);
      }
    }
    return reports;
  }

  std::vector<std::unique_ptr<SpscQueue<Report>>> queues_{};
  std::vector<SpscQueue<Report> *> queue_pointers_{};
  std::unique_ptr<ExchangeStateMachine> machine_{};
};

// Makes batches of orders that rest, match, cancel, and replace, over all
// symbols and both sessions. Request numbers increase for each session.
class BatchSource final
{
public:
  explicit BatchSource(std::uint32_t p_seed) : random_{p_seed} {}

  void FillBatch(OrderBatch &p_batch, std::uint32_t p_entry_count)
  {
    p_batch.Clear();
    for (std::uint32_t entry = 0; entry < p_entry_count; ++entry)
    {
      const auto session = static_cast<SessionId>(random_() % kSessionCount);
      const wire::RequestNumber request{++last_request_[session]};
      const auto symbol = static_cast<wire::SymbolId>(1 + random_() % kSymbolCount);
      const std::uint64_t client_order_id = 1 + random_() % kOrderIdCount;
      const mex::Price price{static_cast<std::int64_t>(95 + random_() % 10)};
      const mex::Quantity quantity{static_cast<std::uint32_t>(1 + random_() % 20)};
      mex::ClientOrder order;
      switch (random_() % 4)
      {
      case 0:
        order = wire::Cancel{
            .request_number_ = request, .client_order_id_ = client_order_id, .symbol_id_ = symbol};
        break;
      case 1:
        order = wire::Replace{.request_number_ = request,
                              .client_order_id_ = client_order_id,
                              .symbol_id_ = symbol,
                              .new_price_ = price,
                              .new_total_qty_ = quantity};
        break;
      default:
        order = wire::NewOrder{.request_number_ = request,
                               .client_order_id_ = client_order_id,
                               .symbol_id_ = symbol,
                               .side_ = random_() % 2 == 0 ? mex::Side::Buy : mex::Side::Sell,
                               .type_ = wire::OrderType::Limit,
                               .price_ = price,
                               .qty_ = quantity};
        break;
      }
      ASSERT_TRUE(p_batch.TryAppend(session, order));
    }
  }

private:
  static constexpr std::uint32_t kSessionCount = 2;
  static constexpr std::uint32_t kSymbolCount = 3;
  static constexpr std::uint32_t kOrderIdCount = 24;

  std::mt19937 random_;
  std::array<std::uint64_t, kSessionCount> last_request_{};
};

constexpr std::uint32_t kEntriesPerBatch = 16;

// Replicas that apply the same operations have the same state and give the
// same reports. A replica that starts from a snapshot continues the same way.
TEST(ExchangeStateMachine, ReplicasAgreeAndContinueFromSnapshot)
{
  const Config config = TestConfig();
  Node first{config};
  Node second{config};
  first.machine_->EmitReportsAfter(0, 0);
  second.machine_->EmitReportsAfter(0, 0);
  BatchSource source{7};
  OrderBatch batch{kBodyBytesMax};
  std::vector<std::byte> snapshot(first.machine_->SnapshotBytesMax());
  std::optional<Node> late;

  constexpr std::uint64_t kOpCount = 200;
  constexpr std::uint64_t kSnapshotOp = 80;
  std::uint64_t applied_total = 0;
  for (std::uint64_t op = 1; op <= kOpCount; ++op)
  {
    source.FillBatch(batch, kEntriesPerBatch);
    const std::uint32_t applied_count = first.Execute(op, batch.Bytes());
    applied_total += applied_count;
    EXPECT_EQ(second.Execute(op, batch.Bytes()), applied_count);
    const std::vector<Report> reports = first.TakeReports();
    EXPECT_GE(reports.size(), applied_count); // each applied order has a final report
    EXPECT_EQ(second.TakeReports(), reports);
    ASSERT_EQ(second.machine_->StateDigest(), first.machine_->StateDigest()) << "op " << op;
    if (late.has_value())
    {
      EXPECT_EQ(late->Execute(op, batch.Bytes()), applied_count);
      EXPECT_EQ(late->TakeReports(), reports);
      ASSERT_EQ(late->machine_->StateDigest(), first.machine_->StateDigest()) << "op " << op;
    }
    if (op == kSnapshotOp)
    {
      const auto snapshot_bytes = first.machine_->Snapshot(snapshot);
      ASSERT_TRUE(snapshot_bytes.has_value());
      late.emplace(config);
      late->machine_->EmitReportsAfter(0, 0);
      ASSERT_TRUE(late->machine_->Restore(std::span{snapshot}.first(*snapshot_bytes)));
      ASSERT_EQ(late->machine_->StateDigest(), first.machine_->StateDigest());
      // The snapshot bytes are a function of the state.
      std::vector<std::byte> again(snapshot.size());
      const auto again_bytes = late->machine_->Snapshot(again);
      ASSERT_EQ(again_bytes, snapshot_bytes);
      EXPECT_TRUE(std::equal(again.begin(),
                             again.begin() + static_cast<std::ptrdiff_t>(*again_bytes),
                             snapshot.begin()));
    }
  }
  EXPECT_GT(applied_total, kOpCount); // the batches did real work
}

// A replica that starts applies again the operations of its log. They
// change the state and push no report.
TEST(ExchangeStateMachine, PushesNoReportUpToTheStartOperation)
{
  const Config config = TestConfig();
  Node live{config};
  Node restarted{config};
  live.machine_->EmitReportsAfter(0, 0);
  constexpr std::uint64_t kLogHead = 10;
  constexpr std::uint64_t kStartNs = 1000;
  restarted.machine_->EmitReportsAfter(kLogHead, kStartNs);
  BatchSource source{11};
  OrderBatch batch{kBodyBytesMax};
  // Operations 11 and 12 are above the log head, but the primary made them
  // before this replica started: the replica missed them and catches up.
  constexpr std::uint64_t kFirstLiveOp = kLogHead + 3;
  for (std::uint64_t op = 1; op <= kLogHead + 5; ++op)
  {
    const std::uint64_t timestamp_ns = op < kFirstLiveOp ? kStartNs - 1 : kStartNs;
    source.FillBatch(batch, kEntriesPerBatch);
    static_cast<void>(live.Execute(op, batch.Bytes(), timestamp_ns));
    static_cast<void>(restarted.Execute(op, batch.Bytes(), timestamp_ns));
    const std::vector<Report> reports = live.TakeReports();
    EXPECT_FALSE(reports.empty());
    if (op < kFirstLiveOp)
    {
      EXPECT_TRUE(restarted.TakeReports().empty()) << "op " << op;
    }
    else
    {
      EXPECT_EQ(restarted.TakeReports(), reports) << "op " << op;
    }
    EXPECT_EQ(restarted.machine_->StateDigest(), live.machine_->StateDigest());
  }
}

// The report rule compares primary timestamps with the clock of the
// replica at its start. The tests below record what a clock difference
// does (docs/replication.md, "Limits"). A replica starts at kStartNs of
// true time with kSkewLogHead operations in its log. The primary stamps one
// operation each kOpIntervalNs of true time, at half intervals, so that no
// stamp is on a limit.
constexpr std::uint64_t kSkewLogHead = 10;
constexpr std::uint64_t kSkewStartNs = 1'000'000;
constexpr std::uint64_t kOpIntervalNs = 100;
constexpr std::uint64_t kClockDifferenceNs = 300;

// Applies operations 1 to p_last_op to a replica that pushes all reports
// and to a replica that starts with p_start_clock_ns. Operation
// p_first_live_op is the first that the primary made after the start.
// Returns, for each operation, whether the started replica pushed reports.
// Its reports must be those of the other replica or none.
[[nodiscard]] std::vector<bool> ReportsAfterStart(std::uint64_t p_start_clock_ns,
                                                  std::uint64_t p_first_live_op,
                                                  std::uint64_t p_last_op)
{
  const Config config = TestConfig();
  Node live{config};
  Node started{config};
  live.machine_->EmitReportsAfter(0, 0);
  started.machine_->EmitReportsAfter(kSkewLogHead, p_start_clock_ns);
  BatchSource source{13};
  OrderBatch batch{kBodyBytesMax};
  std::vector<bool> pushed{false}; // no operation 0
  for (std::uint64_t op = 1; op <= p_last_op; ++op)
  {
    // Operation p_first_live_op is half an interval after the start.
    const std::uint64_t timestamp_ns =
        kSkewStartNs + kOpIntervalNs / 2 + op * kOpIntervalNs - p_first_live_op * kOpIntervalNs;
    source.FillBatch(batch, kEntriesPerBatch);
    static_cast<void>(live.Execute(op, batch.Bytes(), timestamp_ns));
    static_cast<void>(started.Execute(op, batch.Bytes(), timestamp_ns));
    const std::vector<Report> reports = live.TakeReports();
    EXPECT_FALSE(reports.empty());
    const std::vector<Report> started_reports = started.TakeReports();
    pushed.push_back(!started_reports.empty());
    if (!started_reports.empty())
    {
      EXPECT_EQ(started_reports, reports) << "op " << op;
    }
    EXPECT_EQ(started.machine_->StateDigest(), live.machine_->StateDigest());
  }
  return pushed;
}

// A replica clock that is ahead of the primary drops the reports of the
// operations that the primary made after the start, for as long as the
// clock difference. The sessions of this replica are live, so these
// reports are lost.
TEST(ExchangeStateMachine, ReplicaClockAheadDropsReportsOfNewOperations)
{
  constexpr std::uint64_t kFirstLiveOp = kSkewLogHead + 1;
  constexpr std::uint64_t kLastOp = kFirstLiveOp + 5;
  const std::vector<bool> pushed =
      ReportsAfterStart(kSkewStartNs + kClockDifferenceNs, kFirstLiveOp, kLastOp);
  // Operations 11 to 13 are stamped 50 to 250 ns after the start: below
  // the 300 ns difference.
  const std::vector<bool> expected{false, false, false, false, false, false, false, false, false,
                                   false, false, false, false, false, true,  true,  true};
  EXPECT_EQ(pushed, expected);
}

// A replica clock that is behind the primary pushes the reports of
// operations that the primary made before the start, if they are above the
// log head and in the clock difference. Another process already sent these
// reports, so the sessions get them again.
TEST(ExchangeStateMachine, ReplicaClockBehindRepeatsReportsOfOlderOperations)
{
  // The replica missed operations 11 to 15, and applies them after its
  // start to catch up.
  constexpr std::uint64_t kFirstLiveOp = kSkewLogHead + 6;
  constexpr std::uint64_t kLastOp = kFirstLiveOp + 2;
  const std::vector<bool> pushed =
      ReportsAfterStart(kSkewStartNs - kClockDifferenceNs, kFirstLiveOp, kLastOp);
  // Operations 13 to 15 are stamped 250 to 50 ns before the start: in the
  // 300 ns difference. Operations 1 to 10 are in the log and push
  // nothing at any timestamp.
  const std::vector<bool> expected{false, false, false, false, false, false, false,
                                   false, false, false, false, false, false, true,
                                   true,  true,  true,  true,  true};
  EXPECT_EQ(pushed, expected);
}

// The body of an operation is bytes from the network. Entries that are not
// valid are not applied, and no body breaks a contract.
TEST(ExchangeStateMachine, SkipsEntriesThatAreNotValid)
{
  const Config config = TestConfig();
  Node node{config};
  node.machine_->EmitReportsAfter(0, 0);
  const std::uint64_t empty_digest = node.machine_->StateDigest();
  const wire::NewOrder valid{.request_number_ = wire::RequestNumber{1},
                             .client_order_id_ = 1,
                             .symbol_id_ = 1,
                             .side_ = mex::Side::Buy,
                             .type_ = wire::OrderType::Limit,
                             .price_ = mex::Price{100},
                             .qty_ = mex::Quantity{5}};
  OrderBatch batch{kBodyBytesMax};
  std::uint64_t op = 0;

  wire::NewOrder unknown_symbol = valid;
  unknown_symbol.symbol_id_ = 99;
  wire::NewOrder zero_quantity = valid;
  zero_quantity.qty_ = mex::Quantity{0};
  wire::NewOrder out_of_band = valid;
  out_of_band.price_ = mex::Price{1001};
  for (const wire::NewOrder &order : {unknown_symbol, zero_quantity, out_of_band})
  {
    batch.Clear();
    ASSERT_TRUE(batch.TryAppend(0, order));
    EXPECT_EQ(node.Execute(++op, batch.Bytes()), 0U);
  }
  constexpr SessionId kUnknownSession = 2;
  batch.Clear();
  ASSERT_TRUE(batch.TryAppend(kUnknownSession, valid));
  EXPECT_EQ(node.Execute(++op, batch.Bytes()), 0U);
  EXPECT_TRUE(node.TakeReports().empty());
  EXPECT_EQ(node.machine_->StateDigest(), empty_digest);

  // Every prefix of a valid batch, and random bytes.
  batch.Clear();
  ASSERT_TRUE(batch.TryAppend(0, valid));
  const std::vector<std::byte> one_entry{batch.Bytes().begin(), batch.Bytes().end()};
  for (std::size_t size = 0; size < one_entry.size(); ++size)
  {
    EXPECT_EQ(node.Execute(++op, std::span{one_entry}.first(size)), 0U) << "size " << size;
  }
  std::mt19937 random{3};
  std::vector<std::byte> noise(256);
  for (int round = 0; round < 200; ++round)
  {
    for (std::byte &byte : noise)
    {
      byte = static_cast<std::byte>(random());
    }
    static_cast<void>(node.Execute(++op, noise));
  }

  // A valid entry after entries that are not valid is applied.
  batch.Clear();
  ASSERT_TRUE(batch.TryAppend(0, unknown_symbol));
  wire::NewOrder later = valid;
  later.request_number_ = wire::RequestNumber{1'000'000};
  later.client_order_id_ = 777;
  ASSERT_TRUE(batch.TryAppend(1, later));
  EXPECT_EQ(node.Execute(++op, batch.Bytes()), 1U);
}

TEST(ExchangeStateMachine, RestoreRefusesBytesThatAreNotASnapshot)
{
  const Config config = TestConfig();
  Node source{config};
  source.machine_->EmitReportsAfter(0, 0);
  BatchSource orders{5};
  OrderBatch batch{kBodyBytesMax};
  orders.FillBatch(batch, kEntriesPerBatch);
  static_cast<void>(source.Execute(1, batch.Bytes()));
  std::vector<std::byte> snapshot(source.machine_->SnapshotBytesMax());
  const auto snapshot_bytes = source.machine_->Snapshot(snapshot);
  ASSERT_TRUE(snapshot_bytes.has_value());
  snapshot.resize(*snapshot_bytes);

  for (std::size_t size = 0; size < snapshot.size(); ++size)
  {
    Node target{config};
    EXPECT_FALSE(target.machine_->Restore(std::span{snapshot}.first(size))) << "size " << size;
  }
  std::vector<std::byte> trailing = snapshot;
  trailing.push_back(std::byte{0});
  Node target{config};
  EXPECT_FALSE(target.machine_->Restore(trailing));

  std::vector<std::byte> small(8);
  EXPECT_FALSE(source.machine_->Snapshot(small).has_value());
}

// A batch accepts the last entry when its encoded bytes exactly fill the buffer.
TEST(OrderBatch, CapacityIncludesTheSessionAndTheActualFrame)
{
  constexpr std::size_t kEntryBytes = sizeof(SessionId) + wire::kFrameSize<wire::Cancel>;
  const wire::Cancel cancel{};
  for (const std::size_t capacity : {kEntryBytes - 1, kEntryBytes, kEntryBytes + 1})
  {
    OrderBatch batch{capacity};
    EXPECT_EQ(batch.TryAppend(0, cancel), capacity >= kEntryBytes);
    EXPECT_FALSE(batch.TryAppend(0, cancel));
    EXPECT_EQ(batch.Bytes().size(), capacity >= kEntryBytes ? kEntryBytes : 0);
  }
}

TEST(OrderBatch, RefusesAnEntryWhenFull)
{
  const wire::Cancel cancel{
      .request_number_ = wire::RequestNumber{1}, .client_order_id_ = 1, .symbol_id_ = 1};
  constexpr std::size_t kSmallBatchBytes = 256;
  OrderBatch batch{kSmallBatchBytes};
  std::uint32_t appended_count = 0;
  for (; appended_count < kSmallBatchBytes; ++appended_count)
  {
    if (!batch.TryAppend(0, cancel))
    {
      break;
    }
  }
  EXPECT_GT(appended_count, 0U);
  EXPECT_EQ(batch.EntryCount(), appended_count);
  EXPECT_LE(batch.Bytes().size(), kSmallBatchBytes);
  batch.Clear();
  EXPECT_EQ(batch.EntryCount(), 0U);
  EXPECT_TRUE(batch.Bytes().empty());
  EXPECT_TRUE(batch.TryAppend(0, cancel));
}

// The bridge tests drive mex::BasicReplicationBridge on the test thread with
// a fake client: no socket, no thread, no sleep. Each test checks what the
// cluster gets from the bridge, and AbandonedOrders().

using ClientReply = std::expected<std::vector<std::byte>, vsr::io::ClientError>;

// What the fake cluster does with one call of the bridge.
enum class Outcome
{
  Reply = 0,
  Timeout,
  Evicted,
};

// One call of the bridge to the cluster. A Resume has no body.
struct ClientCall
{
  bool resume_ = false;
  std::vector<std::byte> body_{};
};

// A stand-in for vsr::io::Client. The test decides the outcome of each call.
// Like vsr::io::Client, it permits one request in progress: after a timeout
// the next call must be Resume, and else it must be Request. A call that
// breaks this rule fails the test and still goes to the cluster, so that a
// test that drives Run() ends. The real client keeps a copy of the body and
// sends that copy again on Resume, so the cluster gets the same bytes for
// each attempt of one batch.
class FakeClient final
{
public:
  using Cluster = std::function<Outcome(const ClientCall &)>;

  explicit FakeClient(Cluster p_cluster) : cluster_{std::move(p_cluster)} {}

  [[nodiscard]] ClientReply Request(std::span<const std::byte> p_body, std::chrono::milliseconds)
  {
    if (waiting_)
    {
      ADD_FAILURE() << "Request while a request waits";
    }
    return Answer(ClientCall{.resume_ = false, .body_ = {p_body.begin(), p_body.end()}});
  }

  [[nodiscard]] ClientReply Resume(std::chrono::milliseconds)
  {
    if (!waiting_)
    {
      ADD_FAILURE() << "Resume with no request that waits";
    }
    return Answer(ClientCall{.resume_ = true});
  }

private:
  [[nodiscard]] ClientReply Answer(const ClientCall &p_call)
  {
    const Outcome outcome = cluster_(p_call);
    waiting_ = outcome == Outcome::Timeout;
    switch (outcome)
    {
    case Outcome::Reply:
      return std::vector<std::byte>(mex::kBatchReplyBytes);
    case Outcome::Timeout:
      return std::unexpected(vsr::io::ClientError::Timeout);
    case Outcome::Evicted:
      return std::unexpected(vsr::io::ClientError::Evicted);
    }
    std::unreachable();
  }

  Cluster cluster_;
  bool waiting_ = false;
};

using TestBridge = mex::BasicReplicationBridge<FakeClient>;

// The room that the bridge keeps free for one entry of a batch.
constexpr std::size_t kEntryBytesMax = sizeof(SessionId) + wire::kMaxFrameSize;
constexpr std::size_t kInboundCapacity = 1024;
constexpr std::size_t kUnusedOutboundCapacity = 2;

// The inbound queues of a gateway, one for each engine.
struct GatewayQueues
{
  explicit GatewayQueues(std::size_t p_engine_count)
  {
    for (std::size_t index = 0; index < p_engine_count; ++index)
    {
      inbound_.push_back(std::make_unique<SpscQueue<Command>>(kInboundCapacity));
      outbound_.push_back(std::make_unique<SpscQueue<Report>>(kUnusedOutboundCapacity));
      engine_queues_.push_back({*inbound_.back(), *outbound_.back()});
    }
  }

  std::vector<std::unique_ptr<SpscQueue<Command>>> inbound_{};
  std::vector<std::unique_ptr<SpscQueue<Report>>> outbound_{};
  std::vector<mex::EngineQueues> engine_queues_{};
};

// One entry of a batch, by its session and request number.
struct Entry
{
  SessionId session_ = 0;
  std::uint64_t request_ = 0;

  bool operator==(const Entry &) const = default;
};

[[nodiscard]] std::vector<Entry> DecodeBatch(std::span<const std::byte> p_body)
{
  std::vector<Entry> entries;
  std::size_t offset = 0;
  while (offset < p_body.size())
  {
    Entry entry;
    if (p_body.size() - offset < sizeof(entry.session_))
    {
      ADD_FAILURE() << "a batch ends inside a session id";
      break;
    }
    std::memcpy(&entry.session_, p_body.subspan(offset).data(), sizeof(entry.session_));
    offset += sizeof(entry.session_);
    const auto decoded = wire::DecodeClientFrame(p_body.subspan(offset));
    if (decoded.status_ != wire::DecodeStatus::Complete)
    {
      ADD_FAILURE() << "a batch has a frame that does not decode";
      break;
    }
    offset += decoded.frame_size_;
    std::visit(
        [&entry](const auto &p_message)
        {
          if constexpr (requires { p_message.request_number_; })
          {
            entry.request_ = p_message.request_number_.value_;
          }
        },
        decoded.message_);
    entries.push_back(entry);
  }
  return entries;
}

// Orders of three kinds, so that the entries of a batch have different sizes.
[[nodiscard]] mex::ClientOrder MakeOrder(std::uint64_t p_request, std::uint64_t p_kind)
{
  const wire::RequestNumber request{p_request};
  switch (p_kind % 3)
  {
  case 0:
    return wire::Cancel{.request_number_ = request, .client_order_id_ = 1, .symbol_id_ = 1};
  case 1:
    return wire::Replace{.request_number_ = request,
                         .client_order_id_ = 1,
                         .symbol_id_ = 1,
                         .new_price_ = mex::Price{100},
                         .new_total_qty_ = mex::Quantity{1}};
  default:
    return wire::NewOrder{.request_number_ = request,
                          .client_order_id_ = p_request,
                          .symbol_id_ = 1,
                          .side_ = mex::Side::Buy,
                          .type_ = wire::OrderType::Limit,
                          .price_ = mex::Price{100},
                          .qty_ = mex::Quantity{1}};
  }
}

void Push(GatewayQueues &p_queues, std::size_t p_queue, Entry p_entry)
{
  ASSERT_TRUE(p_queues.inbound_[p_queue]->TryPush(
      Command{.session_id_ = p_entry.session_, .order_ = MakeOrder(p_entry.request_, 2)}));
}

// A fake cluster that answers with scripted outcomes, then with replies, and
// keeps each call.
struct ScriptedCluster
{
  [[nodiscard]] FakeClient Client()
  {
    return FakeClient{[this](const ClientCall &p_call)
                      {
                        calls_.push_back(p_call);
                        if (outcomes_.empty())
                        {
                          return Outcome::Reply;
                        }
                        const Outcome outcome = outcomes_.front();
                        outcomes_.erase(outcomes_.begin());
                        return outcome;
                      }};
  }

  std::vector<Outcome> outcomes_{};
  std::vector<ClientCall> calls_{};
};

constexpr std::size_t kLargeBatchBytes = 4096;

// The bridge takes one order from each queue in turn, in the order of each
// queue, and sends nothing when no order waits.
TEST(ReplicationBridge, BatchesOneOrderFromEachQueueInTurn)
{
  GatewayQueues queues{2};
  ScriptedCluster cluster;
  TestBridge bridge{cluster.Client(), queues.engine_queues_, kLargeBatchBytes};
  EXPECT_FALSE(bridge.Step());
  EXPECT_TRUE(cluster.calls_.empty());

  for (const std::uint64_t request : {1U, 2U, 3U})
  {
    Push(queues, 0, {.session_ = 0, .request_ = request});
  }
  for (const std::uint64_t request : {4U, 5U})
  {
    Push(queues, 1, {.session_ = 1, .request_ = request});
  }
  EXPECT_TRUE(bridge.Step());
  ASSERT_EQ(cluster.calls_.size(), 1U);
  EXPECT_FALSE(cluster.calls_[0].resume_);
  EXPECT_EQ(DecodeBatch(cluster.calls_[0].body_),
            (std::vector<Entry>{{0, 1}, {1, 4}, {0, 2}, {1, 5}, {0, 3}}));
  EXPECT_FALSE(bridge.Step());
  EXPECT_EQ(cluster.calls_.size(), 1U);

  Push(queues, 1, {.session_ = 1, .request_ = 6});
  EXPECT_TRUE(bridge.Step());
  ASSERT_EQ(cluster.calls_.size(), 2U);
  EXPECT_EQ(DecodeBatch(cluster.calls_[1].body_), (std::vector<Entry>{{1, 6}}));
  EXPECT_EQ(bridge.AbandonedOrders(), 0U);
}

// An order that does not fit goes first into the next batch. Each order is
// in exactly one batch, also when the cluster evicts the batch before it.
TEST(ReplicationBridge, CarriesAnOrderThatDoesNotFitToTheNextBatch)
{
  for (const bool evict_first : {false, true})
  {
    SCOPED_TRACE(evict_first ? "first batch evicted" : "all batches reply");
    GatewayQueues queues{1};
    ScriptedCluster cluster;
    if (evict_first)
    {
      cluster.outcomes_ = {Outcome::Evicted};
    }
    constexpr std::size_t kBatchBytes = 2 * kEntryBytesMax;
    TestBridge bridge{cluster.Client(), queues.engine_queues_, kBatchBytes};
    constexpr std::uint64_t kOrderCount = 7;
    std::vector<Entry> pushed;
    for (std::uint64_t request = 1; request <= kOrderCount; ++request)
    {
      pushed.push_back({.session_ = 0, .request_ = request});
      Push(queues, 0, pushed.back());
    }
    for (std::uint64_t step = 0; step < kOrderCount && bridge.Step(); ++step)
    {
    }
    EXPECT_FALSE(bridge.Step());

    std::vector<Entry> sent;
    for (const ClientCall &call : cluster.calls_)
    {
      ASSERT_FALSE(call.resume_);
      EXPECT_LE(call.body_.size(), kBatchBytes);
      const std::vector<Entry> entries = DecodeBatch(call.body_);
      ASSERT_FALSE(entries.empty());
      sent.insert(sent.end(), entries.begin(), entries.end());
    }
    EXPECT_GT(cluster.calls_.size(), 1U); // an order did not fit
    EXPECT_EQ(sent, pushed);
    const std::uint64_t first_batch_size = DecodeBatch(cluster.calls_[0].body_).size();
    EXPECT_EQ(bridge.AbandonedOrders(), evict_first ? first_batch_size : 0U);
  }
}

// After a timeout the outcome of the batch is not known. The bridge waits
// for that batch, and orders that arrive meanwhile go into the next batch.
TEST(ReplicationBridge, WaitsForTheSameBatchAfterATimeout)
{
  GatewayQueues queues{1};
  ScriptedCluster cluster;
  cluster.outcomes_ = {Outcome::Timeout, Outcome::Timeout, Outcome::Reply};
  TestBridge bridge{cluster.Client(), queues.engine_queues_, kLargeBatchBytes};
  Push(queues, 0, {.session_ = 0, .request_ = 1});
  Push(queues, 0, {.session_ = 1, .request_ = 2});
  EXPECT_TRUE(bridge.Step());
  Push(queues, 0, {.session_ = 0, .request_ = 3});
  EXPECT_TRUE(bridge.Step());
  EXPECT_TRUE(bridge.Step());
  EXPECT_TRUE(bridge.Step());
  EXPECT_FALSE(bridge.Step());

  ASSERT_EQ(cluster.calls_.size(), 4U);
  EXPECT_FALSE(cluster.calls_[0].resume_);
  EXPECT_EQ(DecodeBatch(cluster.calls_[0].body_), (std::vector<Entry>{{0, 1}, {1, 2}}));
  EXPECT_TRUE(cluster.calls_[1].resume_);
  EXPECT_TRUE(cluster.calls_[2].resume_);
  EXPECT_FALSE(cluster.calls_[3].resume_);
  EXPECT_EQ(DecodeBatch(cluster.calls_[3].body_), (std::vector<Entry>{{0, 3}}));
  EXPECT_EQ(bridge.AbandonedOrders(), 0U);
}

// When the cluster evicts the session of the bridge, the bridge drops the
// batch, counts each of its orders, and does not send it again.
TEST(ReplicationBridge, CountsTheOrdersOfAnEvictedBatch)
{
  GatewayQueues queues{1};
  ScriptedCluster cluster;
  cluster.outcomes_ = {Outcome::Evicted, Outcome::Timeout, Outcome::Evicted};
  TestBridge bridge{cluster.Client(), queues.engine_queues_, kLargeBatchBytes};
  for (const std::uint64_t request : {1U, 2U, 3U})
  {
    Push(queues, 0, {.session_ = 0, .request_ = request});
  }
  EXPECT_TRUE(bridge.Step());
  EXPECT_EQ(bridge.AbandonedOrders(), 3U);

  Push(queues, 0, {.session_ = 0, .request_ = 4});
  Push(queues, 0, {.session_ = 0, .request_ = 5});
  EXPECT_TRUE(bridge.Step()); // timeout
  EXPECT_EQ(bridge.AbandonedOrders(), 3U);
  EXPECT_TRUE(bridge.Step()); // evicted while it waits
  EXPECT_EQ(bridge.AbandonedOrders(), 5U);

  Push(queues, 0, {.session_ = 0, .request_ = 6});
  EXPECT_TRUE(bridge.Step());
  EXPECT_FALSE(bridge.Step());
  ASSERT_EQ(cluster.calls_.size(), 4U);
  EXPECT_EQ(DecodeBatch(cluster.calls_[1].body_), (std::vector<Entry>{{0, 4}, {0, 5}}));
  EXPECT_TRUE(cluster.calls_[2].resume_);
  EXPECT_EQ(DecodeBatch(cluster.calls_[3].body_), (std::vector<Entry>{{0, 6}}));
  EXPECT_EQ(bridge.AbandonedOrders(), 5U);
}

// Stop() while a batch waits for its reply ends Run() after the current
// call. The bridge takes no more orders. It does not count the waiting
// batch as abandoned: only an eviction does that.
TEST(ReplicationBridge, StopsWhileARequestWaits)
{
  GatewayQueues queues{1};
  TestBridge *running = nullptr;
  std::vector<ClientCall> calls;
  FakeClient client{[&](const ClientCall &p_call)
                    {
                      calls.push_back(p_call);
                      if (calls.size() == 2)
                      {
                        Push(queues, 0, {.session_ = 0, .request_ = 3});
                      }
                      if (calls.size() == 3)
                      {
                        running->Stop();
                      }
                      return Outcome::Timeout;
                    }};
  TestBridge bridge{std::move(client), queues.engine_queues_, kLargeBatchBytes};
  running = &bridge;
  Push(queues, 0, {.session_ = 0, .request_ = 1});
  Push(queues, 0, {.session_ = 0, .request_ = 2});
  {
    // Only a safety net: a bridge that goes idle while a batch waits never
    // gets the third call, and Run() would not end.
    std::jthread watchdog{
        [&bridge](std::stop_token p_stop)
        {
          std::mutex mutex;
          std::condition_variable_any ended;
          std::unique_lock lock{mutex};
          if (!ended.wait_for(lock, p_stop, std::chrono::seconds{10}, [] { return false; }) &&
              !p_stop.stop_requested())
          {
            bridge.Stop();
          }
        }};
    bridge.Run(); // no sleep: each step makes a call
  }

  ASSERT_EQ(calls.size(), 3U);
  EXPECT_FALSE(calls[0].resume_);
  EXPECT_EQ(DecodeBatch(calls[0].body_), (std::vector<Entry>{{0, 1}, {0, 2}}));
  EXPECT_TRUE(calls[1].resume_);
  EXPECT_TRUE(calls[2].resume_);
  EXPECT_EQ(bridge.AbandonedOrders(), 0U);
  const std::optional<Command> left = queues.inbound_[0]->TryPop();
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(std::get<wire::NewOrder>(left->order_).request_number_.value_, 3U);
}

constexpr std::uint64_t kSeedCount = 1000;

void ExpectQueueOrder(const std::vector<std::vector<Entry>> &p_pushed,
                      const std::vector<Entry> &p_sent,
                      const std::vector<std::size_t> &p_queue_of_request)
{
  for (std::size_t queue = 0; queue < p_pushed.size(); ++queue)
  {
    std::vector<Entry> sent_from_queue;
    for (const Entry &entry : p_sent)
    {
      if (entry.request_ < p_queue_of_request.size() && p_queue_of_request[entry.request_] == queue)
      {
        sent_from_queue.push_back(entry);
      }
    }
    EXPECT_EQ(sent_from_queue, p_pushed[queue]) << "queue " << queue;
  }
}

// One seeded run of the bridge: random arrival of orders on random queues,
// random batch sizes, and random outcomes (reply, timeout, eviction). Each
// order must be in exactly one batch that the cluster got, in the order of
// its queue, and the orders of the evicted batches must be the abandoned
// count. The bridge cannot keep an order between the queues of two engines:
// each engine has its own client table, so the order in each queue is the
// order that counts.
void RunBridgeWithRandomFaults(std::uint64_t p_seed)
{
  constexpr std::uint64_t kStepCount = 200;
  constexpr std::uint64_t kArrivalsMax = 4;
  constexpr std::uint64_t kSessionCount = 2;
  constexpr std::uint64_t kReplyPercent = 60;
  constexpr std::uint64_t kTimeoutPercent = 25;
  std::mt19937_64 random{p_seed};
  const std::size_t queue_count = 1 + random() % 3;
  const std::size_t batch_bytes = kEntryBytesMax * (1 + random() % 4);
  GatewayQueues queues{queue_count};
  std::vector<std::vector<Entry>> pushed(queue_count);
  std::vector<std::size_t> queue_of_request{0};
  std::vector<Entry> sent;
  std::uint64_t batch_size = 0;
  std::uint64_t evicted_orders = 0;
  bool draining = false;
  FakeClient client{[&](const ClientCall &p_call)
                    {
                      if (!p_call.resume_)
                      {
                        EXPECT_LE(p_call.body_.size(), batch_bytes);
                        const std::vector<Entry> entries = DecodeBatch(p_call.body_);
                        EXPECT_FALSE(entries.empty());
                        sent.insert(sent.end(), entries.begin(), entries.end());
                        batch_size = entries.size();
                      }
                      const std::uint64_t roll = random() % 100;
                      if (draining || roll < kReplyPercent)
                      {
                        return Outcome::Reply;
                      }
                      if (roll < kReplyPercent + kTimeoutPercent)
                      {
                        return Outcome::Timeout;
                      }
                      evicted_orders += batch_size;
                      return Outcome::Evicted;
                    }};
  TestBridge bridge{std::move(client), queues.engine_queues_, batch_bytes};

  std::uint64_t pushed_count = 0;
  for (std::uint64_t step = 0; step < kStepCount; ++step)
  {
    const std::uint64_t arrivals = random() % (kArrivalsMax + 1);
    for (std::uint64_t arrival = 0; arrival < arrivals; ++arrival)
    {
      const std::size_t queue = random() % queue_count;
      const Entry entry{.session_ = static_cast<SessionId>(random() % kSessionCount),
                        .request_ = queue_of_request.size()};
      ASSERT_TRUE(queues.inbound_[queue]->TryPush(
          Command{.session_id_ = entry.session_, .order_ = MakeOrder(entry.request_, random())}));
      queue_of_request.push_back(queue);
      pushed[queue].push_back(entry);
      ++pushed_count;
    }
    static_cast<void>(bridge.Step());
  }
  draining = true;
  for (std::uint64_t step = 0; step <= pushed_count && bridge.Step(); ++step)
  {
  }
  ASSERT_FALSE(bridge.Step());

  EXPECT_EQ(sent.size(), pushed_count);
  ExpectQueueOrder(pushed, sent, queue_of_request);
  EXPECT_EQ(bridge.AbandonedOrders(), evicted_orders);
}

TEST(ReplicationBridge, SeededFaultsLoseAndRepeatNoOrder)
{
  for (std::uint64_t seed = 0; seed < kSeedCount && !HasFailure(); ++seed)
  {
    SCOPED_TRACE(testing::Message() << "seed " << seed);
    RunBridgeWithRandomFaults(seed);
  }
}

[[nodiscard]] bool IsDuplicateReject(const Report &p_report)
{
  return std::visit(
      [](const auto &p_message)
      {
        if constexpr (requires { p_message.reason_; })
        {
          return p_message.reason_ == wire::Reason::DuplicateRequest;
        }
        return false;
      },
      p_report.report_);
}

// Totals over all seeds, to show that the faults happened.
struct ClusterFaultCount
{
  std::uint64_t duplicates_ = 0;
  std::uint64_t evictions_ = 0;
  std::uint64_t restores_ = 0;
};

class FaultRun final
{
public:
  FaultRun(const Config &p_config, std::uint64_t p_seed) : random_{p_seed}, reference_{p_config}
  {
    const std::uint64_t replica_count = 2 + random_() % 2;
    for (std::uint64_t index = 0; index < replica_count; ++index)
    {
      replicas_.push_back(std::make_unique<Node>(p_config));
      replicas_.back()->machine_->EmitReportsAfter(0, 0);
    }
    reference_.machine_->EmitReportsAfter(0, 0);
    restore_at_op_ = 1 + random_() % 40;
    snapshot_.resize(replicas_.front()->machine_->SnapshotBytesMax());
  }
  void Run(const Config &p_config, ClusterFaultCount &p_faults)
  {
    GatewayQueues queues{p_config.engines_.size()};
    TestBridge bridge{
        FakeClient{[&](const ClientCall &p_call) { return OnCall(p_call, p_config, p_faults); }},
        queues.engine_queues_, kBodyBytesMax};

    // The gateway side: valid orders over all symbols, with request numbers
    // that increase for each session.
    std::array<std::uint64_t, kSessionCount> last_request{};
    std::uint64_t pushed_count = 0;
    for (std::uint64_t step = 0; step < kStepCount; ++step)
    {
      const std::uint64_t arrivals = random_() % (kArrivalsMax + 1);
      for (std::uint64_t arrival = 0; arrival < arrivals; ++arrival)
      {
        const auto session = static_cast<SessionId>(random_() % kSessionCount);
        const mex::SymbolConfig &symbol = p_config.symbols_[random_() % p_config.symbols_.size()];
        const wire::RequestNumber request{++last_request[session]};
        const std::uint64_t client_order_id = 1 + random_() % kOrderIdCount;
        const mex::Price price{static_cast<std::int64_t>(95 + random_() % 10)};
        const mex::Quantity quantity{static_cast<std::uint32_t>(1 + random_() % 20)};
        mex::ClientOrder order;
        switch (random_() % 4)
        {
        case 0:
          order = wire::Cancel{.request_number_ = request,
                               .client_order_id_ = client_order_id,
                               .symbol_id_ = symbol.symbol_id_};
          break;
        case 1:
          order = wire::Replace{.request_number_ = request,
                                .client_order_id_ = client_order_id,
                                .symbol_id_ = symbol.symbol_id_,
                                .new_price_ = price,
                                .new_total_qty_ = quantity};
          break;
        default:
          order = wire::NewOrder{.request_number_ = request,
                                 .client_order_id_ = client_order_id,
                                 .symbol_id_ = symbol.symbol_id_,
                                 .side_ = random_() % 2 == 0 ? mex::Side::Buy : mex::Side::Sell,
                                 .type_ = wire::OrderType::Limit,
                                 .price_ = price,
                                 .qty_ = quantity};
          break;
        }
        ASSERT_TRUE(queues.inbound_[symbol.engine_]->TryPush(
            Command{.session_id_ = session, .symbol_slot_ = symbol.slot_, .order_ = order}));
        ++pushed_count;
      }
      static_cast<void>(bridge.Step());
      if (testing::Test::HasFatalFailure())
      {
        return;
      }
    }
    draining_ = true;
    for (std::uint64_t step = 0; step <= pushed_count && bridge.Step(); ++step)
    {
    }
    ASSERT_FALSE(bridge.Step());
    EXPECT_EQ(bridge.AbandonedOrders(), evicted_orders_);

    CheckFinal(p_config);
  }

private:
  void CheckFinal(const Config &p_config)
  {
    // One new request on each engine sets the last applied op number of each
    // engine to the same value on all replicas and on the reference.
    OrderBatch last_batch{kBodyBytesMax};
    for (std::size_t engine = 0; engine < p_config.engines_.size(); ++engine)
    {
      const auto symbol = std::ranges::find(p_config.symbols_, engine, &mex::SymbolConfig::engine_);
      ASSERT_NE(symbol, p_config.symbols_.end());
      ASSERT_TRUE(
          last_batch.TryAppend(0, wire::Cancel{.request_number_ = wire::RequestNumber{~0ULL},
                                               .client_order_id_ = 0,
                                               .symbol_id_ = symbol->symbol_id_}));
    }
    ++op_;
    const std::uint32_t applied_count = reference_.Execute(op_, last_batch.Bytes(), op_);
    EXPECT_EQ(applied_count, p_config.engines_.size());
    EXPECT_EQ(ExecuteOnReplicas(last_batch.Bytes(), applied_count), reference_.TakeReports());
    ExpectEqualDigests();
    EXPECT_EQ(replicas_[0]->machine_->StateDigest(), reference_.machine_->StateDigest());
  }
  std::vector<Report> ExecuteOnReplicas(std::span<const std::byte> p_body,
                                        std::uint32_t p_applied_count)
  {
    std::vector<Report> first_reports;
    for (std::size_t index = 0; index < replicas_.size(); ++index)
    {
      Node &replica = *replicas_[index];
      EXPECT_EQ(replica.Execute(op_, p_body, op_), p_applied_count);
      std::vector<Report> reports = replica.TakeReports();
      if (index == 0)
      {
        first_reports = std::move(reports);
      }
      else
      {
        EXPECT_EQ(reports, first_reports) << "replica " << index << ", op " << op_;
      }
    }
    return first_reports;
  }
  void ExpectEqualDigests() const
  {
    const std::uint64_t digest = replicas_[0]->machine_->StateDigest();
    for (std::size_t index = 1; index < replicas_.size(); ++index)
    {
      EXPECT_EQ(replicas_[index]->machine_->StateDigest(), digest)
          << "replica " << index << ", op " << op_;
    }
  }
  void Commit(const Config &p_config, ClusterFaultCount &p_faults)
  {
    committed_ = true;
    ++op_;
    const std::uint32_t applied_count = reference_.Execute(op_, in_progress_, op_);
    const std::vector<Report> reports = reference_.TakeReports();
    EXPECT_TRUE(std::ranges::none_of(reports, IsDuplicateReject)) << "op " << op_;
    EXPECT_EQ(ExecuteOnReplicas(in_progress_, applied_count), reports) << "op " << op_;
    if (random_() % 100 < kDuplicatePercent)
    {
      ++p_faults.duplicates_;
      ++op_;
      const std::vector<Report> again = ExecuteOnReplicas(in_progress_, applied_count);
      EXPECT_EQ(again.size(), applied_count) << "op " << op_;
      EXPECT_TRUE(std::ranges::all_of(again, IsDuplicateReject)) << "op " << op_;
    }
    if (!restored_ && op_ >= restore_at_op_)
    {
      restored_ = true;
      ++p_faults.restores_;
      const auto snapshot_bytes = replicas_[0]->machine_->Snapshot(snapshot_);
      ASSERT_TRUE(snapshot_bytes.has_value());
      replicas_.push_back(std::make_unique<Node>(p_config));
      replicas_.back()->machine_->EmitReportsAfter(0, 0);
      ASSERT_TRUE(replicas_.back()->machine_->Restore(std::span{snapshot_}.first(*snapshot_bytes)));
      EXPECT_EQ(replicas_.back()->machine_->StateDigest(), replicas_[0]->machine_->StateDigest());
    }
  }
  Outcome OnCall(const ClientCall &p_call, const Config &p_config, ClusterFaultCount &p_faults)
  {
    if (!p_call.resume_)
    {
      in_progress_ = p_call.body_;
      committed_ = false;
    }
    const std::uint64_t roll = random_() % 100;
    Outcome outcome = Outcome::Evicted;
    if (draining_ || roll < kReplyPercent)
    {
      outcome = Outcome::Reply;
    }
    else if (roll < kReplyPercent + kTimeoutPercent)
    {
      outcome = Outcome::Timeout;
    }
    if (!committed_ && (outcome == Outcome::Reply || random_() % 2 == 0))
    {
      Commit(p_config, p_faults);
    }
    if (outcome == Outcome::Evicted)
    {
      ++p_faults.evictions_;
      evicted_orders_ += DecodeBatch(in_progress_).size();
    }
    return outcome;
  }
  static constexpr std::uint64_t kStepCount = 100;
  static constexpr std::uint64_t kArrivalsMax = 5;
  static constexpr std::uint64_t kSessionCount = 2;
  static constexpr std::uint64_t kReplyPercent = 60;
  static constexpr std::uint64_t kTimeoutPercent = 25;
  static constexpr std::uint64_t kDuplicatePercent = 20;
  static constexpr std::uint64_t kOrderIdCount = 24;
  std::mt19937_64 random_;
  std::vector<std::unique_ptr<Node>> replicas_;
  Node reference_;
  std::uint64_t restore_at_op_ = 0;
  bool restored_ = false;
  std::vector<std::byte> snapshot_;
  std::uint64_t op_ = 0;
  std::vector<std::byte> in_progress_;
  bool committed_ = false;
  std::uint64_t evicted_orders_ = 0;
  bool draining_ = false;
};

// One seeded run of gateway, bridge, and replicas on one thread. The
// gateway side queues valid orders. The bridge sends batches to a fake
// cluster with random faults. The cluster commits each batch to all
// replicas, at most once from the view of the bridge: on a reply, or at
// random on a timeout or an eviction, whose outcome the bridge does not
// know. At random the cluster commits a batch a second time, as if it
// applied a retry twice. One replica starts later from a snapshot.
//
// The replicas must have equal digests and equal reports. A reference
// replica applies each batch once only. A second application must give
// only DuplicateRequest rejects, and the replicas must give the same
// reports as the reference. A second application changes the last applied
// op number of the engines, which the digest includes. So the run ends
// with one batch that each engine applies, and then the digest of the
// reference must also be equal: the books and the client tables did not
// change.
void RunClusterWithRandomFaults(const Config &p_config, std::uint64_t p_seed,
                                ClusterFaultCount &p_faults)
{
  FaultRun run{p_config, p_seed};
  run.Run(p_config, p_faults);
}

TEST(ReplicatedExchange, SeededFaultsKeepReplicasEqual)
{
  const Config config = TestConfig();
  ClusterFaultCount faults;
  for (std::uint64_t seed = 0; seed < kSeedCount && !HasFailure(); ++seed)
  {
    SCOPED_TRACE(testing::Message() << "seed " << seed);
    RunClusterWithRandomFaults(config, seed, faults);
  }
  EXPECT_GT(faults.duplicates_, 0U);
  EXPECT_GT(faults.evictions_, 0U);
  EXPECT_GT(faults.restores_, 0U);
}

} // namespace
