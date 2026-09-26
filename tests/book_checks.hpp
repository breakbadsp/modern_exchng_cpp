#pragma once

#include "book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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
        return [this](mex::OrderId maker_id, std::int64_t price, std::uint32_t quantity)
        {
            records_.push_back(Fill{maker_id, price, quantity});
        };
    }

    [[nodiscard]] const std::vector<Fill>& Records() const
    {
        return records_;
    }

    void Clear()
    {
        records_.clear();
    }

private:
    std::vector<Fill> records_;
};

inline void ExpectFill(const Fill& fill,
                       mex::OrderId maker_id,
                       std::int64_t price,
                       std::uint32_t quantity)
{
    EXPECT_EQ(fill.maker_id, maker_id);
    EXPECT_EQ(fill.price, price);
    EXPECT_EQ(fill.quantity, quantity);
}

inline void ExpectResting(const mex::SubmitResult& result, std::uint32_t quantity)
{
    EXPECT_EQ(result.status, mex::SubmitStatus::kAccepted);
    EXPECT_EQ(result.filled_qty, 0u);
    EXPECT_EQ(result.remaining, quantity);
    EXPECT_NE(result.order_id, mex::kInvalidOrderId);
}

inline void ExpectTrade(const mex::SubmitResult& result,
                        std::uint32_t filled,
                        std::uint32_t remaining,
                        bool rests)
{
    EXPECT_EQ(result.status, mex::SubmitStatus::kAccepted);
    EXPECT_EQ(result.filled_qty, filled);
    EXPECT_EQ(result.remaining, remaining);
    if (rests)
    {
        EXPECT_NE(result.order_id, mex::kInvalidOrderId);
    }
    else
    {
        EXPECT_EQ(result.order_id, mex::kInvalidOrderId);
    }
}

inline void ExpectRejected(const mex::SubmitResult& result, std::uint32_t remaining)
{
    EXPECT_EQ(result.status, mex::SubmitStatus::kRejected);
    EXPECT_EQ(result.filled_qty, 0u);
    EXPECT_EQ(result.remaining, remaining);
    EXPECT_EQ(result.order_id, mex::kInvalidOrderId);
}

inline mex::SubmitResult SubmitLimit(mex::Book& book,
                                     FillLog& log,
                                     mex::Side side,
                                     std::int64_t price,
                                     std::uint32_t quantity)
{
    return book.SubmitLimitOrder(side, price, quantity, log.Callback());
}

inline mex::SubmitResult SubmitMarket(mex::Book& book,
                                      FillLog& log,
                                      mex::Side side,
                                      std::uint32_t quantity)
{
    return book.SubmitMarketOrder(side, quantity, log.Callback());
}

inline void ExpectLevelList(const mex::Book& book, const mex::PriceLevel& level, mex::Side side)
{
    EXPECT_GT(level.order_count, 0u);
    EXPECT_GT(level.total_qty, 0u);

    mex::OrderId id = level.head;
    mex::OrderId prev = mex::kInvalidOrderId;
    std::uint32_t count = 0;
    std::uint64_t sum = 0;
    while (id != mex::kInvalidOrderId)
    {
        if (count >= book.MaxOrders())
        {
            ADD_FAILURE() << "order list is longer than the pool";
            break;
        }

        const mex::OrderNode& node = book.Order(id);
        EXPECT_EQ(node.prev, prev);
        EXPECT_EQ(node.next == mex::kInvalidOrderId || node.next != id, true);
        EXPECT_GT(node.quantity, 0u);
        EXPECT_EQ(node.price, level.price);
        EXPECT_EQ(node.side, side);
        sum += node.quantity;
        ++count;
        prev = id;
        id = node.next;
    }

    EXPECT_EQ(count, level.order_count);
    EXPECT_EQ(sum, level.total_qty);
    EXPECT_EQ(prev, level.tail);
}

inline void ExpectBookInvariants(const mex::Book& book)
{
    if (!book.Bids().empty() && !book.Asks().empty())
    {
        EXPECT_LT(book.Bids().back().price, book.Asks().back().price);
    }

    std::uint32_t live_orders = 0;
    for (const mex::PriceLevel& level : book.Bids())
    {
        ExpectLevelList(book, level, mex::Side::kBuy);
        live_orders += level.order_count;
    }
    for (const mex::PriceLevel& level : book.Asks())
    {
        ExpectLevelList(book, level, mex::Side::kSell);
        live_orders += level.order_count;
    }

    for (std::size_t i = 1; i < book.Bids().size(); ++i)
    {
        EXPECT_LT(book.Bids()[i - 1].price, book.Bids()[i].price);
    }
    for (std::size_t i = 1; i < book.Asks().size(); ++i)
    {
        EXPECT_GT(book.Asks()[i - 1].price, book.Asks()[i].price);
    }

    EXPECT_EQ(live_orders + book.FreeSlotCount(), book.MaxOrders());
}

}  // namespace book_test
