#include "config.hpp"
#include "engine.hpp"
#include "protocol.hpp"
#include "replication.hpp"
#include "spsc_queue.hpp"
#include "vsr/interfaces.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace
{

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

TEST(OrderBatch, RefusesAnEntryWhenFull)
{
  const wire::Cancel cancel{
      .request_number_ = wire::RequestNumber{1}, .client_order_id_ = 1, .symbol_id_ = 1};
  constexpr std::size_t kSmallBatchBytes = 256;
  OrderBatch batch{kSmallBatchBytes};
  std::uint32_t appended_count = 0;
  while (batch.TryAppend(0, cancel))
  {
    ++appended_count;
  }
  EXPECT_GT(appended_count, 0U);
  EXPECT_EQ(batch.EntryCount(), appended_count);
  EXPECT_LE(batch.Bytes().size(), kSmallBatchBytes);
  batch.Clear();
  EXPECT_EQ(batch.EntryCount(), 0U);
  EXPECT_TRUE(batch.Bytes().empty());
  EXPECT_TRUE(batch.TryAppend(0, cancel));
}

} // namespace
