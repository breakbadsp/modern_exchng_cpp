#pragma once

#include "book.hpp"
#include "protocol.hpp"
#include "spsc_queue.hpp"
#include "types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

// Engine thread around one or more Books (prep/io-design-spec.md, step 4).
// It pops commands from its inbound queue, calls Book, and pushes order reports
// to its outbound queue. All storage is allocated in the constructor.
//
// The Engine is a deterministic state machine: its state and its reports
// depend only on its configuration and on the commands it applied, in order.
// It reads no clock, random value, or address. Replication relies on this
// (prep/io-design-spec.md, "Replication preparation").
namespace mex
{

// Index of a session in the startup session list.
using SessionId = std::uint32_t;

// A taker that never rests has no pool slot, so its reports carry this id.
constexpr wire::ExchangeOrderId kNoExchangeOrderId = 0;

// generation_ starts at 1 on the first claim, so a real id is never kNoExchangeOrderId.
[[nodiscard]] constexpr wire::ExchangeOrderId ExchangeOrderIdOf(OrderId p_slot,
                                                                std::uint32_t p_generation)
{
  return (wire::ExchangeOrderId{p_generation} << 32U) | p_slot;
}

struct OrderMeta
{
  SessionId owner_{};
  wire::ClientOrderId client_order_id_ = 0;
  Quantity cum_qty_{};           // filled so far; needed for Replace total qty
  std::uint32_t generation_ = 0; // bumped each time the slot is claimed
};

using ClientOrder = std::variant<wire::NewOrder, wire::Cancel, wire::Replace>;

// Position of a command in the one total order over all engines. The first
// command is 1, so 0 means none. Distinct from wire::RequestNumber, which a
// client assigns.
struct OpNumber
{
  std::uint64_t value_ = 0;

  constexpr auto operator<=>(const OpNumber &) const = default;
};

// Gateway -> engine. symbol_slot_ selects the Book inside this engine.
struct Command
{
  OpNumber op_number_{};
  SessionId session_id_ = 0;
  std::uint32_t symbol_slot_ = 0;
  ClientOrder order_{};
};

using OrderReport =
    std::variant<wire::Ack, wire::Reject, wire::Fill, wire::CancelAck, wire::CancelReject,
                 wire::ReplaceAck, wire::ReplaceReject, wire::Done>;

// Engine -> gateway. The gateway adds the session's seq and encodes report_.
struct Report
{
  SessionId session_id_ = 0;
  OrderReport report_{};

  bool operator==(const Report &) const = default;
};

static_assert(std::is_trivially_copyable_v<Command>);
// One cache line for each inbound ring slot. It was 48 bytes before the
// request number (8 bytes in each order) and the op number (8 bytes).
static_assert(sizeof(Command) == 64);
static_assert(std::is_trivially_copyable_v<Report>);

// Live (resting) orders of one Book, keyed by (owner, client_order_id).
// Open addressing, linear probing, backward-shift delete (no tombstones).
// Capacity is a power of two, at least 2 x max_orders, so a probe always
// reaches an empty entry.
// clang-format off
class ClientOrderIndex final
{
public:
  explicit ClientOrderIndex(std::uint32_t p_max_orders)
    pre (p_max_orders > 0);

  [[nodiscard]] std::optional<OrderId> Find(SessionId p_owner, wire::ClientOrderId p_client_order_id) const;

  void Insert(const SessionId p_owner, const wire::ClientOrderId p_client_order_id, const OrderId p_order_id)
    pre (p_order_id != kInvalidOrderId)
    pre (size_ < max_size_)
    post (Find(p_owner, p_client_order_id) == p_order_id);

  // The key must be present.
  void Erase(const SessionId p_owner, const wire::ClientOrderId p_client_order_id)
    pre (size_ > 0)
    post (!Find(p_owner, p_client_order_id).has_value());

  [[nodiscard]] std::size_t Capacity() const;
  [[nodiscard]] std::uint32_t Size() const;
  void Clear();

  // The first entry a key probes. Public so tests can build probe chains that wrap.
  [[nodiscard]] std::size_t HomeSlot(SessionId p_owner, wire::ClientOrderId p_client_order_id) const;

private:
  struct Entry
  {
    wire::ClientOrderId client_order_id_ = 0;
    SessionId owner_ = 0;
    OrderId order_id_ = kInvalidOrderId; // kInvalidOrderId marks an empty entry
  };

  [[nodiscard]] std::size_t SlotOf(SessionId p_owner, wire::ClientOrderId p_client_order_id) const;

  std::size_t mask_ = 0;
  std::unique_ptr<Entry[]> entries_{};
  std::uint32_t size_ = 0;
  std::uint32_t max_size_ = 0;
};
// clang-format on

struct BookConfig
{
  wire::SymbolId symbol_id_ = 0;
  std::uint32_t max_orders_ = 0;
  std::uint32_t max_price_levels_ = 0;
};

// Each NewOrder, Cancel, and Replace gets its fills (maker report, then taker
// reports) and then exactly one final report:
//   NewOrder: Ack (rests, or fully filled with leaves 0), Reject, or Done.
//   Cancel:   CancelAck or CancelReject.
//   Replace:  ReplaceAck (leaves 0 if fully filled) or ReplaceReject.
// The commands are checked by the gateway; contracts catch a routing bug.
//
// A request executes at most once. The client table holds, for each session,
// the last request number this engine applied. A request whose number is not
// above it changes no order state and gets only the reject of its type with
// Reason::DuplicateRequest. A session can send to several engines, so the
// numbers that one engine sees can have gaps.
// clang-format off
class Engine final
{
public:
  // Book i serves symbol_slot_ i. Commands carry a session_id_ below p_session_count.
  Engine(std::span<const BookConfig> p_books, std::size_t p_session_count,
         SpscQueue<Command> &p_inbound, SpscQueue<Report> &p_outbound)
    pre (!p_books.empty())
    pre (p_session_count > 0);
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;
  Engine(Engine &&) = delete;
  Engine &operator=(Engine &&) = delete;
  ~Engine();

  // Engine thread only. Pops at most one command and applies it; returns false
  // if the inbound queue was empty.
  [[nodiscard]] bool ProcessOne();

  // Engine thread only. Applies one command to the state machine and pushes
  // its reports. Spins while the outbound queue is full. A caller that takes
  // commands from another source, such as a replicated log, calls this.
  void Apply(const Command &p_command)
    pre (p_command.op_number_ > LastAppliedOp());

  // Engine thread only.
  [[nodiscard]] OpNumber LastAppliedOp() const;

  // Engine thread only. A digest of all replicated state: the last applied op,
  // the client table, and for each book its resting orders in price-time
  // order with their metadata and client-order index entries, each slot's
  // generation, and the free slots. Slot numbers are part of the state because
  // exchange_order_id shows them. The digest does not depend on addresses or
  // on the probe layout of the index. Engines with equal configuration that
  // applied the same commands have equal digests.
  [[nodiscard]] std::uint64_t StateDigest() const;

  // Engine thread only.
  // An upper bound of the snapshot size for this configuration.
  [[nodiscard]] std::size_t SnapshotBytesMax() const;
  // Writes all replicated state (exactly the state that StateDigest() covers) into p_out.
  // Returns the size, or nothing if p_out is too small. The bytes are a function of the
  // state only: explicit little-endian fields, no padding, no addresses, no probe layout.
  [[nodiscard]] std::optional<std::size_t> Snapshot(std::span<std::byte> p_out) const;
  // Replaces all replicated state with a snapshot from an engine with equal configuration.
  // Returns false if the bytes are not a valid snapshot for this configuration; the engine
  // state is then unspecified, but no call may read or write out of bounds or break a contract.
  // Must reject trailing bytes.
  [[nodiscard]] bool Restore(std::span<const std::byte> p_snapshot);
  // When false, Apply changes state as usual but pushes no report. Default true.
  void SetReportsEnabled(bool p_enabled);

  // Engine thread only. Busy-polls the inbound queue until Stop().
  void Run();

  // Any thread.
  void Stop();

  // Any thread. Number of reports that found the outbound queue full.
  [[nodiscard]] std::uint64_t OutboundStalls() const;

private:
  struct Instrument;

  void Execute(SessionId p_session, Instrument &p_inst, const wire::NewOrder &p_order);
  void Execute(SessionId p_session, Instrument &p_inst, const wire::Cancel &p_cancel);
  void Execute(SessionId p_session, Instrument &p_inst, const wire::Replace &p_replace);

  [[nodiscard]] wire::ExchangeOrderId Settle(Instrument &p_inst, const SubmitResult &p_result,
                                             const OrderMeta &p_taker, Quantity p_leaves,
                                             wire::ExchangeOrderId p_unrested_id);

  void OnMakerFill(Instrument &p_inst, const Fill &p_fill);

  void Emit(SessionId p_session, const OrderReport &p_report);

  std::vector<std::unique_ptr<Instrument>> instruments_{};
  std::vector<wire::RequestNumber> client_table_{}; // by SessionId; 0 means none applied
  OpNumber last_applied_op_{};
  bool reports_enabled_ = true;
  SpscQueue<Command> &inbound_;
  SpscQueue<Report> &outbound_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> outbound_stalls_{0};
};
// clang-format on

} // namespace mex
