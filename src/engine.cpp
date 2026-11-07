#include "engine.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace mex
{

ClientOrderIndex::ClientOrderIndex(std::uint32_t p_max_orders)
    : mask_(std::bit_ceil(std::size_t{2} * p_max_orders) - 1),
      entries_(std::make_unique<Entry[]>(mask_ + 1)), size_(0), max_size_(p_max_orders)
{
}

std::size_t ClientOrderIndex::HomeSlot(SessionId p_owner,
                                       wire::ClientOrderId p_client_order_id) const
{
  // splitmix64 finalizer over the id mixed with the owner.
  std::uint64_t hash = p_client_order_id ^ (std::uint64_t{p_owner} * 0x9E3779B97F4A7C15ULL);
  hash = (hash ^ (hash >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  hash = (hash ^ (hash >> 27U)) * 0x94D049BB133111EBULL;
  hash ^= hash >> 31U;
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

struct Engine::Instrument
{
  explicit Instrument(const BookConfig &p_config)
      : book_(p_config.max_orders_, p_config.max_price_levels_),
        meta_(std::make_unique<OrderMeta[]>(p_config.max_orders_)), index_(p_config.max_orders_),
        symbol_id_(p_config.symbol_id_)
  {
  }

  Book book_;
  std::unique_ptr<OrderMeta[]> meta_;
  ClientOrderIndex index_;
  wire::SymbolId symbol_id_;
};

Engine::Engine(std::span<const BookConfig> p_books, SpscQueue<Command> &p_inbound,
               SpscQueue<Report> &p_outbound)
    : inbound_(p_inbound), outbound_(p_outbound)
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
  contract_assert(command->symbol_slot_ < instruments_.size());
  Instrument &inst = *instruments_[command->symbol_slot_];
  if (const auto *order = std::get_if<wire::NewOrder>(&command->order_))
  {
    OnNewOrder(command->session_id_, inst, *order);
  }
  else if (const auto *cancel = std::get_if<wire::Cancel>(&command->order_))
  {
    OnCancel(command->session_id_, inst, *cancel);
  }
  else
  {
    const auto *replace = std::get_if<wire::Replace>(&command->order_);
    contract_assert(replace != nullptr);
    OnReplace(command->session_id_, inst, *replace);
  }
  return true;
}

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

void Engine::OnNewOrder(SessionId p_session, Instrument &p_inst, const wire::NewOrder &p_order)
{
  contract_assert(p_order.symbol_id_ == p_inst.symbol_id_);
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

void Engine::OnCancel(SessionId p_session, Instrument &p_inst, const wire::Cancel &p_cancel)
{
  contract_assert(p_cancel.symbol_id_ == p_inst.symbol_id_);
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

void Engine::OnReplace(SessionId p_session, Instrument &p_inst, const wire::Replace &p_replace)
{
  contract_assert(p_replace.symbol_id_ == p_inst.symbol_id_);
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
