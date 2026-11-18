#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
#include "spsc_queue.hpp"
#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/io/client.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// Replication with libvsr (docs/replication.md). Several copies of the
// exchange agree on one ordered sequence of batches of orders. Each copy
// applies every batch to its engines, so each copy has the same books.
//
//   client -> Gateway -> inbound queues -> ReplicationBridge -> libvsr cluster
//   libvsr commit -> ExchangeStateMachine -> engines -> outbound queues -> Gateway -> client
namespace mex
{

// One operation of the replicated log: a sequence of entries. One entry is a
// session id (u32) and one client frame (NewOrder, Cancel, or Replace) in the
// wire format of protocol.hpp.
class OrderBatch final
{
public:
  // p_bytes_max is the largest operation body of the cluster.
  explicit OrderBatch(std::size_t p_bytes_max);

  // Returns false, and changes nothing, if the batch has no room.
  [[nodiscard]] bool TryAppend(SessionId p_session, const ClientOrder &p_order);

  [[nodiscard]] std::span<const std::byte> Bytes() const;
  [[nodiscard]] std::uint32_t EntryCount() const;
  void Clear();

private:
  std::vector<std::byte> bytes_{};
  std::size_t used_bytes_ = 0;
  std::uint32_t entry_count_ = 0;
};

// The largest number of entries that one operation applies. The engine op
// number of an entry is (operation number, entry index), packed in 64 bits.
constexpr std::uint32_t kBatchEntryIndexBits = 16;
constexpr std::uint32_t kBatchEntriesMax = (std::uint32_t{1} << kBatchEntryIndexBits) - 1;
constexpr std::size_t kBatchEntryBytesMax =
    sizeof(SessionId) + wire::MaxFrameSize<ClientOrder>::kValue;

// The reply of one operation: the number of entries that it applied (u32).
constexpr std::size_t kBatchReplyBytes = sizeof(std::uint32_t);

// All engines of the exchange as one libvsr state machine. libvsr calls it
// from the replica thread only. It applies the entries of a batch in order,
// each to the engine of its symbol, so all engines share one thread here.
//
// Reports are an output, not replicated state: each replica pushes the
// reports of each operation to its own outbound queues, and its gateway sends
// them to the sessions that it holds.
// clang-format off
class ExchangeStateMachine final : public vsr::StateMachine
{
public:
  // p_outbound[i] takes the reports of p_config.engines_[i]. All storage is
  // allocated here. Reports are discarded until EmitReportsAfter().
  ExchangeStateMachine(const Config &p_config, std::span<SpscQueue<Report> *const> p_outbound)
    pre (p_outbound.size() == p_config.engines_.size())
    pre (!p_config.engines_.empty());
  ExchangeStateMachine(const ExchangeStateMachine &) = delete;
  ExchangeStateMachine &operator=(const ExchangeStateMachine &) = delete;
  ExchangeStateMachine(ExchangeStateMachine &&) = delete;
  ExchangeStateMachine &operator=(ExchangeStateMachine &&) = delete;
  ~ExchangeStateMachine();

  // Starts the reports. An operation pushes its reports only if its number is
  // above p_op and the primary made it at or after p_realtime_ns. All other
  // operations change state and push no report: a replica that starts
  // applies again the operations of its log and the operations that it
  // missed, and the sessions of those ended with the earlier process.
  void EmitReportsAfter(std::uint64_t p_op, std::uint64_t p_realtime_ns);

  // An upper bound of the snapshot size, for vsr::Config::snapshot_bytes_max_.
  [[nodiscard]] std::size_t SnapshotBytesMax() const;

  // A digest of the state of all engines. Equal on replicas that applied the
  // same operations.
  [[nodiscard]] std::uint64_t StateDigest() const;

  [[nodiscard]] std::size_t Execute(const vsr::Operation &p_operation,
                                    std::span<const std::byte> p_body,
                                    std::span<std::byte> p_reply) override;
  [[nodiscard]] std::expected<std::size_t, std::monostate>
  Snapshot(std::span<std::byte> p_out) override;
  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot) override;

private:
  struct EngineSlot;

  std::vector<SymbolConfig> routes_{}; // sorted by symbol_id_
  std::size_t session_count_ = 0;
  std::vector<std::unique_ptr<EngineSlot>> engines_{};
  std::uint64_t emit_after_op_ = ~std::uint64_t{0};
  std::uint64_t emit_from_ns_ = 0;
};
// clang-format on

// The limits of the libvsr cluster for this exchange configuration.
// p_snapshot_bytes_max comes from ExchangeStateMachine::SnapshotBytesMax().
[[nodiscard]] vsr::Config MakeVsrConfig(const Config &p_config, std::uint8_t p_replica,
                                        std::size_t p_snapshot_bytes_max)
    pre(p_replica < p_config.replicas_.size());

// The calls of vsr::io::Client that the bridge uses. A test gives a scripted
// client, to drive the bridge on one thread with no socket and no sleep.
template <typename TClient>
concept ReplicationClient = requires(TClient &p_client, std::span<const std::byte> p_body,
                                     std::chrono::milliseconds p_timeout) {
  {
    p_client.Request(p_body, p_timeout)
  } -> std::same_as<std::expected<std::vector<std::byte>, vsr::io::ClientError>>;
  {
    p_client.Resume(p_timeout)
  } -> std::same_as<std::expected<std::vector<std::byte>, vsr::io::ClientError>>;
};

// Bridge thread. It takes the orders that the gateway queued, packs them
// into batches, and sends each batch to the cluster as one operation. One
// batch is in progress at a time; orders that arrive meanwhile go into the
// next batch.
template <ReplicationClient TClient> class BasicReplicationBridge final
{
public:
  // The bridge is the consumer of each inbound queue.
  BasicReplicationBridge(TClient p_client, std::span<const EngineQueues> p_queues,
                         std::size_t p_batch_bytes_max)
      pre(p_batch_bytes_max >= kBatchEntryBytesMax);
  BasicReplicationBridge(const BasicReplicationBridge &) = delete;
  BasicReplicationBridge &operator=(const BasicReplicationBridge &) = delete;
  BasicReplicationBridge(BasicReplicationBridge &&) = delete;
  BasicReplicationBridge &operator=(BasicReplicationBridge &&) = delete;
  ~BasicReplicationBridge() = default;

  // Bridge thread only. Runs until Stop().
  void Run();

  // Bridge thread only. Makes one call to the cluster: it sends a new batch,
  // or waits again for the batch in progress. Returns false, and makes no
  // call, if no batch is in progress and no order waits. Run() calls it in a
  // loop. A test calls it to drive the bridge one call at a time.
  [[nodiscard]] bool Step();

  // Any thread.
  void Stop();

  // Any thread. Orders in batches whose outcome is not known, because the
  // cluster removed the session of this bridge. Their clients get no report
  // and send them again.
  [[nodiscard]] std::uint64_t AbandonedOrders() const;

private:
  // The bridge checks Stop() at least this often while the cluster is silent.
  static constexpr auto kRequestWait = std::chrono::milliseconds{200};
  // Sleep of the bridge when no order waits. It bounds the added latency.
  static constexpr auto kIdleSleep = std::chrono::microseconds{50};

  // Moves queued orders into the batch. Returns false if the batch is empty.
  [[nodiscard]] bool Fill();

  TClient client_;
  std::vector<SpscQueue<Command> *> inbound_{};
  OrderBatch batch_;
  // An order that did not fit in the batch in progress.
  std::optional<Command> carried_{};
  std::size_t next_queue_ = 0;
  // The batch in progress got no reply yet. Its outcome is not known.
  bool waiting_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> abandoned_orders_{0};
};

using ReplicationBridge = BasicReplicationBridge<vsr::io::Client>;

template <ReplicationClient TClient>
BasicReplicationBridge<TClient>::BasicReplicationBridge(TClient p_client,
                                                        std::span<const EngineQueues> p_queues,
                                                        std::size_t p_batch_bytes_max)
    : client_(std::move(p_client)), batch_(p_batch_bytes_max)
{
  for (const EngineQueues &queues : p_queues)
  {
    inbound_.push_back(&queues.inbound_);
  }
}

template <ReplicationClient TClient> void BasicReplicationBridge<TClient>::Run()
{
  while (!stop_.load(std::memory_order_relaxed))
  {
    if (!Step())
    {
      std::this_thread::sleep_for(kIdleSleep);
    }
  }
}

template <ReplicationClient TClient> bool BasicReplicationBridge<TClient>::Step()
{
  if (!waiting_ && !Fill())
  {
    return false;
  }
  auto reply =
      waiting_ ? client_.Resume(kRequestWait) : client_.Request(batch_.Bytes(), kRequestWait);
  // No reply yet: a view change or a slow disk. The outcome is not known,
  // so wait for this batch; a new batch could apply orders out of order.
  waiting_ = !reply.has_value() && reply.error() == vsr::io::ClientError::Timeout;
  if (waiting_)
  {
    return true;
  }
  if (!reply.has_value() && reply.error() == vsr::io::ClientError::Evicted)
  {
    // The batch could be applied or not. To send it again could give the
    // clients a DuplicateRequest reject for an order that executed.
    abandoned_orders_.store(abandoned_orders_.load(std::memory_order_relaxed) + batch_.EntryCount(),
                            std::memory_order_relaxed);
  }
  batch_.Clear();
  return true;
}

template <ReplicationClient TClient> void BasicReplicationBridge<TClient>::Stop()
{
  stop_.store(true, std::memory_order_relaxed);
}

template <ReplicationClient TClient>
std::uint64_t BasicReplicationBridge<TClient>::AbandonedOrders() const
{
  return abandoned_orders_.load(std::memory_order_relaxed);
}

template <ReplicationClient TClient> bool BasicReplicationBridge<TClient>::Fill()
{
  if (carried_.has_value())
  {
    const bool appended = batch_.TryAppend(carried_->session_id_, carried_->order_);
    contract_assert(appended); // the batch is empty
    carried_.reset();
  }
  // One order from each queue in turn, so a busy engine queue cannot keep
  // the orders of another out of the batch.
  std::size_t empty_queues = 0;
  while (empty_queues < inbound_.size())
  {
    const std::optional<Command> command = inbound_[next_queue_]->TryPop();
    next_queue_ = (next_queue_ + 1) % inbound_.size();
    if (!command.has_value())
    {
      ++empty_queues;
      continue;
    }
    empty_queues = 0;
    if (!batch_.TryAppend(command->session_id_, command->order_))
    {
      carried_ = command;
      break;
    }
  }
  return batch_.EntryCount() > 0;
}

// The production bridge compiles once, in replication.cpp.
extern template class BasicReplicationBridge<vsr::io::Client>;

} // namespace mex
