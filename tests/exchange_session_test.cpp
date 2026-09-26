#include "book_checks.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace
{

using book_test::ExpectBookInvariants;

struct Trade
{
    std::int64_t price = 0;
    std::uint32_t quantity = 0;
};

struct LevelSnap
{
    std::int64_t price = 0;
    std::vector<std::uint32_t> quantities;
};

// Independent price-time book. It uses maps and deques, not the pool, so agreement
// means the real book is following exchange priority rather than its own storage layout.
class ShadowBook
{
public:
    ShadowBook(std::uint32_t max_orders, std::uint32_t max_price_levels)
        : max_orders_(max_orders)
        , max_price_levels_(max_price_levels)
        , free_slots_(max_orders)
        , orders_(1)
    {
    }

    [[nodiscard]] mex::SubmitResult SubmitLimit(mex::Side side, std::int64_t price, std::uint32_t quantity)
    {
        trades_.clear();
        if (quantity == 0)
        {
            return Rejected(0);
        }

        const std::uint32_t filled = Match(side, price, quantity);
        const std::uint32_t remaining = quantity - filled;
        if (remaining == 0)
        {
            return Accepted(filled, 0, mex::kInvalidOrderId);
        }
        if (!CanRest(side, price))
        {
            if (filled == 0)
            {
                return Rejected(quantity);
            }
            return Accepted(filled, remaining, mex::kInvalidOrderId);
        }

        const mex::OrderId order_id = Rest(side, price, remaining);
        return Accepted(filled, remaining, order_id);
    }

    [[nodiscard]] mex::SubmitResult SubmitMarket(mex::Side side, std::uint32_t quantity)
    {
        trades_.clear();
        if (quantity == 0)
        {
            return Rejected(0);
        }

        const std::uint32_t filled = Match(side, std::nullopt, quantity);
        if (filled == 0)
        {
            return Rejected(0);
        }
        return Accepted(filled, 0, mex::kInvalidOrderId);
    }

    [[nodiscard]] bool Cancel(mex::OrderId order_id)
    {
        if (order_id == mex::kInvalidOrderId || order_id >= orders_.size() || !orders_[order_id].live)
        {
            return false;
        }

        Order& order = orders_[order_id];
        auto& levels = Levels(order.side);
        auto level = levels.find(order.price);
        auto& queue = level->second;
        const auto it = std::find(queue.begin(), queue.end(), order_id);
        queue.erase(it);
        if (queue.empty())
        {
            levels.erase(level);
        }
        order.live = false;
        order.quantity = 0;
        ++free_slots_;
        return true;
    }

    [[nodiscard]] const std::vector<Trade>& Trades() const
    {
        return trades_;
    }

    [[nodiscard]] std::uint32_t FreeSlots() const
    {
        return free_slots_;
    }

    [[nodiscard]] std::vector<LevelSnap> LevelsAscending(mex::Side side) const
    {
        std::vector<LevelSnap> snaps;
        const auto& levels = side == mex::Side::kBuy ? bids_ : asks_;
        snaps.reserve(levels.size());
        for (const auto& [price, queue] : levels)
        {
            LevelSnap snap;
            snap.price = price;
            snap.quantities.reserve(queue.size());
            for (const mex::OrderId order_id : queue)
            {
                snap.quantities.push_back(orders_[order_id].quantity);
            }
            snaps.push_back(std::move(snap));
        }
        return snaps;
    }

private:
    struct Order
    {
        bool live = false;
        mex::Side side = mex::Side::kBuy;
        std::int64_t price = 0;
        std::uint32_t quantity = 0;
    };

    using Queue = std::deque<mex::OrderId>;

    [[nodiscard]] std::map<std::int64_t, Queue>& Levels(mex::Side side)
    {
        if (side == mex::Side::kBuy)
        {
            return bids_;
        }
        return asks_;
    }

    [[nodiscard]] bool CanRest(mex::Side side, std::int64_t price) const
    {
        if (free_slots_ == 0)
        {
            return false;
        }
        const auto& levels = side == mex::Side::kBuy ? bids_ : asks_;
        if (levels.find(price) != levels.end())
        {
            return true;
        }
        return levels.size() < static_cast<std::size_t>(max_price_levels_);
    }

    [[nodiscard]] mex::OrderId Rest(mex::Side side, std::int64_t price, std::uint32_t quantity)
    {
        const mex::OrderId order_id = next_id_;
        ++next_id_;
        if (orders_.size() <= order_id)
        {
            orders_.resize(static_cast<std::size_t>(order_id) + 1);
        }
        Order& order = orders_[order_id];
        order.live = true;
        order.side = side;
        order.price = price;
        order.quantity = quantity;
        Levels(side)[price].push_back(order_id);
        --free_slots_;
        return order_id;
    }

    [[nodiscard]] std::uint32_t Match(mex::Side aggressor, std::optional<std::int64_t> limit_price, std::uint32_t quantity)
    {
        std::uint32_t remaining = quantity;
        auto& levels = Levels(aggressor == mex::Side::kBuy ? mex::Side::kSell : mex::Side::kBuy);

        while (remaining > 0 && !levels.empty())
        {
            auto level = aggressor == mex::Side::kBuy ? levels.begin() : std::prev(levels.end());
            if (limit_price.has_value() && !Crosses(aggressor, *limit_price, level->first))
            {
                break;
            }

            Queue& queue = level->second;
            while (remaining > 0 && !queue.empty())
            {
                const mex::OrderId maker_id = queue.front();
                Order& maker = orders_[maker_id];
                const std::uint32_t fill_qty = remaining < maker.quantity ? remaining : maker.quantity;
                maker.quantity -= fill_qty;
                remaining -= fill_qty;
                trades_.push_back(Trade{level->first, fill_qty});
                if (maker.quantity == 0)
                {
                    maker.live = false;
                    queue.pop_front();
                    ++free_slots_;
                }
            }

            if (queue.empty())
            {
                levels.erase(level);
            }
        }
        return quantity - remaining;
    }

    [[nodiscard]] static bool Crosses(mex::Side aggressor, std::int64_t limit_price, std::int64_t maker_price)
    {
        if (aggressor == mex::Side::kBuy)
        {
            return limit_price >= maker_price;
        }
        return limit_price <= maker_price;
    }

    [[nodiscard]] static mex::SubmitResult Accepted(std::uint32_t filled, std::uint32_t remaining, mex::OrderId order_id)
    {
        mex::SubmitResult result;
        result.status = mex::SubmitStatus::kAccepted;
        result.filled_qty = filled;
        result.remaining = remaining;
        result.order_id = order_id;
        return result;
    }

    [[nodiscard]] static mex::SubmitResult Rejected(std::uint32_t remaining)
    {
        mex::SubmitResult result;
        result.status = mex::SubmitStatus::kRejected;
        result.filled_qty = 0;
        result.remaining = remaining;
        result.order_id = mex::kInvalidOrderId;
        return result;
    }

    std::uint32_t max_orders_ = 0;
    std::uint32_t max_price_levels_ = 0;
    std::uint32_t free_slots_ = 0;
    mex::OrderId next_id_ = 1;
    std::vector<Order> orders_;
    std::map<std::int64_t, Queue> bids_;
    std::map<std::int64_t, Queue> asks_;
    std::vector<Trade> trades_;
};

[[nodiscard]] std::vector<LevelSnap> LevelsAscending(const mex::Book& book, mex::Side side)
{
    std::vector<LevelSnap> snaps;
    const std::vector<mex::PriceLevel>& stored = side == mex::Side::kBuy ? book.Bids() : book.Asks();
    if (side == mex::Side::kBuy)
    {
        snaps.reserve(stored.size());
        for (const mex::PriceLevel& level : stored)
        {
            LevelSnap snap;
            snap.price = level.price;
            mex::OrderId id = level.head;
            while (id != mex::kInvalidOrderId)
            {
                snap.quantities.push_back(book.Order(id).quantity);
                id = book.Order(id).next;
            }
            snaps.push_back(std::move(snap));
        }
        return snaps;
    }

    snaps.reserve(stored.size());
    for (auto it = stored.rbegin(); it != stored.rend(); ++it)
    {
        LevelSnap snap;
        snap.price = it->price;
        mex::OrderId id = it->head;
        while (id != mex::kInvalidOrderId)
        {
            snap.quantities.push_back(book.Order(id).quantity);
            id = book.Order(id).next;
        }
        snaps.push_back(std::move(snap));
    }
    return snaps;
}

[[nodiscard]] bool SameLadder(const std::vector<LevelSnap>& left, const std::vector<LevelSnap>& right)
{
    if (left.size() != right.size())
    {
        ADD_FAILURE() << "level count " << left.size() << " vs " << right.size();
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        if (left[i].price != right[i].price || left[i].quantities != right[i].quantities)
        {
            ADD_FAILURE() << "level " << i << " price " << left[i].price << " vs " << right[i].price;
            return false;
        }
    }
    return true;
}

class Session
{
public:
    Session(std::uint32_t max_orders, std::uint32_t max_price_levels)
        : book_(max_orders, max_price_levels)
        , shadow_(max_orders, max_price_levels)
    {
    }

    [[nodiscard]] bool Limit(mex::Side side, std::int64_t price, std::uint32_t quantity)
    {
        return Submit(false, side, price, quantity);
    }

    [[nodiscard]] bool Market(mex::Side side, std::uint32_t quantity)
    {
        return Submit(true, side, 0, quantity);
    }

    [[nodiscard]] bool CancelAt(std::size_t live_index)
    {
        if (live_index >= live_.size())
        {
            ADD_FAILURE() << "cancel index " << live_index;
            return false;
        }
        const Live order = live_[live_index];
        const bool book_cancelled = book_.CancelOrder(order.book_id);
        const bool shadow_cancelled = shadow_.Cancel(order.shadow_id);
        if (book_cancelled != shadow_cancelled)
        {
            ADD_FAILURE() << "step " << step_ << " cancel disagreed";
            return false;
        }
        live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(live_index));
        return SameBook();
    }

    [[nodiscard]] bool CancelAll()
    {
        while (!live_.empty())
        {
            if (!CancelAt(live_.size() - 1))
            {
                return false;
            }
        }
        if (!book_.Bids().empty() || !book_.Asks().empty())
        {
            ADD_FAILURE() << "orders remained after cancelling every live id";
            return false;
        }
        return book_.FreeSlotCount() == book_.MaxOrders();
    }

    [[nodiscard]] std::size_t LiveCount() const
    {
        return live_.size();
    }

    void SetStep(int step)
    {
        step_ = step;
    }

private:
    struct Live
    {
        mex::OrderId book_id = mex::kInvalidOrderId;
        mex::OrderId shadow_id = mex::kInvalidOrderId;
    };

    [[nodiscard]] bool Submit(bool market, mex::Side side, std::int64_t price, std::uint32_t quantity)
    {
        trades_.clear();
        filled_ids_.clear();
        auto on_fill = [this](mex::OrderId maker_id, std::int64_t fill_price, std::uint32_t fill_qty)
        {
            trades_.push_back(Trade{fill_price, fill_qty});
            if (book_.Order(maker_id).quantity == 0)
            {
                filled_ids_.push_back(maker_id);
            }
        };

        const mex::SubmitResult book_result = market ? book_.SubmitMarketOrder(side, quantity, on_fill)
                                                     : book_.SubmitLimitOrder(side, price, quantity, on_fill);
        const mex::SubmitResult shadow_result = market ? shadow_.SubmitMarket(side, quantity)
                                                       : shadow_.SubmitLimit(side, price, quantity);
        if (!SameResult(book_result, shadow_result) || !SameTrades() || !SameBook())
        {
            return false;
        }

        live_.erase(std::remove_if(live_.begin(),
                                   live_.end(),
                                   [this](const Live& order)
                                   {
                                       return std::find(filled_ids_.begin(), filled_ids_.end(), order.book_id) !=
                                              filled_ids_.end();
                                   }),
                    live_.end());
        if (book_result.order_id != mex::kInvalidOrderId)
        {
            live_.push_back(Live{book_result.order_id, shadow_result.order_id});
        }
        return true;
    }

    [[nodiscard]] bool SameResult(const mex::SubmitResult& book_result, const mex::SubmitResult& shadow_result) const
    {
        if (book_result.status != shadow_result.status || book_result.filled_qty != shadow_result.filled_qty ||
            book_result.remaining != shadow_result.remaining ||
            (book_result.order_id == mex::kInvalidOrderId) != (shadow_result.order_id == mex::kInvalidOrderId))
        {
            ADD_FAILURE() << "step " << step_ << " result filled " << book_result.filled_qty << " vs "
                          << shadow_result.filled_qty << " remaining " << book_result.remaining << " vs "
                          << shadow_result.remaining;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool SameTrades() const
    {
        const std::vector<Trade>& shadow_trades = shadow_.Trades();
        if (trades_.size() != shadow_trades.size())
        {
            ADD_FAILURE() << "step " << step_ << " trade count " << trades_.size() << " vs " << shadow_trades.size();
            return false;
        }
        for (std::size_t i = 0; i < trades_.size(); ++i)
        {
            if (trades_[i].price != shadow_trades[i].price || trades_[i].quantity != shadow_trades[i].quantity)
            {
                ADD_FAILURE() << "step " << step_ << " trade " << i;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool SameBook() const
    {
        if (!SameLadder(LevelsAscending(book_, mex::Side::kBuy), shadow_.LevelsAscending(mex::Side::kBuy)) ||
            !SameLadder(LevelsAscending(book_, mex::Side::kSell), shadow_.LevelsAscending(mex::Side::kSell)))
        {
            ADD_FAILURE() << "step " << step_;
            return false;
        }
        if (book_.FreeSlotCount() != shadow_.FreeSlots())
        {
            ADD_FAILURE() << "step " << step_ << " free slots " << book_.FreeSlotCount() << " vs " << shadow_.FreeSlots();
            return false;
        }
        ExpectBookInvariants(book_);
        return true;
    }

    mex::Book book_;
    ShadowBook shadow_;
    std::vector<Live> live_;
    std::vector<Trade> trades_;
    std::vector<mex::OrderId> filled_ids_;
    int step_ = 0;
};

TEST(ExchangeSession, OpeningBookThenSweepAndCancel)
{
    Session session(64, 16);

    for (std::int64_t price = 90; price <= 99; ++price)
    {
        ASSERT_TRUE(session.Limit(mex::Side::kBuy, price, 5));
        ASSERT_TRUE(session.Limit(mex::Side::kBuy, price, 3));
        ASSERT_TRUE(session.Limit(mex::Side::kBuy, price, 1));
    }
    for (std::int64_t price = 101; price <= 110; ++price)
    {
        ASSERT_TRUE(session.Limit(mex::Side::kSell, price, 4));
        ASSERT_TRUE(session.Limit(mex::Side::kSell, price, 6));
    }

    ASSERT_TRUE(session.CancelAt(1));
    ASSERT_TRUE(session.CancelAt(session.LiveCount() / 2));
    ASSERT_TRUE(session.CancelAt(session.LiveCount() - 1));

    ASSERT_TRUE(session.Limit(mex::Side::kBuy, 105, 30));
    ASSERT_TRUE(session.Market(mex::Side::kSell, 1000));
    ASSERT_TRUE(session.Market(mex::Side::kBuy, 1000));
    ASSERT_TRUE(session.Limit(mex::Side::kBuy, 100, 0));
    ASSERT_TRUE(session.Market(mex::Side::kSell, 0));
    ASSERT_TRUE(session.CancelAll());
}

TEST(ExchangeSession, DropsRemainderWhenTheNewLevelCannotFit)
{
    Session session(8, 1);
    ASSERT_TRUE(session.Limit(mex::Side::kBuy, 90, 5));
    ASSERT_TRUE(session.Limit(mex::Side::kSell, 100, 5));
    ASSERT_TRUE(session.Limit(mex::Side::kBuy, 100, 12));
    ASSERT_TRUE(session.Limit(mex::Side::kSell, 100, 4));
    ASSERT_TRUE(session.CancelAll());
}

TEST(ExchangeSession, RandomFlowMatchesTheReferenceBook)
{
    const struct
    {
        std::uint32_t seed;
        std::uint32_t max_orders;
        std::uint32_t max_levels;
        int price_span;
        int steps;
    } configs[] = {
        {42, 8, 2, 6, 1500},
        {7, 32, 4, 12, 2500},
        {99, 128, 16, 20, 4000},
    };

    for (const auto& config : configs)
    {
        SCOPED_TRACE(config.seed);
        Session session(config.max_orders, config.max_levels);
        std::mt19937 rng(config.seed);
        std::uniform_int_distribution<int> action(0, 99);
        std::uniform_int_distribution<int> price_dist(0, config.price_span);
        std::uniform_int_distribution<int> qty_dist(1, 9);
        std::uniform_int_distribution<int> side_bit(0, 1);

        for (int step = 0; step < config.steps; ++step)
        {
            session.SetStep(step);
            const int roll = action(rng);
            const mex::Side side = side_bit(rng) == 0 ? mex::Side::kBuy : mex::Side::kSell;
            if (roll < 60)
            {
                ASSERT_TRUE(session.Limit(side, price_dist(rng), static_cast<std::uint32_t>(qty_dist(rng))));
            }
            else if (roll < 80)
            {
                ASSERT_TRUE(session.Market(side, static_cast<std::uint32_t>(qty_dist(rng))));
            }
            else if (session.LiveCount() > 0)
            {
                std::uniform_int_distribution<std::size_t> pick(0, session.LiveCount() - 1);
                ASSERT_TRUE(session.CancelAt(pick(rng)));
            }
            else
            {
                ASSERT_TRUE(session.Limit(side, price_dist(rng), static_cast<std::uint32_t>(qty_dist(rng))));
            }
        }
        session.SetStep(config.steps);
        ASSERT_TRUE(session.CancelAll());
    }
}

}  // namespace
