#include "book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace mex
{
namespace
{

[[nodiscard]] bool IsBuy(Side p_side) { return p_side == Side::Buy; }

[[nodiscard]] bool IsSell(Side p_side) { return p_side == Side::Sell; }

[[nodiscard]] bool Ascending(Side p_side)
{
  contract_assert(IsBuy(p_side) || IsSell(p_side));
  return IsBuy(p_side);
}

[[nodiscard]] Side Opposite(Side p_side)
{
  if (IsBuy(p_side))
  {
    return Side::Sell;
  }
  contract_assert(IsSell(p_side));
  return Side::Buy;
}

// Bids are stored low to high, asks high to low, so the best price is always back().
[[nodiscard]] std::uint32_t FindLevel(const PriceLevel *p_levels, std::uint32_t p_count,
                                      Price p_price, bool p_ascending)
{
  contract_assert(p_levels != nullptr || p_count == 0);
  const PriceLevel *const end = p_levels + p_count;
  const PriceLevel *const it = std::lower_bound(p_levels, end, p_price,
                                                [p_ascending](PriceLevel p_level, Price p_key)
                                                {
                                                  if (p_ascending)
                                                  {
                                                    return p_level.price_ < p_key;
                                                  }
                                                  return p_level.price_ > p_key;
                                                });
  const auto offset = static_cast<std::uint32_t>(it - p_levels);
  contract_assert(offset <= p_count);
  return offset;
}

[[nodiscard]] bool Crosses(Side p_aggressor, Price p_limit_price, Price p_maker_price)
{
  if (IsBuy(p_aggressor))
  {
    return p_limit_price >= p_maker_price;
  }
  contract_assert(IsSell(p_aggressor));
  return p_limit_price <= p_maker_price;
}

[[nodiscard]] SubmitResult Accepted(Quantity p_filled_qty, Quantity p_remaining, OrderId p_order_id)
{
  contract_assert(p_order_id == kInvalidOrderId || p_remaining > Quantity{});
  SubmitResult result;
  result.status_ = SubmitStatus::Accepted;
  result.filled_qty_ = p_filled_qty;
  result.remaining_ = p_remaining;
  result.order_id_ = p_order_id;
  return result;
}

[[nodiscard]] SubmitResult Rejected(Quantity p_remaining)
{
  SubmitResult result;
  result.status_ = SubmitStatus::Rejected;
  result.filled_qty_ = Quantity{};
  result.remaining_ = p_remaining;
  result.order_id_ = kInvalidOrderId;
  return result;
}

void AddLevelQty(PriceLevel &p_level, Quantity p_quantity) pre(p_quantity > Quantity{})
    pre(p_level.total_qty_ <= std::numeric_limits<std::uint64_t>::max() - p_quantity.units_)
{
  p_level.total_qty_ += p_quantity.units_;
  ++p_level.order_count_;
}

void SubLevelQty(PriceLevel &p_level, Quantity p_quantity) pre(p_quantity > Quantity{})
    pre(p_level.total_qty_ >= p_quantity.units_) pre(p_level.order_count_ > 0)
{
  p_level.total_qty_ -= p_quantity.units_;
  --p_level.order_count_;
}

void UnlinkNode(OrderNode *p_pool, std::uint32_t p_pool_size, PriceLevel &p_level,
                OrderId p_order_id)
{
  contract_assert(p_pool != nullptr);
  contract_assert(p_order_id < p_pool_size);
  contract_assert(p_level.order_count_ > 0);
  OrderNode &node = p_pool[p_order_id];
  contract_assert(node.prev_ == kInvalidOrderId || p_level.head_ != p_order_id);
  contract_assert(node.next_ == kInvalidOrderId || p_level.tail_ != p_order_id);
  contract_assert(node.prev_ != kInvalidOrderId || p_level.head_ == p_order_id);
  contract_assert(node.next_ != kInvalidOrderId || p_level.tail_ == p_order_id);

  if (node.prev_ != kInvalidOrderId)
  {
    contract_assert(node.prev_ < p_pool_size);
    p_pool[node.prev_].next_ = node.next_;
  }
  else
  {
    p_level.head_ = node.next_;
  }

  if (node.next_ != kInvalidOrderId)
  {
    contract_assert(node.next_ < p_pool_size);
    p_pool[node.next_].prev_ = node.prev_;
  }
  else
  {
    p_level.tail_ = node.prev_;
  }
}

void ReleaseSlot(OrderNode *p_pool, std::uint32_t p_pool_size, OrderId *p_free_ids,
                 std::uint32_t &p_free_count, OrderId p_order_id)
{
  contract_assert(p_pool != nullptr);
  contract_assert(p_free_ids != nullptr);
  contract_assert(p_order_id < p_pool_size);
  contract_assert(p_free_count < p_pool_size);
  OrderNode &node = p_pool[p_order_id];
  node.quantity_ = Quantity{};
  node.prev_ = kInvalidOrderId;
  node.next_ = kInvalidOrderId;
  p_free_ids[p_free_count] = p_order_id;
  ++p_free_count;
}

void RecordFill(Fill *p_fills, std::uint32_t p_capacity, std::uint32_t &p_count, OrderId p_maker_id,
                Price p_price, Quantity p_quantity, Side p_side, bool p_maker_removed)
{
  contract_assert(p_fills != nullptr);
  contract_assert(p_count < p_capacity);
  contract_assert(p_quantity > Quantity{});
  p_fills[p_count] = Fill{p_maker_id, p_price, p_quantity, p_side, p_maker_removed};
  ++p_count;
}

// One FIFO head fill. Leaves control of emptying the level to the caller.
void FillHead(OrderNode *p_pool, std::uint32_t p_pool_size, PriceLevel &p_level, Fill *p_fills,
              std::uint32_t p_fills_capacity, std::uint32_t &p_fill_count, OrderId *p_free_ids,
              std::uint32_t &p_free_count, Quantity &p_remaining)
{
  contract_assert(p_remaining > Quantity{});
  contract_assert(p_level.head_ != kInvalidOrderId);
  const OrderId maker_id = p_level.head_;
  contract_assert(maker_id < p_pool_size);
  OrderNode &maker = p_pool[maker_id];
  contract_assert(maker.quantity_ > Quantity{});
  contract_assert(p_level.total_qty_ >= maker.quantity_.units_);

  const Quantity fill_qty = p_remaining < maker.quantity_ ? p_remaining : maker.quantity_;
  maker.quantity_ -= fill_qty;
  p_level.total_qty_ -= fill_qty.units_;
  p_remaining -= fill_qty;
  RecordFill(p_fills, p_fills_capacity, p_fill_count, maker_id, p_level.price_, fill_qty,
             maker.side_, maker.quantity_ == Quantity{});

  if (maker.quantity_ == Quantity{})
  {
    UnlinkNode(p_pool, p_pool_size, p_level, maker_id);
    contract_assert(p_level.order_count_ > 0);
    --p_level.order_count_;
    ReleaseSlot(p_pool, p_pool_size, p_free_ids, p_free_count, maker_id);
  }
}

void AppendToLevel(OrderNode *p_pool, std::uint32_t p_pool_size, PriceLevel &p_level,
                   OrderId p_order_id, Quantity p_quantity)
{
  contract_assert(p_pool != nullptr);
  contract_assert(p_order_id < p_pool_size);
  contract_assert(p_level.order_count_ > 0);
  contract_assert(p_level.tail_ < p_pool_size);
  OrderNode &node = p_pool[p_order_id];
  node.prev_ = p_level.tail_;
  node.next_ = kInvalidOrderId;
  p_pool[p_level.tail_].next_ = p_order_id;
  p_level.tail_ = p_order_id;
  AddLevelQty(p_level, p_quantity);
}

} // namespace

PriceLadder::PriceLadder(std::uint32_t p_capacity) : size_(0), capacity_(p_capacity)
{
  if (p_capacity > 0)
  {
    storage_ = std::make_unique<PriceLevel[]>(p_capacity);
  }
}

bool PriceLadder::empty() const { return size_ == 0; }

std::size_t PriceLadder::size() const { return size_; }

std::uint32_t PriceLadder::Capacity() const { return capacity_; }

PriceLevel &PriceLadder::operator[](std::size_t p_index) { return storage_[p_index]; }

const PriceLevel &PriceLadder::operator[](std::size_t p_index) const { return storage_[p_index]; }

PriceLevel &PriceLadder::front() { return storage_[0]; }

const PriceLevel &PriceLadder::front() const { return storage_[0]; }

PriceLevel &PriceLadder::back() { return storage_[size_ - 1]; }

const PriceLevel &PriceLadder::back() const { return storage_[size_ - 1]; }

PriceLevel *PriceLadder::begin() { return storage_.get(); }

PriceLevel *PriceLadder::end() { return storage_.get() + size_; }

const PriceLevel *PriceLadder::begin() const { return storage_.get(); }

const PriceLevel *PriceLadder::end() const { return storage_.get() + size_; }

void PriceLadder::Insert(std::uint32_t p_index, PriceLevel p_level)
{
  PriceLevel *const data = storage_.get();
  if (p_index < size_)
  {
    std::move_backward(data + p_index, data + size_, data + size_ + 1);
  }
  data[p_index] = p_level;
  ++size_;
}

void PriceLadder::Erase(std::uint32_t p_index)
{
  PriceLevel *const data = storage_.get();
  if (p_index + 1 < size_)
  {
    std::move(data + p_index + 1, data + size_, data + p_index);
  }
  --size_;
}

void PriceLadder::PopBack() { --size_; }

Book::Book(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels)
    : pool_(std::make_unique<OrderNode[]>(p_max_orders)), pool_size_(p_max_orders),
      free_ids_(std::make_unique<OrderId[]>(p_max_orders)), free_count_(p_max_orders),
      bids_(p_max_price_levels), asks_(p_max_price_levels), max_price_levels_(p_max_price_levels),
      // One fill per distinct maker. Every maker is a resting order, so the
      // buffer cannot exceed max_orders (tight: one market order can take
      // every slot). The overflow check is a contract; bench_book ignores
      // contracts, so this size is what keeps that path in bounds.
      fills_(std::make_unique<Fill[]>(p_max_orders)), fill_count_(0)
{
  contract_assert(bids_.Capacity() == p_max_price_levels);
  contract_assert(asks_.Capacity() == p_max_price_levels);
  for (std::uint32_t i = 0; i < p_max_orders; ++i)
  {
    free_ids_[i] = i;
    contract_assert(pool_[i].quantity_ == Quantity{});
  }
}

void Book::ClearFills() { fill_count_ = 0; }

SubmitResult Book::SubmitLimitOrder(Side p_side, Price p_price, const Quantity p_quantity)
{
  ClearFills();
  contract_assert(IsBuy(p_side) || IsSell(p_side));
  if (p_quantity == Quantity{})
  {
    return Rejected(Quantity{});
  }

  const Quantity filled = Match(p_side, &p_price, p_quantity);
  const Quantity remaining = p_quantity - filled;
  if (remaining == Quantity{})
  {
    return Accepted(filled, Quantity{}, kInvalidOrderId);
  }

  // Matching runs first because a fill can free the slot or the level this
  // remainder needs. A reject with no fills has not changed the book.
  // A partial fill that then cannot rest keeps the fills and drops the rest.
  if (!CanRest(p_side, p_price))
  {
    if (filled == Quantity{})
    {
      return Rejected(p_quantity);
    }
    return Accepted(filled, remaining, kInvalidOrderId);
  }

  const OrderId order_id = Rest(p_side, p_price, remaining);
  return Accepted(filled, remaining, order_id);
}

SubmitResult Book::SubmitMarketOrder(Side p_side, const Quantity p_quantity)
{
  ClearFills();
  contract_assert(IsBuy(p_side) || IsSell(p_side));
  if (p_quantity == Quantity{})
  {
    return Rejected(Quantity{});
  }

  const Quantity filled = Match(p_side, nullptr, p_quantity);
  if (filled == Quantity{})
  {
    return Rejected(Quantity{});
  }

  // A market order never rests. Unfilled quantity is cancelled.
  return Accepted(filled, Quantity{}, kInvalidOrderId);
}

bool Book::CancelOrder(const OrderId p_order_id)
{
  if (p_order_id >= pool_size_)
  {
    return false;
  }

  OrderNode &node = pool_[p_order_id];
  if (node.quantity_ == Quantity{})
  {
    return false;
  }

  PriceLadder &levels = Levels(node.side_);
  const std::uint32_t index = FindLevel(levels.begin(), static_cast<std::uint32_t>(levels.size()),
                                        node.price_, Ascending(node.side_));
  contract_assert(index < levels.size());
  contract_assert(levels[index].price_ == node.price_);
  PriceLevel &level = levels[index];
  contract_assert(level.total_qty_ >= node.quantity_.units_);
  Unlink(level, p_order_id);
  SubLevelQty(level, node.quantity_);
  Release(p_order_id);

  if (level.order_count_ == 0)
  {
    contract_assert(level.head_ == kInvalidOrderId);
    contract_assert(level.tail_ == kInvalidOrderId);
    contract_assert(level.total_qty_ == 0);
    levels.Erase(index);
  }
  return true;
}

const OrderNode &Book::Order(OrderId p_order_id) const { return pool_[p_order_id]; }

const PriceLadder &Book::Bids() const { return bids_; }

const PriceLadder &Book::Asks() const { return asks_; }

std::span<const Fill> Book::Fills() const
{
  return std::span<const Fill>(fills_.get(), fill_count_);
}

std::uint32_t Book::FreeSlotCount() const { return free_count_; }

std::uint32_t Book::MaxOrders() const { return pool_size_; }

Quantity Book::Match(Side p_aggressor, const Price *p_limit_price, Quantity p_quantity)
{
  contract_assert(p_quantity > Quantity{});
  Quantity remaining = p_quantity;
  PriceLadder &levels = Levels(Opposite(p_aggressor));
  const std::uint32_t level_bound = max_price_levels_ == 0 ? 1 : max_price_levels_;
  std::uint32_t levels_seen = 0;
  std::uint32_t fills_seen = 0;

  while (remaining > Quantity{} && !levels.empty())
  {
    contract_assert(levels_seen < level_bound);
    ++levels_seen;

    PriceLevel &level = levels.back();
    if (p_limit_price != nullptr && !Crosses(p_aggressor, *p_limit_price, level.price_))
    {
      break;
    }

    while (remaining > Quantity{} && level.head_ != kInvalidOrderId)
    {
      contract_assert(fills_seen < pool_size_);
      ++fills_seen;
      FillHead(pool_.get(), pool_size_, level, fills_.get(), pool_size_, fill_count_,
               free_ids_.get(), free_count_, remaining);
    }

    contract_assert(level.head_ != kInvalidOrderId || level.order_count_ == 0);
    if (level.order_count_ == 0)
    {
      contract_assert(level.head_ == kInvalidOrderId);
      contract_assert(level.total_qty_ == 0);
      levels.PopBack();
    }
  }

  return p_quantity - remaining;
}

bool Book::CanRest(Side p_side, Price p_price) const
{
  if (free_count_ == 0)
  {
    return false;
  }

  const PriceLadder &levels = Levels(p_side);
  const std::uint32_t index = FindLevel(levels.begin(), static_cast<std::uint32_t>(levels.size()),
                                        p_price, Ascending(p_side));
  if (index < levels.size() && levels[index].price_ == p_price)
  {
    return true;
  }
  return levels.size() < static_cast<std::size_t>(max_price_levels_);
}

OrderId Book::Rest(Side p_side, Price p_price, Quantity p_quantity)
{
  contract_assert(free_count_ > 0);
  --free_count_;
  const OrderId order_id = free_ids_[free_count_];
  contract_assert(order_id < pool_size_);

  OrderNode &node = pool_[order_id];
  contract_assert(node.quantity_ == Quantity{});
  node.price_ = p_price;
  node.quantity_ = p_quantity;
  node.side_ = p_side;
  node.next_ = kInvalidOrderId;

  PriceLadder &levels = Levels(p_side);
  const std::uint32_t index = FindLevel(levels.begin(), static_cast<std::uint32_t>(levels.size()),
                                        p_price, Ascending(p_side));
  if (index < levels.size() && levels[index].price_ == p_price)
  {
    AppendToLevel(pool_.get(), pool_size_, levels[index], order_id, p_quantity);
    return order_id;
  }

  contract_assert(levels.size() < levels.Capacity());
  node.prev_ = kInvalidOrderId;
  PriceLevel level;
  level.price_ = p_price;
  level.head_ = order_id;
  level.tail_ = order_id;
  level.order_count_ = 1;
  level.total_qty_ = p_quantity.units_;
  levels.Insert(index, level);
  return order_id;
}

void Book::Unlink(PriceLevel &p_level, OrderId p_order_id)
{
  UnlinkNode(pool_.get(), pool_size_, p_level, p_order_id);
}

void Book::Release(OrderId p_order_id)
{
  ReleaseSlot(pool_.get(), pool_size_, free_ids_.get(), free_count_, p_order_id);
}

PriceLadder &Book::Levels(Side p_side)
{
  if (IsBuy(p_side))
  {
    return bids_;
  }
  contract_assert(IsSell(p_side));
  return asks_;
}

const PriceLadder &Book::Levels(Side p_side) const
{
  if (IsBuy(p_side))
  {
    return bids_;
  }
  contract_assert(IsSell(p_side));
  return asks_;
}

} // namespace mex
