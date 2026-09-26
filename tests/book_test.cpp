#include "book_checks.hpp"

#include <cstdint>
#include <limits>

namespace
{

using book_test::ExpectBookInvariants;
using book_test::ExpectFill;
using book_test::ExpectRejected;
using book_test::ExpectResting;
using book_test::ExpectTrade;
using book_test::FillLog;
using book_test::SubmitLimit;
using book_test::SubmitMarket;

TEST(Resting, StartsEmptyWithEverySlotFree)
{
    mex::Book book(3, 2);
    FillLog log;

    EXPECT_EQ(book.MaxOrders(), 3u);
    EXPECT_EQ(book.FreeSlotCount(), 3u);
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_TRUE(book.Asks().empty());
    ExpectBookInvariants(book);

    ExpectRejected(SubmitLimit(book, log, mex::Side::kBuy, 100, 0), 0);
    ExpectRejected(SubmitMarket(book, log, mex::Side::kSell, 0), 0);
    EXPECT_TRUE(log.Records().empty());
    EXPECT_EQ(book.FreeSlotCount(), 3u);
    ExpectBookInvariants(book);
}

TEST(Resting, DoesNotTradeInsideTheSpread)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    ExpectResting(ask, 10);
    EXPECT_TRUE(log.Records().empty());
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Asks().size(), std::size_t{1});
    EXPECT_EQ(book.Asks().back().price, 100);
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_EQ(book.Order(ask.order_id).side, mex::Side::kSell);
    EXPECT_EQ(book.Order(ask.order_id).quantity, 10u);

    const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::kBuy, 99, 5);
    ExpectResting(bid, 5);
    EXPECT_TRUE(log.Records().empty());
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().price, 99);
    EXPECT_EQ(book.Asks().back().price, 100);
    EXPECT_EQ(book.FreeSlotCount(), 2u);
}

TEST(Resting, InsertsPricesInBookOrderOnBothSides)
{
    mex::Book book(8, 8);
    FillLog log;

    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 100, 1), 1);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 1), 1);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 95, 1), 1);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Bids().size(), std::size_t{3});
    EXPECT_EQ(book.Bids()[0].price, 90);
    EXPECT_EQ(book.Bids()[1].price, 95);
    EXPECT_EQ(book.Bids()[2].price, 100);

    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 110, 1), 1);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 130, 1), 1);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 120, 1), 1);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Asks().size(), std::size_t{3});
    EXPECT_EQ(book.Asks()[0].price, 130);
    EXPECT_EQ(book.Asks()[1].price, 120);
    EXPECT_EQ(book.Asks()[2].price, 110);
    EXPECT_TRUE(log.Records().empty());
}

TEST(Resting, AppendsSamePriceInArrivalOrder)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 100, 4);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kSell, 100, 6);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Asks().size(), std::size_t{1});
    EXPECT_EQ(book.Asks().back().order_count, 2u);
    EXPECT_EQ(book.Asks().back().total_qty, 10u);
    EXPECT_EQ(book.Asks().back().head, first.order_id);
    EXPECT_EQ(book.Asks().back().tail, second.order_id);
    EXPECT_EQ(book.Order(first.order_id).next, second.order_id);
    EXPECT_EQ(book.Order(second.order_id).prev, first.order_id);
}

TEST(Resting, AllowsZeroAndNegativePrices)
{
    mex::Book book(4, 4);
    FillLog log;

    const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::kBuy, -5, 2);
    const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::kSell, -1, 2);
    ExpectResting(bid, 2);
    ExpectResting(ask, 2);
    EXPECT_TRUE(log.Records().empty());
    ExpectBookInvariants(book);

    const mex::SubmitResult sell = SubmitLimit(book, log, mex::Side::kSell, -5, 2);
    ExpectTrade(sell, 2, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], bid.order_id, -5, 2);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Bids().empty());

    log.Clear();
    const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::kBuy, -1, 2);
    ExpectTrade(buy, 2, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], ask.order_id, -1, 2);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
}

TEST(Matching, PartiallyFillsOneMakerAndRestsTheRest)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 15);
    ExpectTrade(taker, 10, 5, true);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], maker.order_id, 100, 10);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    ASSERT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().price, 100);
    EXPECT_EQ(book.Bids().back().total_qty, 5u);
    EXPECT_EQ(book.Bids().back().head, taker.order_id);
}

TEST(Matching, ExactFillRemovesBothOrders)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    ExpectTrade(taker, 10, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], maker.order_id, 100, 10);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());
    EXPECT_FALSE(book.CancelOrder(maker.order_id));
}

TEST(Matching, LeavesAPartiallyFilledMakerAtTheHead)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 50, 5);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kSell, 50, 7);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 50, 3);
    ExpectTrade(taker, 3, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], first.order_id, 50, 3);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Asks().back().head, first.order_id);
    EXPECT_EQ(book.Asks().back().tail, second.order_id);
    EXPECT_EQ(book.Order(first.order_id).quantity, 2u);
    EXPECT_EQ(book.Order(second.order_id).quantity, 7u);
    EXPECT_EQ(book.Asks().back().total_qty, 9u);
}

TEST(Matching, WalksAskLevelsAndStopsAtTheLimit)
{
    mex::Book book(8, 8);
    FillLog log;

    const mex::SubmitResult ask98 = SubmitLimit(book, log, mex::Side::kSell, 98, 30);
    const mex::SubmitResult ask99 = SubmitLimit(book, log, mex::Side::kSell, 99, 40);
    const mex::SubmitResult ask101 = SubmitLimit(book, log, mex::Side::kSell, 101, 50);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 100);
    ExpectTrade(taker, 70, 30, true);
    ASSERT_EQ(log.Records().size(), std::size_t{2});
    ExpectFill(log.Records()[0], ask98.order_id, 98, 30);
    ExpectFill(log.Records()[1], ask99.order_id, 99, 40);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Asks().size(), std::size_t{1});
    EXPECT_EQ(book.Asks().back().price, 101);
    EXPECT_EQ(book.Asks().back().head, ask101.order_id);
    EXPECT_EQ(book.Bids().back().price, 100);
    EXPECT_EQ(book.Bids().back().total_qty, 30u);
}

TEST(Matching, SellsIntoBidsFromBestPriceDown)
{
    mex::Book book(8, 8);
    FillLog log;

    const mex::SubmitResult bid100 = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    const mex::SubmitResult bid99 = SubmitLimit(book, log, mex::Side::kBuy, 99, 10);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kSell, 99, 15);
    ExpectTrade(taker, 15, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{2});
    ExpectFill(log.Records()[0], bid100.order_id, 100, 10);
    ExpectFill(log.Records()[1], bid99.order_id, 99, 5);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().price, 99);
    EXPECT_EQ(book.Bids().back().total_qty, 5u);
    EXPECT_EQ(book.Order(bid99.order_id).quantity, 5u);
}

TEST(Matching, FillsSamePriceInFifoOrder)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    ExpectTrade(taker, 10, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], first.order_id, 100, 10);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().order_count, 1u);
    EXPECT_EQ(book.Bids().back().head, second.order_id);
}

TEST(Matching, ExhaustsOneLevelBeforeTheNextOrderAtThatPrice)
{
    mex::Book book(8, 4);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 100, 4);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kSell, 100, 6);
    const mex::SubmitResult worse = SubmitLimit(book, log, mex::Side::kSell, 101, 9);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 7);
    ExpectTrade(taker, 7, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{2});
    ExpectFill(log.Records()[0], first.order_id, 100, 4);
    ExpectFill(log.Records()[1], second.order_id, 100, 3);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Asks().size(), std::size_t{2});
    EXPECT_EQ(book.Asks().back().price, 100);
    EXPECT_EQ(book.Asks().back().head, second.order_id);
    EXPECT_EQ(book.Order(second.order_id).quantity, 3u);
    EXPECT_EQ(book.Order(worse.order_id).quantity, 9u);
}

TEST(Matching, RestsWhatIsLeftAfterTheOppositeSideIsGone)
{
    mex::Book book(8, 8);
    FillLog log;

    (void)SubmitLimit(book, log, mex::Side::kSell, 98, 10);
    (void)SubmitLimit(book, log, mex::Side::kSell, 99, 10);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 30);
    ExpectTrade(taker, 20, 10, true);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_EQ(book.Bids().back().price, 100);
    EXPECT_EQ(book.Bids().back().total_qty, 10u);
}

TEST(Matching, TradesExtremePricesAtTheMakerPrice)
{
    mex::Book book(4, 4);
    FillLog log;

    constexpr std::int64_t kHigh = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kLow = std::numeric_limits<std::int64_t>::min();

    const mex::SubmitResult best_bid = SubmitLimit(book, log, mex::Side::kBuy, kHigh, 1);
    const mex::SubmitResult worse_bid = SubmitLimit(book, log, mex::Side::kBuy, kHigh - 1, 1);
    const mex::SubmitResult sell = SubmitLimit(book, log, mex::Side::kSell, kHigh - 1, 1);
    ExpectTrade(sell, 1, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], best_bid.order_id, kHigh, 1);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().head, worse_bid.order_id);
    EXPECT_TRUE(book.CancelOrder(worse_bid.order_id));

    log.Clear();
    const mex::SubmitResult best_ask = SubmitLimit(book, log, mex::Side::kSell, kLow, 1);
    const mex::SubmitResult worse_ask = SubmitLimit(book, log, mex::Side::kSell, kLow + 1, 1);
    const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::kBuy, kLow + 1, 2);
    ExpectTrade(buy, 2, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{2});
    ExpectFill(log.Records()[0], best_ask.order_id, kLow, 1);
    ExpectFill(log.Records()[1], worse_ask.order_id, kLow + 1, 1);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_TRUE(book.Asks().empty());
}

TEST(Market, RejectsWhenTheOppositeSideIsEmpty)
{
    mex::Book book(4, 2);
    FillLog log;

    ExpectRejected(SubmitMarket(book, log, mex::Side::kBuy, 10), 0);
    ExpectRejected(SubmitMarket(book, log, mex::Side::kSell, 1), 0);
    EXPECT_TRUE(log.Records().empty());
    ExpectBookInvariants(book);

    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 40, 5), 5);
    const mex::SubmitResult buy = SubmitMarket(book, log, mex::Side::kBuy, 10);
    ExpectRejected(buy, 0);
    EXPECT_TRUE(log.Records().empty());
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().total_qty, 5u);

    const mex::SubmitResult sell = SubmitMarket(book, log, mex::Side::kSell, 1);
    ExpectTrade(sell, 1, 0, false);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().total_qty, 4u);
}

TEST(Market, FillsWhatIsThereAndCancelsTheRest)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    const mex::SubmitResult taker = SubmitMarket(book, log, mex::Side::kBuy, 20);
    ExpectTrade(taker, 10, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], maker.order_id, 100, 10);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());
}

TEST(Market, SweepsSeveralLevels)
{
    mex::Book book(8, 8);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 10, 2);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kSell, 11, 3);
    const mex::SubmitResult taker = SubmitMarket(book, log, mex::Side::kBuy, 100);
    ExpectTrade(taker, 5, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{2});
    ExpectFill(log.Records()[0], first.order_id, 10, 2);
    ExpectFill(log.Records()[1], second.order_id, 11, 3);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_TRUE(book.Bids().empty());
}

TEST(Cancel, RemovesARestingOrderAndRejectsASecondCancel)
{
    mex::Book book(4, 2);
    FillLog log;

    const mex::SubmitResult resting = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    EXPECT_TRUE(book.CancelOrder(resting.order_id));
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());

    EXPECT_FALSE(book.CancelOrder(resting.order_id));
    EXPECT_FALSE(book.CancelOrder(mex::kInvalidOrderId));
    EXPECT_FALSE(book.CancelOrder(book.MaxOrders()));
    EXPECT_FALSE(book.CancelOrder(0));
    ExpectBookInvariants(book);
}

TEST(Cancel, DropsTheMiddleOrderAndKeepsFifo)
{
    mex::Book book(8, 4);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    const mex::SubmitResult middle = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    const mex::SubmitResult last = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    EXPECT_TRUE(book.CancelOrder(middle.order_id));
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Asks().back().head, first.order_id);
    EXPECT_EQ(book.Asks().back().tail, last.order_id);
    EXPECT_EQ(book.Order(first.order_id).next, last.order_id);
    EXPECT_EQ(book.Order(last.order_id).prev, first.order_id);

    const mex::SubmitResult first_take = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    ExpectTrade(first_take, 10, 0, false);
    ExpectFill(log.Records()[0], first.order_id, 100, 10);

    const mex::SubmitResult second_take = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    ExpectTrade(second_take, 10, 0, false);
    ExpectFill(log.Records()[1], last.order_id, 100, 10);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_FALSE(book.CancelOrder(middle.order_id));
}

TEST(Cancel, RemovesHeadOrTailWithoutDisturbingTheOtherOrders)
{
    mex::Book book(8, 2);
    FillLog log;

    const mex::SubmitResult head = SubmitLimit(book, log, mex::Side::kBuy, 7, 1);
    const mex::SubmitResult mid = SubmitLimit(book, log, mex::Side::kBuy, 7, 1);
    const mex::SubmitResult tail = SubmitLimit(book, log, mex::Side::kBuy, 7, 1);

    EXPECT_TRUE(book.CancelOrder(tail.order_id));
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().head, head.order_id);
    EXPECT_EQ(book.Bids().back().tail, mid.order_id);
    EXPECT_EQ(book.Order(head.order_id).next, mid.order_id);
    EXPECT_EQ(book.Order(mid.order_id).next, mex::kInvalidOrderId);

    EXPECT_TRUE(book.CancelOrder(head.order_id));
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().back().head, mid.order_id);
    EXPECT_EQ(book.Bids().back().tail, mid.order_id);
    EXPECT_EQ(book.Order(mid.order_id).prev, mex::kInvalidOrderId);
    EXPECT_EQ(book.Order(mid.order_id).quantity, 1u);
}

TEST(Cancel, FindsALevelAfterLaterInsertsMoveIt)
{
    mex::Book book(8, 4);
    FillLog log;

    const mex::SubmitResult deep_bid = SubmitLimit(book, log, mex::Side::kBuy, 100, 7);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 4), 4);
    EXPECT_EQ(book.Bids().front().price, 90);
    EXPECT_EQ(book.Bids().back().price, 100);
    EXPECT_TRUE(book.CancelOrder(deep_bid.order_id));
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().price, 90);
    EXPECT_EQ(book.Bids().back().total_qty, 4u);

    const mex::SubmitResult deep_ask = SubmitLimit(book, log, mex::Side::kSell, 110, 3);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 120, 3), 3);
    EXPECT_EQ(book.Asks().back().price, 110);
    EXPECT_TRUE(book.CancelOrder(deep_ask.order_id));
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Asks().size(), std::size_t{1});
    EXPECT_EQ(book.Asks().back().price, 120);
}

TEST(Cancel, ReusesTheFreedSlotForTheNextOrder)
{
    mex::Book book(2, 2);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kSell, 100, 8);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 1), 1);
    EXPECT_TRUE(book.CancelOrder(first.order_id));
    ExpectBookInvariants(book);

    const mex::SubmitResult reused = SubmitLimit(book, log, mex::Side::kSell, 105, 2);
    ExpectResting(reused, 2);
    EXPECT_EQ(reused.order_id, first.order_id);
    EXPECT_EQ(book.Order(reused.order_id).price, 105);
    EXPECT_EQ(book.Order(reused.order_id).quantity, 2u);
    EXPECT_EQ(book.Asks().back().head, reused.order_id);
    ExpectBookInvariants(book);

    EXPECT_TRUE(book.CancelOrder(reused.order_id));
    EXPECT_FALSE(book.CancelOrder(reused.order_id));
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
}

TEST(Capacity, FullyFilledTakerDoesNotNeedAFreeSlot)
{
    mex::Book book(2, 2);
    FillLog log;

    const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::kSell, 100, 10);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 102, 10), 10);
    EXPECT_EQ(book.FreeSlotCount(), 0u);

    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 101, 5);
    ExpectTrade(taker, 5, 0, false);
    EXPECT_EQ(book.FreeSlotCount(), 0u);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Order(maker.order_id).quantity, 5u);
    EXPECT_EQ(book.Asks().back().price, 100);
}

TEST(Capacity, RejectsARestThatDoesNotTradeWhenThePoolIsFull)
{
    mex::Book book(2, 2);
    FillLog log;

    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 100, 10), 10);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 5), 5);
    EXPECT_EQ(book.FreeSlotCount(), 0u);
    const mex::OrderId bid_head = book.Bids().back().head;
    const std::uint64_t bid_qty = book.Bids().back().total_qty;
    const mex::OrderId ask_head = book.Asks().back().head;
    const std::uint64_t ask_qty = book.Asks().back().total_qty;

    const mex::SubmitResult rejected = SubmitLimit(book, log, mex::Side::kBuy, 85, 3);
    ExpectRejected(rejected, 3);
    EXPECT_TRUE(log.Records().empty());
    EXPECT_EQ(book.FreeSlotCount(), 0u);
    EXPECT_EQ(book.Bids().back().head, bid_head);
    EXPECT_EQ(book.Bids().back().total_qty, bid_qty);
    EXPECT_EQ(book.Asks().back().head, ask_head);
    EXPECT_EQ(book.Asks().back().total_qty, ask_qty);
    ExpectBookInvariants(book);
}

TEST(Capacity, RestsAfterAFillFreesASlot)
{
    mex::Book book(2, 2);
    FillLog log;

    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 100, 10), 10);
    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 5), 5);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 15);
    ExpectTrade(taker, 10, 5, true);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    EXPECT_EQ(book.Bids().size(), std::size_t{2});
    EXPECT_EQ(book.FreeSlotCount(), 0u);
}

TEST(Capacity, RejectsANewLevelAtTheCapAndStillMatches)
{
    mex::Book book(4, 2);
    FillLog log;

    ExpectResting(SubmitLimit(book, log, mex::Side::kBuy, 90, 5), 5);
    const mex::SubmitResult best_bid = SubmitLimit(book, log, mex::Side::kBuy, 95, 5);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 100, 5), 5);
    ExpectBookInvariants(book);

    const mex::SubmitResult rejected = SubmitLimit(book, log, mex::Side::kBuy, 88, 3);
    ExpectRejected(rejected, 3);
    EXPECT_TRUE(log.Records().empty());
    EXPECT_EQ(book.Bids().size(), std::size_t{2});

    const mex::SubmitResult accepted = SubmitLimit(book, log, mex::Side::kSell, 95, 5);
    ExpectTrade(accepted, 5, 0, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], best_bid.order_id, 95, 5);
    ExpectBookInvariants(book);
    ASSERT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().price, 90);
}

TEST(Capacity, JoinsAnExistingLevelWhenNoNewLevelFits)
{
    mex::Book book(4, 1);
    FillLog log;

    const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::kBuy, 10, 1);
    const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::kBuy, 10, 2);
    ExpectResting(first, 1);
    ExpectResting(second, 2);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().order_count, 2u);
    EXPECT_EQ(book.Bids().back().total_qty, 3u);

    ExpectRejected(SubmitLimit(book, log, mex::Side::kBuy, 9, 1), 1);
    EXPECT_EQ(book.Bids().size(), std::size_t{1});

    const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::kSell, 12, 1);
    ExpectResting(ask, 1);
    ExpectRejected(SubmitLimit(book, log, mex::Side::kSell, 13, 1), 1);
    ExpectResting(SubmitLimit(book, log, mex::Side::kSell, 12, 4), 4);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Asks().size(), std::size_t{1});
    EXPECT_EQ(book.Asks().back().total_qty, 5u);
}

TEST(Capacity, DropsTheUnrestedRemainderWhenTheNewLevelDoesNotFit)
{
    mex::Book book(4, 1);
    FillLog log;

    const mex::SubmitResult resting_bid = SubmitLimit(book, log, mex::Side::kBuy, 90, 5);
    const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::kSell, 100, 5);
    const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::kBuy, 100, 10);
    ExpectTrade(taker, 5, 5, false);
    ASSERT_EQ(log.Records().size(), std::size_t{1});
    ExpectFill(log.Records()[0], maker.order_id, 100, 5);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Asks().empty());
    ASSERT_EQ(book.Bids().size(), std::size_t{1});
    EXPECT_EQ(book.Bids().back().price, 90);
    EXPECT_EQ(book.Bids().back().head, resting_bid.order_id);
    EXPECT_EQ(book.Bids().back().total_qty, 5u);
}

TEST(Scenario, MixedOrdersKeepTheBookConsistent)
{
    mex::Book book(8, 4);
    FillLog log;

    const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::kBuy, 10, 5);
    ExpectBookInvariants(book);
    const mex::SubmitResult ask_a = SubmitLimit(book, log, mex::Side::kSell, 12, 4);
    const mex::SubmitResult ask_b = SubmitLimit(book, log, mex::Side::kSell, 12, 4);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.CancelOrder(ask_a.order_id));
    ExpectBookInvariants(book);

    const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::kBuy, 12, 3);
    ExpectTrade(buy, 3, 0, false);
    ExpectBookInvariants(book);
    EXPECT_EQ(book.Order(ask_b.order_id).quantity, 1u);
    EXPECT_EQ(book.Order(bid.order_id).quantity, 5u);

    log.Clear();
    const mex::SubmitResult market = SubmitMarket(book, log, mex::Side::kSell, 9);
    ExpectTrade(market, 5, 0, false);
    ExpectFill(log.Records()[0], bid.order_id, 10, 5);
    ExpectBookInvariants(book);
    EXPECT_TRUE(book.Bids().empty());
    EXPECT_EQ(book.Asks().back().total_qty, 1u);
    EXPECT_FALSE(book.CancelOrder(bid.order_id));
    EXPECT_FALSE(book.CancelOrder(ask_a.order_id));
    ExpectBookInvariants(book);
}

}  // namespace
