#pragma once

#include "book.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <span>
#include <vector>

namespace book_test
{

class FillLog
{
public:
  void Capture(std::span<const mex::Fill> p_fills)
  {
    records_.insert(records_.end(), p_fills.begin(), p_fills.end());
  }

  [[nodiscard]] const std::vector<mex::Fill> &Records() const { return records_; }

  void Clear() { records_.clear(); }

private:
  std::vector<mex::Fill> records_;
};

inline void ExpectFill(mex::Fill p_fill, mex::OrderId p_maker_id, std::int64_t p_price,
                       std::uint32_t p_quantity)
{
  EXPECT_EQ(p_fill.maker_id_, p_maker_id);
  EXPECT_EQ(p_fill.price_, mex::Price{p_price});
  EXPECT_EQ(p_fill.quantity_, mex::Quantity{p_quantity});
}

inline void ExpectResting(mex::SubmitResult p_result, std::uint32_t p_quantity)
{
  EXPECT_EQ(p_result.status_, mex::SubmitStatus::Accepted);
  EXPECT_EQ(p_result.filled_qty_, mex::Quantity{});
  EXPECT_EQ(p_result.remaining_, mex::Quantity{p_quantity});
  EXPECT_NE(p_result.order_id_, mex::kInvalidOrderId);
}

inline void ExpectTrade(mex::SubmitResult p_result, std::uint32_t p_filled,
                        std::uint32_t p_remaining, bool p_rests)
{
  EXPECT_EQ(p_result.status_, mex::SubmitStatus::Accepted);
  EXPECT_EQ(p_result.filled_qty_, mex::Quantity{p_filled});
  EXPECT_EQ(p_result.remaining_, mex::Quantity{p_remaining});
  if (p_rests)
  {
    EXPECT_NE(p_result.order_id_, mex::kInvalidOrderId);
  }
  else
  {
    EXPECT_EQ(p_result.order_id_, mex::kInvalidOrderId);
  }
}

inline void ExpectRejected(mex::SubmitResult p_result, std::uint32_t p_remaining)
{
  EXPECT_EQ(p_result.status_, mex::SubmitStatus::Rejected);
  EXPECT_EQ(p_result.filled_qty_, mex::Quantity{});
  EXPECT_EQ(p_result.remaining_, mex::Quantity{p_remaining});
  EXPECT_EQ(p_result.order_id_, mex::kInvalidOrderId);
}

inline mex::SubmitResult SubmitLimit(mex::Book &p_book, FillLog &p_log, mex::Side p_side,
                                     std::int64_t p_price, std::uint32_t p_quantity)
{
  const mex::SubmitResult result =
      p_book.SubmitLimitOrder(p_side, mex::Price{p_price}, mex::Quantity{p_quantity});
  p_log.Capture(p_book.Fills());
  return result;
}

inline mex::SubmitResult SubmitMarket(mex::Book &p_book, FillLog &p_log, mex::Side p_side,
                                      std::uint32_t p_quantity)
{
  const mex::SubmitResult result = p_book.SubmitMarketOrder(p_side, mex::Quantity{p_quantity});
  p_log.Capture(p_book.Fills());
  return result;
}

inline void ExpectLevelList(const mex::Book &p_book, mex::PriceLevel p_level, mex::Side p_side)
{
  EXPECT_GT(p_level.order_count_, 0u);
  EXPECT_GT(p_level.total_qty_, 0u);

  mex::OrderId id = p_level.head_;
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
    EXPECT_EQ(node.prev_, prev);
    EXPECT_EQ(node.next_ == mex::kInvalidOrderId || node.next_ != id, true);
    EXPECT_GT(node.quantity_, mex::Quantity{});
    EXPECT_EQ(node.price_, p_level.price_);
    EXPECT_EQ(node.side_, p_side);
    sum += node.quantity_.units_;
    ++count;
    prev = id;
    id = node.next_;
  }

  EXPECT_EQ(count, p_level.order_count_);
  EXPECT_EQ(sum, p_level.total_qty_);
  EXPECT_EQ(prev, p_level.tail_);
}

inline void ExpectBookInvariants(const mex::Book &p_book)
{
  if (!p_book.Bids().empty() && !p_book.Asks().empty())
  {
    EXPECT_LT(p_book.Bids().back().price_, p_book.Asks().back().price_);
  }

  std::uint32_t live_orders = 0;
  for (const mex::PriceLevel &level : p_book.Bids())
  {
    ExpectLevelList(p_book, level, mex::Side::Buy);
    live_orders += level.order_count_;
  }
  for (const mex::PriceLevel &level : p_book.Asks())
  {
    ExpectLevelList(p_book, level, mex::Side::Sell);
    live_orders += level.order_count_;
  }

  for (std::size_t i = 1; i < p_book.Bids().size(); ++i)
  {
    EXPECT_LT(p_book.Bids()[i - 1].price_, p_book.Bids()[i].price_);
  }
  for (std::size_t i = 1; i < p_book.Asks().size(); ++i)
  {
    EXPECT_GT(p_book.Asks()[i - 1].price_, p_book.Asks()[i].price_);
  }

  EXPECT_EQ(live_orders + p_book.FreeSlotCount(), p_book.MaxOrders());
}

} // namespace book_test
