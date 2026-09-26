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

[[nodiscard]] bool Ascending(Side side)
{
    return side == Side::kBuy;
}

// Bids are stored low to high, asks high to low, so the best price is always back().
[[nodiscard]] std::size_t FindLevel(const std::vector<PriceLevel>& levels,
                                    std::int64_t price,
                                    bool ascending)
{
    const auto it = std::lower_bound(
        levels.begin(),
        levels.end(),
        price,
        [ascending](const PriceLevel& level, std::int64_t key)
        {
            if (ascending)
            {
                return level.price < key;
            }
            return level.price > key;
        });
    return static_cast<std::size_t>(it - levels.begin());
}

[[nodiscard]] bool Crosses(Side aggressor, std::int64_t limit_price, std::int64_t maker_price)
{
    if (aggressor == Side::kBuy)
    {
        return limit_price >= maker_price;
    }
    return limit_price <= maker_price;
}

[[nodiscard]] SubmitResult Accepted(std::uint32_t filled_qty, std::uint32_t remaining, OrderId order_id)
{
    SubmitResult result;
    result.status = SubmitStatus::kAccepted;
    result.filled_qty = filled_qty;
    result.remaining = remaining;
    result.order_id = order_id;
    return result;
}

[[nodiscard]] SubmitResult Rejected(std::uint32_t remaining)
{
    SubmitResult result;
    result.status = SubmitStatus::kRejected;
    result.filled_qty = 0;
    result.remaining = remaining;
    result.order_id = kInvalidOrderId;
    return result;
}

}  // namespace

Book::Book(std::uint32_t max_orders, std::uint32_t max_price_levels)
    : pool_(max_orders)
    , free_list_(max_orders)
    , max_price_levels_(max_price_levels)
{
    std::iota(free_list_.begin(), free_list_.end(), OrderId{0});
    bids_.reserve(max_price_levels);
    asks_.reserve(max_price_levels);
}

SubmitResult Book::SubmitLimit(Side side,
                               std::int64_t price,
                               std::uint32_t quantity,
                               FillCallback on_fill,
                               void* context)
{
    if (quantity == 0)
    {
        return Rejected(0);
    }

    const std::uint32_t filled = Match(side, price, quantity, on_fill, context);
    const std::uint32_t remaining = quantity - filled;
    if (remaining == 0)
    {
        return Accepted(filled, 0, kInvalidOrderId);
    }

    // Matching runs first because a fill can free the slot or the level this
    // remainder needs. A reject with no fills has not changed the book.
    // A partial fill that then cannot rest keeps the fills and drops the rest.
    if (!CanRest(side, price))
    {
        if (filled == 0)
        {
            return Rejected(quantity);
        }
        return Accepted(filled, remaining, kInvalidOrderId);
    }

    const OrderId order_id = Rest(side, price, remaining);
    return Accepted(filled, remaining, order_id);
}

SubmitResult Book::SubmitMarket(Side side,
                                std::uint32_t quantity,
                                FillCallback on_fill,
                                void* context)
{
    if (quantity == 0)
    {
        return Rejected(0);
    }

    const std::uint32_t filled = Match(side, std::nullopt, quantity, on_fill, context);
    if (filled == 0)
    {
        return Rejected(0);
    }

    // A market order never rests. Unfilled quantity is cancelled.
    return Accepted(filled, 0, kInvalidOrderId);
}

bool Book::CancelOrder(OrderId order_id)
{
    if (order_id >= pool_.size())
    {
        return false;
    }

    OrderNode& node = pool_[order_id];
    if (node.quantity == 0)
    {
        return false;
    }

    std::vector<PriceLevel>& levels = Levels(node.side);
    const std::size_t index = FindLevel(levels, node.price, Ascending(node.side));
    PriceLevel& level = levels[index];
    level.total_qty -= node.quantity;
    --level.order_count;
    Unlink(level, order_id);
    Release(order_id);

    if (level.order_count == 0)
    {
        levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(index));
    }
    return true;
}

const OrderNode& Book::Order(OrderId order_id) const
{
    return pool_[order_id];
}

const std::vector<PriceLevel>& Book::Bids() const
{
    return bids_;
}

const std::vector<PriceLevel>& Book::Asks() const
{
    return asks_;
}

std::uint32_t Book::FreeSlotCount() const
{
    return static_cast<std::uint32_t>(free_list_.size());
}

std::uint32_t Book::MaxOrders() const
{
    return static_cast<std::uint32_t>(pool_.size());
}

std::uint32_t Book::Match(Side aggressor,
                          std::optional<std::int64_t> limit_price,
                          std::uint32_t quantity,
                          FillCallback on_fill,
                          void* context)
{
    std::uint32_t remaining = quantity;
    std::vector<PriceLevel>& levels = Levels(aggressor == Side::kBuy ? Side::kSell : Side::kBuy);

    while (remaining > 0 && !levels.empty())
    {
        PriceLevel& level = levels.back();
        if (limit_price.has_value() && !Crosses(aggressor, *limit_price, level.price))
        {
            break;
        }

        while (remaining > 0 && level.head != kInvalidOrderId)
        {
            const OrderId maker_id = level.head;
            OrderNode& maker = pool_[maker_id];
            const std::uint32_t fill_qty = remaining < maker.quantity ? remaining : maker.quantity;

            maker.quantity -= fill_qty;
            level.total_qty -= fill_qty;
            remaining -= fill_qty;
            on_fill(maker_id, level.price, fill_qty, context);

            if (maker.quantity == 0)
            {
                Unlink(level, maker_id);
                --level.order_count;
                Release(maker_id);
            }
        }

        if (level.order_count == 0)
        {
            levels.pop_back();
        }
    }

    return quantity - remaining;
}

bool Book::CanRest(Side side, std::int64_t price) const
{
    if (free_list_.empty())
    {
        return false;
    }

    const std::vector<PriceLevel>& levels = Levels(side);
    const std::size_t index = FindLevel(levels, price, Ascending(side));
    const bool level_exists = index < levels.size() && levels[index].price == price;
    if (level_exists)
    {
        return true;
    }
    return levels.size() < static_cast<std::size_t>(max_price_levels_);
}

OrderId Book::Rest(Side side, std::int64_t price, std::uint32_t quantity)
{
    const OrderId order_id = free_list_.back();
    free_list_.pop_back();

    OrderNode& node = pool_[order_id];
    node.price = price;
    node.quantity = quantity;
    node.side = side;
    node.next = kInvalidOrderId;

    std::vector<PriceLevel>& levels = Levels(side);
    const std::size_t index = FindLevel(levels, price, Ascending(side));
    if (index < levels.size() && levels[index].price == price)
    {
        PriceLevel& level = levels[index];
        node.prev = level.tail;
        if (level.tail != kInvalidOrderId)
        {
            pool_[level.tail].next = order_id;
        }
        else
        {
            level.head = order_id;
        }
        level.tail = order_id;
        ++level.order_count;
        level.total_qty += quantity;
        return order_id;
    }

    node.prev = kInvalidOrderId;
    PriceLevel level;
    level.price = price;
    level.head = order_id;
    level.tail = order_id;
    level.order_count = 1;
    level.total_qty = quantity;
    levels.insert(levels.begin() + static_cast<std::ptrdiff_t>(index), level);
    return order_id;
}

void Book::Unlink(PriceLevel& level, OrderId order_id)
{
    OrderNode& node = pool_[order_id];
    if (node.prev != kInvalidOrderId)
    {
        pool_[node.prev].next = node.next;
    }
    else
    {
        level.head = node.next;
    }

    if (node.next != kInvalidOrderId)
    {
        pool_[node.next].prev = node.prev;
    }
    else
    {
        level.tail = node.prev;
    }
}

void Book::Release(OrderId order_id)
{
    OrderNode& node = pool_[order_id];
    node.quantity = 0;
    node.prev = kInvalidOrderId;
    node.next = kInvalidOrderId;
    free_list_.push_back(order_id);
}

std::vector<PriceLevel>& Book::Levels(Side side)
{
    if (side == Side::kBuy)
    {
        return bids_;
    }
    return asks_;
}

const std::vector<PriceLevel>& Book::Levels(Side side) const
{
    if (side == Side::kBuy)
    {
        return bids_;
    }
    return asks_;
}

}  // namespace mex
