#include "engine.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace mex
{
namespace
{

constexpr std::uint64_t kGoldenRatio = 0x9E3779B97F4A7C15ULL;

// splitmix64 finalizer.
[[nodiscard]] constexpr std::uint64_t Finalize(std::uint64_t p_value)
{
  std::uint64_t hash = p_value;
  hash = (hash ^ (hash >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  hash = (hash ^ (hash >> 27U)) * 0x94D049BB133111EBULL;
  return hash ^ (hash >> 31U);
}

// Adds one value to a state digest. The result depends on the order of the
// values. The constant keeps a run of zero values from leaving the digest at 0.
constexpr void Mix(std::uint64_t &p_digest, std::uint64_t p_value)
{
  p_digest = Finalize(p_digest + kGoldenRatio + p_value);
}

// Version 1 uses unsigned 64-bit little-endian fields throughout. Prices use
// their two's-complement bits. No native object representation is copied.
constexpr std::uint64_t kSnapshotVersion = 1;
constexpr std::size_t kSnapshotFieldBytes = 8;

class SnapshotWriter final
{
public:
  explicit SnapshotWriter(std::span<std::byte> p_out) : out_(p_out) {}

  void Write(std::uint64_t p_value)
  {
    if (position_ <= out_.size() && out_.size() - position_ >= kSnapshotFieldBytes)
    {
      for (std::size_t i = 0; i < kSnapshotFieldBytes; ++i)
      {
        out_[position_ + i] = static_cast<std::byte>((p_value >> (i * 8U)) & 0xffU);
      }
    }
    position_ += kSnapshotFieldBytes;
  }

  [[nodiscard]] std::optional<std::size_t> Result() const
  {
    return position_ <= out_.size() ? std::optional{position_} : std::nullopt;
  }

private:
  std::span<std::byte> out_;
  std::size_t position_ = 0;
};

class SnapshotReader final
{
public:
  explicit SnapshotReader(std::span<const std::byte> p_bytes) : bytes_(p_bytes) {}

  [[nodiscard]] std::uint64_t Read()
  {
    if (bytes_.size() - position_ < kSnapshotFieldBytes)
    {
      valid_ = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < kSnapshotFieldBytes; ++i)
    {
      value |= std::uint64_t{std::to_integer<unsigned char>(bytes_[position_ + i])} << (i * 8U);
    }
    position_ += kSnapshotFieldBytes;
    return value;
  }

  [[nodiscard]] bool Complete() const { return valid_ && position_ == bytes_.size(); }
  [[nodiscard]] bool Valid() const { return valid_; }

private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  bool valid_ = true;
};

} // namespace

ClientOrderIndex::ClientOrderIndex(std::uint32_t p_max_orders)
    : mask_(std::bit_ceil(std::size_t{2} * p_max_orders) - 1),
      entries_(std::make_unique<Entry[]>(mask_ + 1)), size_(0), max_size_(p_max_orders)
{
}

std::size_t ClientOrderIndex::HomeSlot(SessionId p_owner,
                                       wire::ClientOrderId p_client_order_id) const
{
  const std::uint64_t hash = Finalize(p_client_order_id ^ (std::uint64_t{p_owner} * kGoldenRatio));
  return static_cast<std::size_t>(hash) & mask_;
}

// The entry that holds the key, or the empty entry where it would go.
std::size_t ClientOrderIndex::SlotOf(SessionId p_owner, wire::ClientOrderId p_client_order_id) const
{
  std::size_t slot = HomeSlot(p_owner, p_client_order_id);
  // At most half the entries are used, so the probe meets an empty entry.
  for (std::size_t probes = 0; probes <= mask_; ++probes)
  {
    const Entry &entry = entries_[slot];
    if (entry.order_id_ == kInvalidOrderId ||
        (entry.owner_ == p_owner && entry.client_order_id_ == p_client_order_id))
    {
      return slot;
    }
    slot = (slot + 1) & mask_;
  }
  contract_assert(false);
  return slot;
}

std::optional<OrderId> ClientOrderIndex::Find(SessionId p_owner,
                                              wire::ClientOrderId p_client_order_id) const
{
  const Entry &entry = entries_[SlotOf(p_owner, p_client_order_id)];
  if (entry.order_id_ == kInvalidOrderId)
  {
    return std::nullopt;
  }
  return entry.order_id_;
}

void ClientOrderIndex::Insert(const SessionId p_owner, const wire::ClientOrderId p_client_order_id,
                              const OrderId p_order_id)
{
  Entry &entry = entries_[SlotOf(p_owner, p_client_order_id)];
  contract_assert(entry.order_id_ == kInvalidOrderId);
  entry = Entry{.client_order_id_ = p_client_order_id, .owner_ = p_owner, .order_id_ = p_order_id};
  ++size_;
}

void ClientOrderIndex::Erase(const SessionId p_owner, const wire::ClientOrderId p_client_order_id)
{
  std::size_t hole = SlotOf(p_owner, p_client_order_id);
  contract_assert(entries_[hole].order_id_ != kInvalidOrderId);
  // Backward shift: move each later entry of the run into the hole if the hole
  // lies between its home slot and where it is now, so every probe still finds it.
  std::size_t next = hole;
  for (std::size_t probes = 0; probes < mask_; ++probes)
  {
    next = (next + 1) & mask_;
    const Entry &entry = entries_[next];
    if (entry.order_id_ == kInvalidOrderId)
    {
      break;
    }
    const std::size_t home = HomeSlot(entry.owner_, entry.client_order_id_);
    if (((next - home) & mask_) >= ((next - hole) & mask_))
    {
      entries_[hole] = entry;
      hole = next;
    }
  }
  entries_[hole] = Entry{};
  --size_;
}

std::size_t ClientOrderIndex::Capacity() const { return mask_ + 1; }

std::uint32_t ClientOrderIndex::Size() const { return size_; }

void ClientOrderIndex::Clear()
{
  for (std::size_t i = 0; i <= mask_; ++i)
  {
    entries_[i] = Entry{};
  }
  size_ = 0;
}

struct Engine::Instrument
{
  explicit Instrument(const BookConfig &p_config)
      : book_(p_config.max_orders_, p_config.max_price_levels_),
        meta_(std::make_unique<OrderMeta[]>(p_config.max_orders_)), index_(p_config.max_orders_),
        symbol_id_(p_config.symbol_id_),
        restore_orders_(std::make_unique<Book::RestingOrder[]>(p_config.max_orders_)),
        restore_free_(std::make_unique<OrderId[]>(p_config.max_orders_))
  {
  }

  // Adds the resting orders of one side: its levels in ladder order, and the
  // orders of each level in time priority.
  void MixSide(std::uint64_t &p_digest, const PriceLadder &p_levels) const
  {
    Mix(p_digest, p_levels.size());
    for (const PriceLevel &level : p_levels)
    {
      // Two's complement, so each price has one value.
      Mix(p_digest, static_cast<std::uint64_t>(level.price_.ticks_));
      Mix(p_digest, level.order_count_);
      OrderId order_id = level.head_;
      for (std::uint32_t count = 0; count < level.order_count_; ++count)
      {
        const OrderMeta &meta = meta_[order_id];
        Mix(p_digest, order_id);
        Mix(p_digest, book_.Order(order_id).quantity_.units_);
        Mix(p_digest, meta.owner_);
        Mix(p_digest, meta.client_order_id_);
        Mix(p_digest, meta.cum_qty_.units_);
        // The index entry of this order, read by key and not by position.
        Mix(p_digest, index_.Find(meta.owner_, meta.client_order_id_).value_or(kInvalidOrderId));
        order_id = book_.Order(order_id).next_;
      }
    }
  }

  void MixState(std::uint64_t &p_digest) const
  {
    Mix(p_digest, symbol_id_);
    MixSide(p_digest, book_.Bids());
    MixSide(p_digest, book_.Asks());
    // With the lookups above, the size shows that the index has no other entry.
    Mix(p_digest, index_.Size());
    // The next exchange_order_id of each slot, and the slot that each later
    // order takes, also depend on the free slots.
    for (OrderId slot = 0; slot < book_.MaxOrders(); ++slot)
    {
      Mix(p_digest, meta_[slot].generation_);
    }
    for (const OrderId slot : book_.FreeSlots())
    {
      Mix(p_digest, slot);
    }
  }

  void WriteSide(SnapshotWriter &p_writer, const PriceLadder &p_levels) const
  {
    p_writer.Write(p_levels.size());
    for (const PriceLevel &level : p_levels)
    {
      p_writer.Write(static_cast<std::uint64_t>(level.price_.ticks_));
      p_writer.Write(level.order_count_);
      OrderId slot = level.head_;
      for (std::uint32_t i = 0; i < level.order_count_; ++i)
      {
        const OrderNode &order = book_.Order(slot);
        const OrderMeta &meta = meta_[slot];
        p_writer.Write(slot);
        p_writer.Write(order.quantity_.units_);
        p_writer.Write(meta.owner_);
        p_writer.Write(meta.client_order_id_);
        p_writer.Write(meta.cum_qty_.units_);
        slot = order.next_;
      }
    }
  }

  [[nodiscard]] bool ReadSide(SnapshotReader &p_reader, Side p_side, std::uint32_t &p_order_count,
                              std::size_t p_sessions)
  {
    const std::uint64_t levels = p_reader.Read();
    if (levels > book_.Bids().Capacity())
    {
      return false;
    }
    Price previous{};
    for (std::uint64_t i = 0; i < levels; ++i)
    {
      const Price price{std::bit_cast<std::int64_t>(p_reader.Read())};
      const std::uint64_t count = p_reader.Read();
      if (count == 0 || count > book_.MaxOrders() - p_order_count ||
          (i > 0 && (p_side == Side::Buy ? price <= previous : price >= previous)))
      {
        return false;
      }
      previous = price;
      for (std::uint64_t j = 0; j < count; ++j)
      {
        const std::uint64_t slot = p_reader.Read();
        const std::uint64_t quantity = p_reader.Read();
        const std::uint64_t owner = p_reader.Read();
        const std::uint64_t client_id = p_reader.Read();
        const std::uint64_t cumulative = p_reader.Read();
        if (!p_reader.Valid() || slot >= book_.MaxOrders() || quantity == 0 ||
            quantity > std::numeric_limits<std::uint32_t>::max() || owner >= p_sessions ||
            owner > std::numeric_limits<SessionId>::max() ||
            cumulative > std::numeric_limits<std::uint32_t>::max() - quantity ||
            index_.Find(static_cast<SessionId>(owner), client_id).has_value())
        {
          return false;
        }
        const auto order_slot = static_cast<OrderId>(slot);
        restore_orders_[p_order_count++] = Book::RestingOrder{
            order_slot, p_side, price, Quantity{static_cast<std::uint32_t>(quantity)}};
        OrderMeta &meta = meta_[order_slot];
        meta.owner_ = static_cast<SessionId>(owner);
        meta.client_order_id_ = client_id;
        meta.cum_qty_ = Quantity{static_cast<std::uint32_t>(cumulative)};
        index_.Insert(meta.owner_, client_id, order_slot);
      }
    }
    return true;
  }

  [[nodiscard]] bool ReadState(SnapshotReader &p_reader, std::size_t p_sessions)
  {
    index_.Clear();
    std::uint32_t count = 0;
    if (!ReadSide(p_reader, Side::Buy, count, p_sessions) ||
        !ReadSide(p_reader, Side::Sell, count, p_sessions))
    {
      return false;
    }
    for (OrderId slot = 0; slot < book_.MaxOrders(); ++slot)
    {
      const std::uint64_t generation = p_reader.Read();
      if (generation > std::numeric_limits<std::uint32_t>::max())
      {
        return false;
      }
      meta_[slot].generation_ = static_cast<std::uint32_t>(generation);
    }
    const std::uint64_t free_count = p_reader.Read();
    if (free_count != book_.MaxOrders() - count)
    {
      return false;
    }
    for (std::uint64_t i = 0; i < free_count; ++i)
    {
      const std::uint64_t slot = p_reader.Read();
      if (slot >= book_.MaxOrders())
      {
        return false;
      }
      restore_free_[i] = static_cast<OrderId>(slot);
    }
    return p_reader.Valid() &&
           book_.Restore(std::span{restore_orders_.get(), count},
                         std::span{restore_free_.get(), static_cast<std::size_t>(free_count)});
  }

  Book book_;
  std::unique_ptr<OrderMeta[]> meta_;
  ClientOrderIndex index_;
  wire::SymbolId symbol_id_;
  std::unique_ptr<Book::RestingOrder[]> restore_orders_;
  std::unique_ptr<OrderId[]> restore_free_;
};

Engine::Engine(std::span<const BookConfig> p_books, std::size_t p_session_count,
               SpscQueue<Command> &p_inbound, SpscQueue<Report> &p_outbound)
    : client_table_(p_session_count), inbound_(p_inbound), outbound_(p_outbound)
{
  instruments_.reserve(p_books.size());
  for (const BookConfig &config : p_books)
  {
    instruments_.push_back(std::make_unique<Instrument>(config));
  }
}

Engine::~Engine() = default;

bool Engine::ProcessOne()
{
  const std::optional<Command> command = inbound_.TryPop();
  if (!command.has_value())
  {
    return false;
  }
  Apply(*command);
  return true;
}

void Engine::Apply(const Command &p_command)
{
  contract_assert(p_command.symbol_slot_ < instruments_.size());
  contract_assert(p_command.session_id_ < client_table_.size());
  last_applied_op_ = p_command.op_number_;
  Instrument &inst = *instruments_[p_command.symbol_slot_];
  wire::RequestNumber &last_request = client_table_[p_command.session_id_];
  std::visit(
      [&](const auto &p_order)
      {
        contract_assert(p_order.symbol_id_ == inst.symbol_id_);
        if (p_order.request_number_ <= last_request)
        {
          Emit(p_command.session_id_, wire::RejectOf(p_order, wire::Reason::DuplicateRequest));
          return;
        }
        last_request = p_order.request_number_;
        Execute(p_command.session_id_, inst, p_order);
      },
      p_command.order_);
}

OpNumber Engine::LastAppliedOp() const { return last_applied_op_; }

std::uint64_t Engine::StateDigest() const
{
  std::uint64_t digest = 0;
  Mix(digest, last_applied_op_.value_);
  for (const wire::RequestNumber last_request : client_table_)
  {
    Mix(digest, last_request.value_);
  }
  for (const std::unique_ptr<Instrument> &inst : instruments_)
  {
    inst->MixState(digest);
  }
  return digest;
}

std::size_t Engine::SnapshotBytesMax() const
{
  // Header: version, book count, session count, configurations, last op, client table.
  std::size_t fields = 4 + instruments_.size() * 3 + client_table_.size();
  for (const auto &inst : instruments_)
  {
    // Two side counts, free count, up to two fields per live level, five per
    // live order, one generation per slot, and one free entry per slot.
    fields += 3 + std::size_t{inst->book_.MaxOrders()} * 9;
  }
  return fields * kSnapshotFieldBytes;
}

std::optional<std::size_t> Engine::Snapshot(std::span<std::byte> p_out) const
{
  SnapshotWriter writer{p_out};
  writer.Write(kSnapshotVersion);
  writer.Write(instruments_.size());
  writer.Write(client_table_.size());
  for (const auto &inst : instruments_)
  {
    writer.Write(inst->symbol_id_);
    writer.Write(inst->book_.MaxOrders());
    writer.Write(inst->book_.Bids().Capacity());
  }
  writer.Write(last_applied_op_.value_);
  for (const wire::RequestNumber request : client_table_)
  {
    writer.Write(request.value_);
  }
  for (const auto &inst : instruments_)
  {
    inst->WriteSide(writer, inst->book_.Bids());
    inst->WriteSide(writer, inst->book_.Asks());
    for (OrderId slot = 0; slot < inst->book_.MaxOrders(); ++slot)
    {
      writer.Write(inst->meta_[slot].generation_);
    }
    writer.Write(inst->book_.FreeSlotCount());
    for (const OrderId slot : inst->book_.FreeSlots())
    {
      writer.Write(slot);
    }
  }
  return writer.Result();
}

bool Engine::Restore(std::span<const std::byte> p_snapshot)
{
  SnapshotReader reader{p_snapshot};
  if (reader.Read() != kSnapshotVersion || reader.Read() != instruments_.size() ||
      reader.Read() != client_table_.size())
  {
    return false;
  }
  for (const auto &inst : instruments_)
  {
    if (reader.Read() != inst->symbol_id_ || reader.Read() != inst->book_.MaxOrders() ||
        reader.Read() != inst->book_.Bids().Capacity())
    {
      return false;
    }
  }
  last_applied_op_.value_ = reader.Read();
  for (wire::RequestNumber &request : client_table_)
  {
    request.value_ = reader.Read();
  }
  for (const auto &inst : instruments_)
  {
    if (!inst->ReadState(reader, client_table_.size()))
    {
      return false;
    }
  }
  return reader.Complete();
}

void Engine::SetReportsEnabled(bool p_enabled) { reports_enabled_ = p_enabled; }

void Engine::Run()
{
  while (!stop_.load(std::memory_order_relaxed))
  {
    static_cast<void>(ProcessOne());
  }
}

void Engine::Stop() { stop_.store(true, std::memory_order_relaxed); }

std::uint64_t Engine::OutboundStalls() const
{
  return outbound_stalls_.load(std::memory_order_relaxed);
}

void Engine::Execute(SessionId p_session, Instrument &p_inst, const wire::NewOrder &p_order)
{
  const auto reject = [&](wire::Reason p_reason)
  {
    Emit(p_session, wire::Reject{.client_order_id_ = p_order.client_order_id_,
                                 .symbol_id_ = p_inst.symbol_id_,
                                 .reason_ = p_reason});
  };
  const auto done = [&](wire::Reason p_reason)
  {
    Emit(p_session, wire::Done{.client_order_id_ = p_order.client_order_id_,
                               .symbol_id_ = p_inst.symbol_id_,
                               .reason_ = p_reason});
  };

  if (p_order.qty_ == Quantity{})
  {
    reject(wire::Reason::InvalidQuantity);
    return;
  }
  if (p_inst.index_.Find(p_session, p_order.client_order_id_).has_value())
  {
    reject(wire::Reason::DuplicateClientOrderId);
    return;
  }

  const bool is_market = p_order.type_ == wire::OrderType::Market;
  const SubmitResult result =
      is_market ? p_inst.book_.SubmitMarketOrder(p_order.side_, p_order.qty_)
                : p_inst.book_.SubmitLimitOrder(p_order.side_, p_order.price_, p_order.qty_);
  if (result.status_ == SubmitStatus::Rejected)
  {
    // Nothing filled. A market order found no liquidity; a limit order cannot rest.
    if (is_market)
    {
      done(wire::Reason::MarketRemainder);
    }
    else
    {
      reject(wire::Reason::BookFull);
    }
    return;
  }

  const OrderMeta taker{.owner_ = p_session, .client_order_id_ = p_order.client_order_id_};
  const wire::ExchangeOrderId exchange_order_id =
      Settle(p_inst, result, taker, p_order.qty_, kNoExchangeOrderId);

  if (result.filled_qty_ < p_order.qty_ && result.order_id_ == kInvalidOrderId)
  {
    done(is_market ? wire::Reason::MarketRemainder : wire::Reason::CannotRest);
    return;
  }
  const Quantity leaves = result.order_id_ == kInvalidOrderId ? Quantity{} : result.remaining_;
  Emit(p_session, wire::Ack{.client_order_id_ = p_order.client_order_id_,
                            .symbol_id_ = p_inst.symbol_id_,
                            .exchange_order_id_ = exchange_order_id,
                            .leaves_ = leaves});
}

void Engine::Execute(SessionId p_session, Instrument &p_inst, const wire::Cancel &p_cancel)
{
  const std::optional<OrderId> order_id = p_inst.index_.Find(p_session, p_cancel.client_order_id_);
  if (!order_id.has_value())
  {
    Emit(p_session, wire::CancelReject{.client_order_id_ = p_cancel.client_order_id_,
                                       .symbol_id_ = p_inst.symbol_id_,
                                       .reason_ = wire::Reason::UnknownOrder});
    return;
  }
  [[maybe_unused]] const bool cancelled = p_inst.book_.CancelOrder(*order_id);
  contract_assert(cancelled);
  p_inst.index_.Erase(p_session, p_cancel.client_order_id_);
  Emit(p_session, wire::CancelAck{.client_order_id_ = p_cancel.client_order_id_,
                                  .symbol_id_ = p_inst.symbol_id_});
}

void Engine::Execute(SessionId p_session, Instrument &p_inst, const wire::Replace &p_replace)
{
  const auto reject = [&](wire::Reason p_reason)
  {
    Emit(p_session, wire::ReplaceReject{.client_order_id_ = p_replace.client_order_id_,
                                        .symbol_id_ = p_inst.symbol_id_,
                                        .reason_ = p_reason});
  };
  const auto ack = [&](Quantity p_leaves)
  {
    Emit(p_session, wire::ReplaceAck{.client_order_id_ = p_replace.client_order_id_,
                                     .symbol_id_ = p_inst.symbol_id_,
                                     .price_ = p_replace.new_price_,
                                     .leaves_ = p_leaves});
  };

  const std::optional<OrderId> order_id = p_inst.index_.Find(p_session, p_replace.client_order_id_);
  if (!order_id.has_value())
  {
    reject(wire::Reason::UnknownOrder);
    return;
  }
  const OrderMeta original = p_inst.meta_[*order_id];
  if (p_replace.new_total_qty_ <= original.cum_qty_)
  {
    reject(wire::Reason::TooLate);
    return;
  }
  const Quantity new_leaves = p_replace.new_total_qty_ - original.cum_qty_;
  const OrderNode &node = p_inst.book_.Order(*order_id);

  if (node.price_ == p_replace.new_price_ && new_leaves < node.quantity_)
  {
    [[maybe_unused]] const bool reduced = p_inst.book_.ReduceOrder(*order_id, new_leaves);
    contract_assert(reduced);
    ack(new_leaves);
    return;
  }

  const SubmitResult result =
      p_inst.book_.ReplaceOrder(*order_id, p_replace.new_price_, new_leaves);
  if (result.status_ == SubmitStatus::Rejected)
  {
    reject(wire::Reason::BookFull);
    return;
  }
  // The old slot is gone. Settle inserts the key again if the order rests.
  p_inst.index_.Erase(p_session, p_replace.client_order_id_);
  static_cast<void>(Settle(p_inst, result, original, new_leaves,
                           ExchangeOrderIdOf(*order_id, original.generation_)));
  ack(result.remaining_);
}

// Reports the fills of the last submit and claims the slot the taker rested in.
// Returns the taker's exchange_order_id: of its new slot, or p_unrested_id.
wire::ExchangeOrderId Engine::Settle(Instrument &p_inst, const SubmitResult &p_result,
                                     const OrderMeta &p_taker, Quantity p_leaves,
                                     wire::ExchangeOrderId p_unrested_id)
{
  const std::span<const Fill> fills = p_inst.book_.Fills();

  // 1. Makers first, while meta_[maker_id_] is still theirs: Rest may have
  //    reused a slot that matching freed.
  for (const Fill &fill : fills)
  {
    OnMakerFill(p_inst, fill);
  }

  // 2. Only now claim the slot.
  wire::ExchangeOrderId exchange_order_id = p_unrested_id;
  if (p_result.order_id_ != kInvalidOrderId)
  {
    OrderMeta &meta = p_inst.meta_[p_result.order_id_];
    meta.owner_ = p_taker.owner_;
    meta.client_order_id_ = p_taker.client_order_id_;
    meta.cum_qty_ = p_taker.cum_qty_ + p_result.filled_qty_;
    ++meta.generation_;
    p_inst.index_.Insert(p_taker.owner_, p_taker.client_order_id_, p_result.order_id_);
    exchange_order_id = ExchangeOrderIdOf(p_result.order_id_, meta.generation_);
  }

  // 3. Taker reports. leaves counts down from the quantity sent to the book.
  Quantity leaves = p_leaves;
  for (const Fill &fill : fills)
  {
    leaves -= fill.quantity_;
    Emit(p_taker.owner_, wire::Fill{.exchange_order_id_ = exchange_order_id,
                                    .client_order_id_ = p_taker.client_order_id_,
                                    .symbol_id_ = p_inst.symbol_id_,
                                    .price_ = fill.price_,
                                    .qty_ = fill.quantity_,
                                    .leaves_ = leaves,
                                    .liquidity_ = wire::Liquidity::Taker});
  }
  return exchange_order_id;
}

void Engine::OnMakerFill(Instrument &p_inst, const Fill &p_fill)
{
  OrderMeta &meta = p_inst.meta_[p_fill.maker_id_];
  meta.cum_qty_ += p_fill.quantity_;
  // Only the last fill can leave its maker resting, and that slot is not reused.
  const Quantity leaves =
      p_fill.maker_removed_ ? Quantity{} : p_inst.book_.Order(p_fill.maker_id_).quantity_;
  Emit(meta.owner_,
       wire::Fill{.exchange_order_id_ = ExchangeOrderIdOf(p_fill.maker_id_, meta.generation_),
                  .client_order_id_ = meta.client_order_id_,
                  .symbol_id_ = p_inst.symbol_id_,
                  .price_ = p_fill.price_,
                  .qty_ = p_fill.quantity_,
                  .leaves_ = leaves,
                  .liquidity_ = wire::Liquidity::Maker});
  if (p_fill.maker_removed_)
  {
    p_inst.index_.Erase(meta.owner_, meta.client_order_id_);
  }
}

void Engine::Emit(SessionId p_session, const OrderReport &p_report)
{
  if (!reports_enabled_)
  {
    return;
  }
  const Report report{.session_id_ = p_session, .report_ = p_report};
  if (outbound_.TryPush(report))
  {
    return;
  }
  // Single writer, so a load and a store are enough.
  outbound_stalls_.store(outbound_stalls_.load(std::memory_order_relaxed) + 1,
                         std::memory_order_relaxed);
  // The gateway never blocks, so it always drains this queue.
  while (!outbound_.TryPush(report))
  {
  }
}

} // namespace mex
