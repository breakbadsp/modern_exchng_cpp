#pragma once

#include "config.hpp"
#include "engine.hpp"
#include "gateway.hpp"
#include "spsc_queue.hpp"
#include "vsr/config.hpp"
#include "vsr/interfaces.hpp"
#include "vsr/io/client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
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
constexpr std::uint32_t kBatchEntriesMax = 0xFFFF;
constexpr std::uint32_t kBatchEntryIndexBits = 16;

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

// Bridge thread. It takes the orders that the gateway queued, packs them
// into batches, and sends each batch to the cluster as one operation. One
// batch is in progress at a time; orders that arrive meanwhile go into the
// next batch.
class ReplicationBridge final
{
public:
  // The bridge is the consumer of each inbound queue.
  ReplicationBridge(vsr::io::Client p_client, std::span<const EngineQueues> p_queues,
                    std::size_t p_batch_bytes_max);
  ReplicationBridge(const ReplicationBridge &) = delete;
  ReplicationBridge &operator=(const ReplicationBridge &) = delete;
  ReplicationBridge(ReplicationBridge &&) = delete;
  ReplicationBridge &operator=(ReplicationBridge &&) = delete;
  ~ReplicationBridge();

  // Bridge thread only. Runs until Stop().
  void Run();

  // Any thread.
  void Stop();

  // Any thread. Orders in batches whose outcome is not known, because the
  // cluster removed the session of this bridge. Their clients get no report
  // and send them again.
  [[nodiscard]] std::uint64_t AbandonedOrders() const;

private:
  // Moves queued orders into the batch. Returns false if the batch is empty.
  [[nodiscard]] bool Fill();
  void Submit();

  vsr::io::Client client_;
  std::vector<SpscQueue<Command> *> inbound_{};
  OrderBatch batch_;
  // An order that did not fit in the batch in progress.
  std::optional<Command> carried_{};
  std::size_t next_queue_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> abandoned_orders_{0};
};

} // namespace mex
