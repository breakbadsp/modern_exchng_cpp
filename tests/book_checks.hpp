#pragma once

#include "book.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

namespace book_test
{

struct Fill
{
  mex::OrderId maker_id = mex::kInvalidOrderId;
  std::int64_t price = 0;
  std::uint32_t quantity = 0;
};

class FillLog
{
public:
  [[nodiscard]] auto Callback()
  {
    return [this](mex::OrderId p_maker_id, std::int64_t p_price, std::uint32_t p_quantity)
    { records_.push_back(Fill{p_maker_id, p_price, p_quantity}); };
  }

  [[nodiscard]] const std::vector<Fill> &Records() const { return records_; }

  void Clear() { records_.clear(); }

private:
  std::vector<Fill> records_;
};

inline void ExpectFill(const Fill &p_fill, mex::OrderId p_maker_id, std::int64_t p_price,
                       std::uint32_t p_quantity)
{
  EXPECT_EQ(p_fill.maker_id, p_maker_id);
  EXPECT_EQ(p_fill.price, p_price);
  EXPECT_EQ(p_fill.quantity, p_quantity);
}

inline void ExpectResting(const mex::SubmitResult &p_result, std::uint32_t p_quantity)
{
  EXPECT_EQ(p_result.status, mex::SubmitStatus::kAccepted);
  EXPECT_EQ(p_result.filled_qty, 0u);
  EXPECT_EQ(p_result.remaining, p_quantity);
  EXPECT_NE(p_result.order_id, mex::kInvalidOrderId);
}

inline void ExpectTrade(const mex::SubmitResult &p_result, std::uint32_t p_filled,
                        std::uint32_t p_remaining, bool p_rests)
{
  EXPECT_EQ(p_result.status, mex::SubmitStatus::kAccepted);
  EXPECT_EQ(p_result.filled_qty, p_filled);
  EXPECT_EQ(p_result.remaining, p_remaining);
  if (p_rests)
  {
    EXPECT_NE(p_result.order_id, mex::kInvalidOrderId);
  }
  else
  {
    EXPECT_EQ(p_result.order_id, mex::kInvalidOrderId);
  }
}

inline void ExpectRejected(const mex::SubmitResult &p_result, std::uint32_t p_remaining)
{
  EXPECT_EQ(p_result.status, mex::SubmitStatus::kRejected);
  EXPECT_EQ(p_result.filled_qty, 0u);
  EXPECT_EQ(p_result.remaining, p_remaining);
  EXPECT_EQ(p_result.order_id, mex::kInvalidOrderId);
}

inline mex::SubmitResult SubmitLimit(mex::Book &p_book, FillLog &p_log, mex::Side p_side,
                                     std::int64_t p_price, std::uint32_t p_quantity)
{
  return p_book.SubmitLimitOrder(p_side, p_price, p_quantity, p_log.Callback());
}

inline mex::SubmitResult SubmitMarket(mex::Book &p_book, FillLog &p_log, mex::Side p_side,
                                      std::uint32_t p_quantity)
{
  return p_book.SubmitMarketOrder(p_side, p_quantity, p_log.Callback());
}

inline void ExpectLevelList(const mex::Book &p_book, const mex::PriceLevel &p_level,
                            mex::Side p_side)
{
  EXPECT_GT(p_level.order_count, 0u);
  EXPECT_GT(p_level.total_qty, 0u);

  mex::OrderId id = p_level.head;
  mex::OrderId prev = mex::kInvalidOrderId;
  std::uint32_t count = 0;
  std::uint64_t sum = 0;
  while (id != mex::kInvalidOrderId)
  {
    if (count >= p_book.MaxOrders())
    {
      ADD_FAILURE() << "order list is longer than the pool";
      break;
    }

    const mex::OrderNode &node = p_book.Order(id);
    EXPECT_EQ(node.prev, prev);
    EXPECT_EQ(node.next == mex::kInvalidOrderId || node.next != id, true);
    EXPECT_GT(node.quantity, 0u);
    EXPECT_EQ(node.price, p_level.price);
    EXPECT_EQ(node.side, p_side);
    sum += node.quantity;
    ++count;
    prev = id;
    id = node.next;
  }

  EXPECT_EQ(count, p_level.order_count);
  EXPECT_EQ(sum, p_level.total_qty);
  EXPECT_EQ(prev, p_level.tail);
}

inline void ExpectBookInvariants(const mex::Book &p_book)
{
  if (!p_book.Bids().empty() && !p_book.Asks().empty())
  {
    EXPECT_LT(p_book.Bids().back().price, p_book.Asks().back().price);
  }

  std::uint32_t live_orders = 0;
  for (const mex::PriceLevel &level : p_book.Bids())
  {
    ExpectLevelList(p_book, level, mex::Side::kBuy);
    live_orders += level.order_count;
  }
  for (const mex::PriceLevel &level : p_book.Asks())
  {
    ExpectLevelList(p_book, level, mex::Side::kSell);
    live_orders += level.order_count;
  }

  for (std::size_t i = 1; i < p_book.Bids().size(); ++i)
  {
    EXPECT_LT(p_book.Bids()[i - 1].price, p_book.Bids()[i].price);
  }
  for (std::size_t i = 1; i < p_book.Asks().size(); ++i)
  {
    EXPECT_GT(p_book.Asks()[i - 1].price, p_book.Asks()[i].price);
  }

  EXPECT_EQ(live_orders + p_book.FreeSlotCount(), p_book.MaxOrders());
}

} // namespace book_test
