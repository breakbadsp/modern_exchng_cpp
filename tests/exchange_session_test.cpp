#include "book_checks.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
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
  mex::Price price_{};
  mex::Quantity quantity_{};
};

struct LevelSnap
{
  mex::Price price_{};
  std::vector<mex::Quantity> quantities_;
};

// Independent price-time book. It uses maps and deques, not the pool, so agreement
// means the real book is following exchange priority rather than its own storage layout.
class ShadowBook
{
public:
  ShadowBook(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels)
      : max_orders_(p_max_orders), max_price_levels_(p_max_price_levels), free_slots_(p_max_orders),
        orders_(1)
  {
  }

  [[nodiscard]] mex::SubmitResult SubmitLimit(mex::Side p_side, mex::Price p_price,
                                              mex::Quantity p_quantity)
  {
    trades_.clear();
    if (p_quantity == mex::Quantity{})
    {
      return Rejected(mex::Quantity{});
    }

    const mex::Quantity filled = Match(p_side, p_price, p_quantity);
    const mex::Quantity remaining = p_quantity - filled;
    if (remaining == mex::Quantity{})
    {
      return Accepted(filled, mex::Quantity{}, mex::kInvalidOrderId);
    }
    if (!CanRest(p_side, p_price))
    {
      if (filled == mex::Quantity{})
      {
        return Rejected(p_quantity);
      }
      return Accepted(filled, remaining, mex::kInvalidOrderId);
    }

    const mex::OrderId order_id = Rest(p_side, p_price, remaining);
    return Accepted(filled, remaining, order_id);
  }

  [[nodiscard]] mex::SubmitResult SubmitMarket(mex::Side p_side, mex::Quantity p_quantity)
  {
    trades_.clear();
    if (p_quantity == mex::Quantity{})
    {
      return Rejected(mex::Quantity{});
    }

    const mex::Quantity filled = Match(p_side, std::nullopt, p_quantity);
    if (filled == mex::Quantity{})
    {
      return Rejected(mex::Quantity{});
    }
    return Accepted(filled, mex::Quantity{}, mex::kInvalidOrderId);
  }

  [[nodiscard]] bool Cancel(mex::OrderId p_order_id)
  {
    if (p_order_id == mex::kInvalidOrderId || p_order_id >= orders_.size() ||
        !orders_[p_order_id].live_)
    {
      return false;
    }

    Order &order = orders_[p_order_id];
    auto &levels = Levels(order.side_);
    auto level = levels.find(order.price_);
    auto &queue = level->second;
    const auto it = std::find(queue.begin(), queue.end(), p_order_id);
    queue.erase(it);
    if (queue.empty())
    {
      levels.erase(level);
    }
    order.live_ = false;
    order.quantity_ = mex::Quantity{};
    ++free_slots_;
    return true;
  }

  [[nodiscard]] const std::vector<Trade> &Trades() const { return trades_; }

  [[nodiscard]] std::uint32_t FreeSlots() const { return free_slots_; }

  [[nodiscard]] std::vector<LevelSnap> LevelsAscending(mex::Side p_side) const
  {
    std::vector<LevelSnap> snaps;
    const auto &levels = p_side == mex::Side::Buy ? bids_ : asks_;
    snaps.reserve(levels.size());
    for (const auto &[price, queue] : levels)
    {
      LevelSnap snap;
      snap.price_ = price;
      snap.quantities_.reserve(queue.size());
      for (const mex::OrderId order_id : queue)
      {
        snap.quantities_.push_back(orders_[order_id].quantity_);
      }
      snaps.push_back(std::move(snap));
    }
    return snaps;
  }

private:
  struct Order
  {
    bool live_ = false;
    mex::Side side_ = mex::Side::Buy;
    mex::Price price_{};
    mex::Quantity quantity_{};
  };

  using Queue = std::deque<mex::OrderId>;

  [[nodiscard]] std::map<mex::Price, Queue> &Levels(mex::Side p_side)
  {
    if (p_side == mex::Side::Buy)
    {
      return bids_;
    }
    return asks_;
  }

  [[nodiscard]] bool CanRest(mex::Side p_side, mex::Price p_price) const
  {
    if (free_slots_ == 0)
    {
      return false;
    }
    const auto &levels = p_side == mex::Side::Buy ? bids_ : asks_;
    if (levels.find(p_price) != levels.end())
    {
      return true;
    }
    return levels.size() < static_cast<std::size_t>(max_price_levels_);
  }

  [[nodiscard]] mex::OrderId Rest(mex::Side p_side, mex::Price p_price, mex::Quantity p_quantity)
  {
    const mex::OrderId order_id = next_id_;
    ++next_id_;
    if (orders_.size() <= order_id)
    {
      orders_.resize(static_cast<std::size_t>(order_id) + 1);
    }
    Order &order = orders_[order_id];
    order.live_ = true;
    order.side_ = p_side;
    order.price_ = p_price;
    order.quantity_ = p_quantity;
    Levels(p_side)[p_price].push_back(order_id);
    --free_slots_;
    return order_id;
  }

  [[nodiscard]] mex::Quantity Match(mex::Side p_aggressor, std::optional<mex::Price> p_limit_price,
                                    mex::Quantity p_quantity)
  {
    mex::Quantity remaining = p_quantity;
    auto &levels = Levels(p_aggressor == mex::Side::Buy ? mex::Side::Sell : mex::Side::Buy);

    while (remaining > mex::Quantity{} && !levels.empty())
    {
      auto level = p_aggressor == mex::Side::Buy ? levels.begin() : std::prev(levels.end());
      if (p_limit_price.has_value() && !Crosses(p_aggressor, *p_limit_price, level->first))
      {
        break;
      }

      Queue &queue = level->second;
      while (remaining > mex::Quantity{} && !queue.empty())
      {
        const mex::OrderId maker_id = queue.front();
        Order &maker = orders_[maker_id];
        const mex::Quantity fill_qty = remaining < maker.quantity_ ? remaining : maker.quantity_;
        maker.quantity_ -= fill_qty;
        remaining -= fill_qty;
        trades_.push_back(Trade{level->first, fill_qty});
        if (maker.quantity_ == mex::Quantity{})
        {
          maker.live_ = false;
          queue.pop_front();
          ++free_slots_;
        }
      }

      if (queue.empty())
      {
        levels.erase(level);
      }
    }
    return p_quantity - remaining;
  }

  [[nodiscard]] static bool Crosses(mex::Side p_aggressor, mex::Price p_limit_price,
                                    mex::Price p_maker_price)
  {
    if (p_aggressor == mex::Side::Buy)
    {
      return p_limit_price >= p_maker_price;
    }
    return p_limit_price <= p_maker_price;
  }

  [[nodiscard]] static mex::SubmitResult Accepted(mex::Quantity p_filled, mex::Quantity p_remaining,
                                                  mex::OrderId p_order_id)
  {
    mex::SubmitResult result;
    result.status_ = mex::SubmitStatus::Accepted;
    result.filled_qty_ = p_filled;
    result.remaining_ = p_remaining;
    result.order_id_ = p_order_id;
    return result;
  }

  [[nodiscard]] static mex::SubmitResult Rejected(mex::Quantity p_remaining)
  {
    mex::SubmitResult result;
    result.status_ = mex::SubmitStatus::Rejected;
    result.filled_qty_ = mex::Quantity{};
    result.remaining_ = p_remaining;
    result.order_id_ = mex::kInvalidOrderId;
    return result;
  }

  std::uint32_t max_orders_ = 0;
  std::uint32_t max_price_levels_ = 0;
  std::uint32_t free_slots_ = 0;
  mex::OrderId next_id_ = 1;
  std::vector<Order> orders_;
  std::map<mex::Price, Queue> bids_;
  std::map<mex::Price, Queue> asks_;
  std::vector<Trade> trades_;
};

[[nodiscard]] std::vector<LevelSnap> LevelsAscending(const mex::Book &p_book, mex::Side p_side)
{
  std::vector<LevelSnap> snaps;
  const mex::PriceLadder &stored = p_side == mex::Side::Buy ? p_book.Bids() : p_book.Asks();
  snaps.reserve(stored.size());
  if (p_side == mex::Side::Buy)
  {
    for (const mex::PriceLevel &level : stored)
    {
      LevelSnap snap;
      snap.price_ = level.price_;
      mex::OrderId id = level.head_;
      while (id != mex::kInvalidOrderId)
      {
        snap.quantities_.push_back(p_book.Order(id).quantity_);
        id = p_book.Order(id).next_;
      }
      snaps.push_back(std::move(snap));
    }
    return snaps;
  }

  for (std::size_t i = stored.size(); i > 0; --i)
  {
    const mex::PriceLevel &level = stored[i - 1];
    LevelSnap snap;
    snap.price_ = level.price_;
    mex::OrderId id = level.head_;
    while (id != mex::kInvalidOrderId)
    {
      snap.quantities_.push_back(p_book.Order(id).quantity_);
      id = p_book.Order(id).next_;
    }
    snaps.push_back(std::move(snap));
  }
  return snaps;
}

[[nodiscard]] bool SameLadder(const std::vector<LevelSnap> &p_left,
                              const std::vector<LevelSnap> &p_right)
{
  if (p_left.size() != p_right.size())
  {
    ADD_FAILURE() << "level count " << p_left.size() << " vs " << p_right.size();
    return false;
  }
  for (std::size_t i = 0; i < p_left.size(); ++i)
  {
    if (p_left[i].price_ != p_right[i].price_ || p_left[i].quantities_ != p_right[i].quantities_)
    {
      ADD_FAILURE() << "level " << i << " price " << p_left[i].price_.ticks_ << " vs "
                    << p_right[i].price_.ticks_;
      return false;
    }
  }
  return true;
}

class Session
{
public:
  Session(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels)
      : book_(p_max_orders, p_max_price_levels), shadow_(p_max_orders, p_max_price_levels)
  {
  }

  [[nodiscard]] bool Limit(mex::Side p_side, std::int64_t p_price, std::uint32_t p_quantity)
  {
    return Submit(false, p_side, p_price, p_quantity);
  }

  [[nodiscard]] bool Market(mex::Side p_side, std::uint32_t p_quantity)
  {
    return Submit(true, p_side, 0, p_quantity);
  }

  [[nodiscard]] bool CancelAt(std::size_t p_live_index)
  {
    if (p_live_index >= live_.size())
    {
      ADD_FAILURE() << "cancel index " << p_live_index;
      return false;
    }
    const Live order = live_[p_live_index];
    const bool book_cancelled = book_.CancelOrder(order.book_id_);
    const bool shadow_cancelled = shadow_.Cancel(order.shadow_id_);
    if (book_cancelled != shadow_cancelled)
    {
      ADD_FAILURE() << "step " << step_ << " cancel disagreed";
      return false;
    }
    live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(p_live_index));
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

  [[nodiscard]] std::size_t LiveCount() const { return live_.size(); }

  void SetStep(int p_step) { step_ = p_step; }

private:
  struct Live
  {
    mex::OrderId book_id_ = mex::kInvalidOrderId;
    mex::OrderId shadow_id_ = mex::kInvalidOrderId;
  };

  [[nodiscard]] bool Submit(bool p_market, mex::Side p_side, std::int64_t p_price,
                            std::uint32_t p_quantity)
  {
    trades_.clear();
    filled_ids_.clear();

    const mex::Price price{p_price};
    const mex::Quantity quantity{p_quantity};
    const mex::SubmitResult book_result = p_market
                                              ? book_.SubmitMarketOrder(p_side, quantity)
                                              : book_.SubmitLimitOrder(p_side, price, quantity);
    for (const mex::Fill &fill : book_.Fills())
    {
      trades_.push_back(Trade{fill.price_, fill.quantity_});
      if (fill.maker_removed_)
      {
        filled_ids_.push_back(fill.maker_id_);
      }
    }
    const mex::SubmitResult shadow_result = p_market ? shadow_.SubmitMarket(p_side, quantity)
                                                     : shadow_.SubmitLimit(p_side, price, quantity);
    if (!SameResult(book_result, shadow_result) || !SameTrades() || !SameBook())
    {
      return false;
    }

    live_.erase(std::remove_if(live_.begin(), live_.end(),
                               [this](Live p_order)
                               {
                                 return std::find(filled_ids_.begin(), filled_ids_.end(),
                                                  p_order.book_id_) != filled_ids_.end();
                               }),
                live_.end());
    if (book_result.order_id_ != mex::kInvalidOrderId)
    {
      live_.push_back(Live{book_result.order_id_, shadow_result.order_id_});
    }
    return true;
  }

  [[nodiscard]] bool SameResult(mex::SubmitResult p_book_result,
                                mex::SubmitResult p_shadow_result) const
  {
    if (p_book_result.status_ != p_shadow_result.status_ ||
        p_book_result.filled_qty_ != p_shadow_result.filled_qty_ ||
        p_book_result.remaining_ != p_shadow_result.remaining_ ||
        (p_book_result.order_id_ == mex::kInvalidOrderId) !=
            (p_shadow_result.order_id_ == mex::kInvalidOrderId))
    {
      ADD_FAILURE() << "step " << step_ << " result filled " << p_book_result.filled_qty_.units_
                    << " vs " << p_shadow_result.filled_qty_.units_ << " remaining "
                    << p_book_result.remaining_.units_ << " vs "
                    << p_shadow_result.remaining_.units_;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool SameTrades() const
  {
    const std::vector<Trade> &shadow_trades = shadow_.Trades();
    if (trades_.size() != shadow_trades.size())
    {
      ADD_FAILURE() << "step " << step_ << " trade count " << trades_.size() << " vs "
                    << shadow_trades.size();
      return false;
    }
    for (std::size_t i = 0; i < trades_.size(); ++i)
    {
      if (trades_[i].price_ != shadow_trades[i].price_ ||
          trades_[i].quantity_ != shadow_trades[i].quantity_)
      {
        ADD_FAILURE() << "step " << step_ << " trade " << i;
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool SameBook() const
  {
    if (!SameLadder(LevelsAscending(book_, mex::Side::Buy),
                    shadow_.LevelsAscending(mex::Side::Buy)) ||
        !SameLadder(LevelsAscending(book_, mex::Side::Sell),
                    shadow_.LevelsAscending(mex::Side::Sell)))
    {
      ADD_FAILURE() << "step " << step_;
      return false;
    }
    if (book_.FreeSlotCount() != shadow_.FreeSlots())
    {
      ADD_FAILURE() << "step " << step_ << " free slots " << book_.FreeSlotCount() << " vs "
                    << shadow_.FreeSlots();
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
    ASSERT_TRUE(session.Limit(mex::Side::Buy, price, 5));
    ASSERT_TRUE(session.Limit(mex::Side::Buy, price, 3));
    ASSERT_TRUE(session.Limit(mex::Side::Buy, price, 1));
  }
  for (std::int64_t price = 101; price <= 110; ++price)
  {
    ASSERT_TRUE(session.Limit(mex::Side::Sell, price, 4));
    ASSERT_TRUE(session.Limit(mex::Side::Sell, price, 6));
  }

  ASSERT_TRUE(session.CancelAt(1));
  ASSERT_TRUE(session.CancelAt(session.LiveCount() / 2));
  ASSERT_TRUE(session.CancelAt(session.LiveCount() - 1));

  ASSERT_TRUE(session.Limit(mex::Side::Buy, 105, 30));
  ASSERT_TRUE(session.Market(mex::Side::Sell, 1000));
  ASSERT_TRUE(session.Market(mex::Side::Buy, 1000));
  ASSERT_TRUE(session.Limit(mex::Side::Buy, 100, 0));
  ASSERT_TRUE(session.Market(mex::Side::Sell, 0));
  ASSERT_TRUE(session.CancelAll());
}

TEST(ExchangeSession, DropsRemainderWhenTheNewLevelCannotFit)
{
  Session session(8, 1);
  ASSERT_TRUE(session.Limit(mex::Side::Buy, 90, 5));
  ASSERT_TRUE(session.Limit(mex::Side::Sell, 100, 5));
  ASSERT_TRUE(session.Limit(mex::Side::Buy, 100, 12));
  ASSERT_TRUE(session.Limit(mex::Side::Sell, 100, 4));
  ASSERT_TRUE(session.CancelAll());
}

TEST(ExchangeSession, BoundaryQuantitiesExtremePricesAndFullBook)
{
  Session session(4, 2);
  constexpr std::int64_t kHigh = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kLow = std::numeric_limits<std::int64_t>::min();

  ASSERT_TRUE(session.Limit(mex::Side::Buy, 100, 0));
  ASSERT_TRUE(session.Market(mex::Side::Sell, 0));
  ASSERT_TRUE(session.Limit(mex::Side::Buy, kLow, 1));
  ASSERT_TRUE(session.Limit(mex::Side::Sell, kHigh, 1));
  ASSERT_TRUE(session.Limit(mex::Side::Buy, 0, std::numeric_limits<std::uint32_t>::max()));
  ASSERT_TRUE(session.CancelAll());

  Session full(2, 2);
  ASSERT_TRUE(full.Limit(mex::Side::Buy, 10, 1));
  ASSERT_TRUE(full.Limit(mex::Side::Buy, 11, 1));
  ASSERT_TRUE(full.Limit(mex::Side::Buy, 9, 1));
  ASSERT_TRUE(full.Limit(mex::Side::Sell, 20, 1));
  ASSERT_TRUE(full.Limit(mex::Side::Sell, 21, 1));
  ASSERT_TRUE(full.CancelAll());
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

  for (const auto &config : configs)
  {
    SCOPED_TRACE(config.seed);
    Session session(config.max_orders, config.max_levels);
    std::mt19937 rng(config.seed);
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<int> price_dist(0, config.price_span);
    std::uniform_int_distribution<int> qty_dist(1, 9);
    std::uniform_int_distribution<int> qty_kind(0, 19);
    std::uniform_int_distribution<int> price_kind(0, 19);
    std::uniform_int_distribution<int> side_bit(0, 1);

    auto choose_price = [&]() -> std::int64_t
    {
      const int kind = price_kind(rng);
      if (kind == 0)
      {
        return std::numeric_limits<std::int64_t>::min();
      }
      if (kind == 1)
      {
        return std::numeric_limits<std::int64_t>::max();
      }
      return price_dist(rng);
    };

    auto choose_qty = [&]() -> std::uint32_t
    {
      const int kind = qty_kind(rng);
      if (kind == 0)
      {
        return 0;
      }
      if (kind == 1)
      {
        return 1U << 16U;
      }
      return static_cast<std::uint32_t>(qty_dist(rng));
    };

    for (int step = 0; step < config.steps; ++step)
    {
      session.SetStep(step);
      const int roll = action(rng);
      const mex::Side side = side_bit(rng) == 0 ? mex::Side::Buy : mex::Side::Sell;
      if (roll < 60)
      {
        ASSERT_TRUE(session.Limit(side, choose_price(), choose_qty()));
      }
      else if (roll < 80)
      {
        ASSERT_TRUE(session.Market(side, choose_qty()));
      }
      else if (session.LiveCount() > 0)
      {
        std::uniform_int_distribution<std::size_t> pick(0, session.LiveCount() - 1);
        ASSERT_TRUE(session.CancelAt(pick(rng)));
      }
      else
      {
        ASSERT_TRUE(session.Limit(side, choose_price(), choose_qty()));
      }
    }
    session.SetStep(config.steps);
    ASSERT_TRUE(session.CancelAll());
  }
}

TEST(ExchangeSession, FiveHundredSeededSubmitCancelSequencesMatch)
{
  constexpr int kSequences = 512;
  constexpr int kSteps = 64;

  for (int seq = 0; seq < kSequences; ++seq)
  {
    const std::uint32_t seed = 2000u + static_cast<std::uint32_t>(seq);
    SCOPED_TRACE(seed);
    Session session(24, 6);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<int> price_dist(0, 8);
    std::uniform_int_distribution<int> qty_dist(1, 6);
    std::uniform_int_distribution<int> side_bit(0, 1);

    for (int step = 0; step < kSteps; ++step)
    {
      session.SetStep(step);
      const mex::Side side = side_bit(rng) == 0 ? mex::Side::Buy : mex::Side::Sell;
      if (action(rng) < 70 || session.LiveCount() == 0)
      {
        ASSERT_TRUE(
            session.Limit(side, price_dist(rng), static_cast<std::uint32_t>(qty_dist(rng))));
      }
      else
      {
        std::uniform_int_distribution<std::size_t> pick(0, session.LiveCount() - 1);
        ASSERT_TRUE(session.CancelAt(pick(rng)));
      }
    }
    session.SetStep(kSteps);
    ASSERT_TRUE(session.CancelAll());
  }
}

} // namespace
