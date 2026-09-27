#include "book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace mex
{
namespace
{

[[nodiscard]] bool Ascending(Side p_side) { return p_side == Side::Buy; }

// Bids are stored low to high, asks high to low, so the best price is always back().
[[nodiscard]] std::size_t FindLevel(const std::vector<PriceLevel> &p_levels, std::int64_t p_price,
                                    bool p_ascending)
{
  const auto it = std::lower_bound(p_levels.begin(), p_levels.end(), p_price,
                                   [p_ascending](PriceLevel p_level, std::int64_t p_key)
                                   {
                                     if (p_ascending)
                                     {
                                       return p_level.price_ < p_key;
                                     }
                                     return p_level.price_ > p_key;
                                   });
  return static_cast<std::size_t>(it - p_levels.begin());
}

[[nodiscard]] bool Crosses(Side p_aggressor, std::int64_t p_limit_price, std::int64_t p_maker_price)
{
  if (p_aggressor == Side::Buy)
  {
    return p_limit_price >= p_maker_price;
  }
  return p_limit_price <= p_maker_price;
}

[[nodiscard]] SubmitResult Accepted(std::uint32_t p_filled_qty, std::uint32_t p_remaining,
                                    OrderId p_order_id)
{
  SubmitResult result;
  result.status_ = SubmitStatus::Accepted;
  result.filled_qty_ = p_filled_qty;
  result.remaining_ = p_remaining;
  result.order_id_ = p_order_id;
  return result;
}

[[nodiscard]] SubmitResult Rejected(std::uint32_t p_remaining)
{
  SubmitResult result;
  result.status_ = SubmitStatus::Rejected;
  result.filled_qty_ = 0;
  result.remaining_ = p_remaining;
  result.order_id_ = kInvalidOrderId;
  return result;
}

} // namespace

Book::Book(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels)
    : pool_(p_max_orders), free_list_(p_max_orders), max_price_levels_(p_max_price_levels)
{
  std::iota(free_list_.begin(), free_list_.end(), OrderId{0});
  bids_.reserve(p_max_price_levels);
  asks_.reserve(p_max_price_levels);
}

SubmitResult Book::SubmitLimit(Side p_side, std::int64_t p_price, std::uint32_t p_quantity,
                               FillCallback p_on_fill, void *p_context)
{
  if (p_quantity == 0)
  {
    return Rejected(0);
  }

  const std::uint32_t filled = Match(p_side, p_price, p_quantity, p_on_fill, p_context);
  const std::uint32_t remaining = p_quantity - filled;
  if (remaining == 0)
  {
    return Accepted(filled, 0, kInvalidOrderId);
  }

  // Matching runs first because a fill can free the slot or the level this
  // remainder needs. A reject with no fills has not changed the book.
  // A partial fill that then cannot rest keeps the fills and drops the rest.
  if (!CanRest(p_side, p_price))
  {
    if (filled == 0)
    {
      return Rejected(p_quantity);
    }
    return Accepted(filled, remaining, kInvalidOrderId);
  }

  const OrderId order_id = Rest(p_side, p_price, remaining);
  return Accepted(filled, remaining, order_id);
}

SubmitResult Book::SubmitMarket(Side p_side, std::uint32_t p_quantity, FillCallback p_on_fill,
                                void *p_context)
{
  if (p_quantity == 0)
  {
    return Rejected(0);
  }

  const std::uint32_t filled = Match(p_side, std::nullopt, p_quantity, p_on_fill, p_context);
  if (filled == 0)
  {
    return Rejected(0);
  }

  // A market order never rests. Unfilled quantity is cancelled.
  return Accepted(filled, 0, kInvalidOrderId);
}

bool Book::CancelOrder(const OrderId p_order_id)
{
  if (p_order_id >= pool_.size())
  {
    return false;
  }

  OrderNode &node = pool_[p_order_id];
  if (node.quantity_ == 0)
  {
    return false;
  }

  std::vector<PriceLevel> &levels = Levels(node.side_);
  const std::size_t index = FindLevel(levels, node.price_, Ascending(node.side_));
  contract_assert(index < levels.size() && levels[index].price_ == node.price_);
  PriceLevel &level = levels[index];
  contract_assert(level.total_qty_ >= node.quantity_ && level.order_count_ > 0);
  Unlink(level, p_order_id);
  level.total_qty_ -= node.quantity_;
  --level.order_count_;
  Release(p_order_id);

  if (level.order_count_ == 0)
  {
    levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(index));
  }
  return true;
}

const OrderNode &Book::Order(OrderId p_order_id) const { return pool_[p_order_id]; }

const std::vector<PriceLevel> &Book::Bids() const { return bids_; }

const std::vector<PriceLevel> &Book::Asks() const { return asks_; }

std::uint32_t Book::FreeSlotCount() const { return static_cast<std::uint32_t>(free_list_.size()); }

std::uint32_t Book::MaxOrders() const { return static_cast<std::uint32_t>(pool_.size()); }

std::uint32_t Book::Match(Side p_aggressor, std::optional<std::int64_t> p_limit_price,
                          std::uint32_t p_quantity, FillCallback p_on_fill, void *p_context)
{
  std::uint32_t remaining = p_quantity;
  std::vector<PriceLevel> &levels = Levels(p_aggressor == Side::Buy ? Side::Sell : Side::Buy);

  while (remaining > 0 && !levels.empty())
  {
    PriceLevel &level = levels.back();
    if (p_limit_price.has_value() && !Crosses(p_aggressor, *p_limit_price, level.price_))
    {
      break;
    }

    while (remaining > 0 && level.head_ != kInvalidOrderId)
    {
      const OrderId maker_id = level.head_;
      contract_assert(maker_id < pool_.size());
      OrderNode &maker = pool_[maker_id];
      contract_assert(maker.quantity_ > 0 && level.total_qty_ >= maker.quantity_);
      const std::uint32_t fill_qty = remaining < maker.quantity_ ? remaining : maker.quantity_;

      maker.quantity_ -= fill_qty;
      level.total_qty_ -= fill_qty;
      remaining -= fill_qty;
      p_on_fill(maker_id, level.price_, fill_qty, p_context);

      if (maker.quantity_ == 0)
      {
        Unlink(level, maker_id);
        --level.order_count_;
        Release(maker_id);
      }
    }

    contract_assert(level.head_ != kInvalidOrderId || level.order_count_ == 0);
    if (level.order_count_ == 0)
    {
      levels.pop_back();
    }
  }

  return p_quantity - remaining;
}

bool Book::CanRest(Side p_side, std::int64_t p_price) const
{
  if (free_list_.empty())
  {
    return false;
  }

  const std::vector<PriceLevel> &levels = Levels(p_side);
  const std::size_t index = FindLevel(levels, p_price, Ascending(p_side));
  const bool level_exists = index < levels.size() && levels[index].price_ == p_price;
  if (level_exists)
  {
    return true;
  }
  return levels.size() < static_cast<std::size_t>(max_price_levels_);
}

OrderId Book::Rest(Side p_side, std::int64_t p_price, std::uint32_t p_quantity)
{
  const OrderId order_id = free_list_.back();
  free_list_.pop_back();
  contract_assert(order_id < pool_.size());

  OrderNode &node = pool_[order_id];
  node.price_ = p_price;
  node.quantity_ = p_quantity;
  node.side_ = p_side;
  node.next_ = kInvalidOrderId;

  std::vector<PriceLevel> &levels = Levels(p_side);
  const std::size_t index = FindLevel(levels, p_price, Ascending(p_side));
  if (index < levels.size() && levels[index].price_ == p_price)
  {
    PriceLevel &level = levels[index];
    contract_assert(level.order_count_ > 0 && level.tail_ < pool_.size());
    node.prev_ = level.tail_;
    if (level.tail_ != kInvalidOrderId)
    {
      pool_[level.tail_].next_ = order_id;
    }
    else
    {
      level.head_ = order_id;
    }
    level.tail_ = order_id;
    ++level.order_count_;
    level.total_qty_ += p_quantity;
    return order_id;
  }

  contract_assert(levels.size() < static_cast<std::size_t>(max_price_levels_) &&
                  levels.size() < levels.capacity());
  node.prev_ = kInvalidOrderId;
  PriceLevel level;
  level.price_ = p_price;
  level.head_ = order_id;
  level.tail_ = order_id;
  level.order_count_ = 1;
  level.total_qty_ = p_quantity;
  levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(index), level);
  return order_id;
}

void Book::Unlink(PriceLevel &p_level, OrderId p_order_id)
{
  OrderNode &node = pool_[p_order_id];
  if (node.prev_ != kInvalidOrderId)
  {
    pool_[node.prev_].next_ = node.next_;
  }
  else
  {
    p_level.head_ = node.next_;
  }

  if (node.next_ != kInvalidOrderId)
  {
    pool_[node.next_].prev_ = node.prev_;
  }
  else
  {
    p_level.tail_ = node.prev_;
  }
}

void Book::Release(OrderId p_order_id)
{
  OrderNode &node = pool_[p_order_id];
  node.quantity_ = 0;
  node.prev_ = kInvalidOrderId;
  node.next_ = kInvalidOrderId;
  free_list_.push_back(p_order_id);
}

std::vector<PriceLevel> &Book::Levels(Side p_side)
{
  if (p_side == Side::Buy)
  {
    return bids_;
  }
  return asks_;
}

const std::vector<PriceLevel> &Book::Levels(Side p_side) const
{
  if (p_side == Side::Buy)
  {
    return bids_;
  }
  return asks_;
}

} // namespace mex
