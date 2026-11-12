#include "book_checks.hpp"

#include <array>
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

  ExpectRejected(SubmitLimit(book, log, mex::Side::Buy, 100, 0), 0);
  ExpectRejected(SubmitMarket(book, log, mex::Side::Sell, 0), 0);
  EXPECT_TRUE(log.Records().empty());
  EXPECT_EQ(book.FreeSlotCount(), 3u);
  ExpectBookInvariants(book);
}

TEST(Resting, DoesNotTradeInsideTheSpread)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  ExpectResting(ask, 10);
  EXPECT_TRUE(log.Records().empty());
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Asks().size(), std::size_t{1});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{100});
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_EQ(book.Order(ask.order_id_).side_, mex::Side::Sell);
  EXPECT_EQ(book.Order(ask.order_id_).quantity_, mex::Quantity{10});

  const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::Buy, 99, 5);
  ExpectResting(bid, 5);
  EXPECT_TRUE(log.Records().empty());
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().price_, mex::Price{99});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{100});
  EXPECT_EQ(book.FreeSlotCount(), 2u);
}

TEST(Resting, InsertsPricesInBookOrderOnBothSides)
{
  mex::Book book(8, 8);
  FillLog log;

  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 100, 1), 1);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 1), 1);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 95, 1), 1);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Bids().size(), std::size_t{3});
  EXPECT_EQ(book.Bids()[0].price_, mex::Price{90});
  EXPECT_EQ(book.Bids()[1].price_, mex::Price{95});
  EXPECT_EQ(book.Bids()[2].price_, mex::Price{100});

  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 110, 1), 1);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 130, 1), 1);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 120, 1), 1);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Asks().size(), std::size_t{3});
  EXPECT_EQ(book.Asks()[0].price_, mex::Price{130});
  EXPECT_EQ(book.Asks()[1].price_, mex::Price{120});
  EXPECT_EQ(book.Asks()[2].price_, mex::Price{110});
  EXPECT_TRUE(log.Records().empty());
}

TEST(Resting, AppendsSamePriceInArrivalOrder)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 100, 4);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Sell, 100, 6);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Asks().size(), std::size_t{1});
  EXPECT_EQ(book.Asks().back().order_count_, 2u);
  EXPECT_EQ(book.Asks().back().total_qty_, 10u);
  EXPECT_EQ(book.Asks().back().head_, first.order_id_);
  EXPECT_EQ(book.Asks().back().tail_, second.order_id_);
  EXPECT_EQ(book.Order(first.order_id_).next_, second.order_id_);
  EXPECT_EQ(book.Order(second.order_id_).prev_, first.order_id_);
}

TEST(Resting, AllowsZeroAndNegativePrices)
{
  mex::Book book(4, 4);
  FillLog log;

  const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::Buy, -5, 2);
  const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::Sell, -1, 2);
  ExpectResting(bid, 2);
  ExpectResting(ask, 2);
  EXPECT_TRUE(log.Records().empty());
  ExpectBookInvariants(book);

  const mex::SubmitResult sell = SubmitLimit(book, log, mex::Side::Sell, -5, 2);
  ExpectTrade(sell, 2, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], bid.order_id_, -5, 2);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Bids().empty());

  log.Clear();
  const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::Buy, -1, 2);
  ExpectTrade(buy, 2, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], ask.order_id_, -1, 2);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
}

TEST(Matching, PartiallyFillsOneMakerAndRestsTheRest)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 15);
  ExpectTrade(taker, 10, 5, true);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], maker.order_id_, 100, 10);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{100});
  EXPECT_EQ(book.Bids().back().total_qty_, 5u);
  EXPECT_EQ(book.Bids().back().head_, taker.order_id_);
}

TEST(Matching, ExactFillRemovesBothOrders)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  ExpectTrade(taker, 10, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], maker.order_id_, 100, 10);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());
  EXPECT_FALSE(book.CancelOrder(maker.order_id_));
}

TEST(Matching, LeavesAPartiallyFilledMakerAtTheHead)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 50, 5);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Sell, 50, 7);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 50, 3);
  ExpectTrade(taker, 3, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], first.order_id_, 50, 3);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Asks().back().head_, first.order_id_);
  EXPECT_EQ(book.Asks().back().tail_, second.order_id_);
  EXPECT_EQ(book.Order(first.order_id_).quantity_, mex::Quantity{2});
  EXPECT_EQ(book.Order(second.order_id_).quantity_, mex::Quantity{7});
  EXPECT_EQ(book.Asks().back().total_qty_, 9u);
}

TEST(Matching, WalksAskLevelsAndStopsAtTheLimit)
{
  mex::Book book(8, 8);
  FillLog log;

  const mex::SubmitResult ask98 = SubmitLimit(book, log, mex::Side::Sell, 98, 30);
  const mex::SubmitResult ask99 = SubmitLimit(book, log, mex::Side::Sell, 99, 40);
  const mex::SubmitResult ask101 = SubmitLimit(book, log, mex::Side::Sell, 101, 50);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 100);
  ExpectTrade(taker, 70, 30, true);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], ask98.order_id_, 98, 30);
  ExpectFill(log.Records()[1], ask99.order_id_, 99, 40);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Asks().size(), std::size_t{1});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{101});
  EXPECT_EQ(book.Asks().back().head_, ask101.order_id_);
  EXPECT_EQ(book.Bids().back().price_, mex::Price{100});
  EXPECT_EQ(book.Bids().back().total_qty_, 30u);
}

TEST(Matching, SellsIntoBidsFromBestPriceDown)
{
  mex::Book book(8, 8);
  FillLog log;

  const mex::SubmitResult bid100 = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  const mex::SubmitResult bid99 = SubmitLimit(book, log, mex::Side::Buy, 99, 10);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Sell, 99, 15);
  ExpectTrade(taker, 15, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], bid100.order_id_, 100, 10);
  ExpectFill(log.Records()[1], bid99.order_id_, 99, 5);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{99});
  EXPECT_EQ(book.Bids().back().total_qty_, 5u);
  EXPECT_EQ(book.Order(bid99.order_id_).quantity_, mex::Quantity{5});
}

TEST(Matching, FillsSamePriceInFifoOrder)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  ExpectTrade(taker, 10, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], first.order_id_, 100, 10);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().order_count_, 1u);
  EXPECT_EQ(book.Bids().back().head_, second.order_id_);
}

TEST(Matching, ExhaustsOneLevelBeforeTheNextOrderAtThatPrice)
{
  mex::Book book(8, 4);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 100, 4);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Sell, 100, 6);
  const mex::SubmitResult worse = SubmitLimit(book, log, mex::Side::Sell, 101, 9);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 7);
  ExpectTrade(taker, 7, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], first.order_id_, 100, 4);
  ExpectFill(log.Records()[1], second.order_id_, 100, 3);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Asks().size(), std::size_t{2});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{100});
  EXPECT_EQ(book.Asks().back().head_, second.order_id_);
  EXPECT_EQ(book.Order(second.order_id_).quantity_, mex::Quantity{3});
  EXPECT_EQ(book.Order(worse.order_id_).quantity_, mex::Quantity{9});
}

TEST(Matching, RestsWhatIsLeftAfterTheOppositeSideIsGone)
{
  mex::Book book(8, 8);
  FillLog log;

  (void)SubmitLimit(book, log, mex::Side::Sell, 98, 10);
  (void)SubmitLimit(book, log, mex::Side::Sell, 99, 10);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 30);
  ExpectTrade(taker, 20, 10, true);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_EQ(book.Bids().back().price_, mex::Price{100});
  EXPECT_EQ(book.Bids().back().total_qty_, 10u);
}

TEST(Matching, TradesExtremePricesAtTheMakerPrice)
{
  mex::Book book(4, 4);
  FillLog log;

  constexpr std::int64_t kHigh = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kLow = std::numeric_limits<std::int64_t>::min();

  const mex::SubmitResult best_bid = SubmitLimit(book, log, mex::Side::Buy, kHigh, 1);
  const mex::SubmitResult worse_bid = SubmitLimit(book, log, mex::Side::Buy, kHigh - 1, 1);
  const mex::SubmitResult sell = SubmitLimit(book, log, mex::Side::Sell, kHigh - 1, 1);
  ExpectTrade(sell, 1, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], best_bid.order_id_, kHigh, 1);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().head_, worse_bid.order_id_);
  EXPECT_TRUE(book.CancelOrder(worse_bid.order_id_));

  log.Clear();
  const mex::SubmitResult best_ask = SubmitLimit(book, log, mex::Side::Sell, kLow, 1);
  const mex::SubmitResult worse_ask = SubmitLimit(book, log, mex::Side::Sell, kLow + 1, 1);
  const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::Buy, kLow + 1, 2);
  ExpectTrade(buy, 2, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], best_ask.order_id_, kLow, 1);
  ExpectFill(log.Records()[1], worse_ask.order_id_, kLow + 1, 1);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_TRUE(book.Asks().empty());
}

TEST(Market, RejectsWhenTheOppositeSideIsEmpty)
{
  mex::Book book(4, 2);
  FillLog log;

  ExpectRejected(SubmitMarket(book, log, mex::Side::Buy, 10), 0);
  ExpectRejected(SubmitMarket(book, log, mex::Side::Sell, 1), 0);
  EXPECT_TRUE(log.Records().empty());
  ExpectBookInvariants(book);

  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 40, 5), 5);
  const mex::SubmitResult buy = SubmitMarket(book, log, mex::Side::Buy, 10);
  ExpectRejected(buy, 0);
  EXPECT_TRUE(log.Records().empty());
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().total_qty_, 5u);

  const mex::SubmitResult sell = SubmitMarket(book, log, mex::Side::Sell, 1);
  ExpectTrade(sell, 1, 0, false);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().total_qty_, 4u);
}

TEST(Market, FillsWhatIsThereAndCancelsTheRest)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  const mex::SubmitResult taker = SubmitMarket(book, log, mex::Side::Buy, 20);
  ExpectTrade(taker, 10, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], maker.order_id_, 100, 10);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());
}

TEST(Market, SweepsSeveralLevels)
{
  mex::Book book(8, 8);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 10, 2);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Sell, 11, 3);
  const mex::SubmitResult taker = SubmitMarket(book, log, mex::Side::Buy, 100);
  ExpectTrade(taker, 5, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], first.order_id_, 10, 2);
  ExpectFill(log.Records()[1], second.order_id_, 11, 3);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_TRUE(book.Bids().empty());
}

TEST(Cancel, RemovesARestingOrderAndRejectsASecondCancel)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult resting = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  EXPECT_TRUE(book.CancelOrder(resting.order_id_));
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_EQ(book.FreeSlotCount(), book.MaxOrders());

  EXPECT_FALSE(book.CancelOrder(resting.order_id_));
  EXPECT_FALSE(book.CancelOrder(mex::kInvalidOrderId));
  EXPECT_FALSE(book.CancelOrder(book.MaxOrders()));
  EXPECT_FALSE(book.CancelOrder(0));
  ExpectBookInvariants(book);
}

TEST(Cancel, DropsTheMiddleOrderAndKeepsFifo)
{
  mex::Book book(8, 4);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  const mex::SubmitResult middle = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  const mex::SubmitResult last = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  EXPECT_TRUE(book.CancelOrder(middle.order_id_));
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Asks().back().head_, first.order_id_);
  EXPECT_EQ(book.Asks().back().tail_, last.order_id_);
  EXPECT_EQ(book.Order(first.order_id_).next_, last.order_id_);
  EXPECT_EQ(book.Order(last.order_id_).prev_, first.order_id_);

  const mex::SubmitResult first_take = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  ExpectTrade(first_take, 10, 0, false);
  ExpectFill(log.Records()[0], first.order_id_, 100, 10);

  const mex::SubmitResult second_take = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  ExpectTrade(second_take, 10, 0, false);
  ExpectFill(log.Records()[1], last.order_id_, 100, 10);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_FALSE(book.CancelOrder(middle.order_id_));
}

TEST(Cancel, RemovesHeadOrTailWithoutDisturbingTheOtherOrders)
{
  mex::Book book(8, 2);
  FillLog log;

  const mex::SubmitResult head = SubmitLimit(book, log, mex::Side::Buy, 7, 1);
  const mex::SubmitResult mid = SubmitLimit(book, log, mex::Side::Buy, 7, 1);
  const mex::SubmitResult tail = SubmitLimit(book, log, mex::Side::Buy, 7, 1);

  EXPECT_TRUE(book.CancelOrder(tail.order_id_));
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().head_, head.order_id_);
  EXPECT_EQ(book.Bids().back().tail_, mid.order_id_);
  EXPECT_EQ(book.Order(head.order_id_).next_, mid.order_id_);
  EXPECT_EQ(book.Order(mid.order_id_).next_, mex::kInvalidOrderId);

  EXPECT_TRUE(book.CancelOrder(head.order_id_));
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().back().head_, mid.order_id_);
  EXPECT_EQ(book.Bids().back().tail_, mid.order_id_);
  EXPECT_EQ(book.Order(mid.order_id_).prev_, mex::kInvalidOrderId);
  EXPECT_EQ(book.Order(mid.order_id_).quantity_, mex::Quantity{1});
}

TEST(Cancel, FindsALevelAfterLaterInsertsMoveIt)
{
  mex::Book book(8, 4);
  FillLog log;

  const mex::SubmitResult deep_bid = SubmitLimit(book, log, mex::Side::Buy, 100, 7);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 4), 4);
  EXPECT_EQ(book.Bids().front().price_, mex::Price{90});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{100});
  EXPECT_TRUE(book.CancelOrder(deep_bid.order_id_));
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{90});
  EXPECT_EQ(book.Bids().back().total_qty_, 4u);

  const mex::SubmitResult deep_ask = SubmitLimit(book, log, mex::Side::Sell, 110, 3);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 120, 3), 3);
  EXPECT_EQ(book.Asks().back().price_, mex::Price{110});
  EXPECT_TRUE(book.CancelOrder(deep_ask.order_id_));
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Asks().size(), std::size_t{1});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{120});
}

TEST(Cancel, ReusesTheFreedSlotForTheNextOrder)
{
  mex::Book book(2, 2);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Sell, 100, 8);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 1), 1);
  EXPECT_TRUE(book.CancelOrder(first.order_id_));
  ExpectBookInvariants(book);

  const mex::SubmitResult reused = SubmitLimit(book, log, mex::Side::Sell, 105, 2);
  ExpectResting(reused, 2);
  EXPECT_EQ(reused.order_id_, first.order_id_);
  EXPECT_EQ(book.Order(reused.order_id_).price_, mex::Price{105});
  EXPECT_EQ(book.Order(reused.order_id_).quantity_, mex::Quantity{2});
  EXPECT_EQ(book.Asks().back().head_, reused.order_id_);
  ExpectBookInvariants(book);

  EXPECT_TRUE(book.CancelOrder(reused.order_id_));
  EXPECT_FALSE(book.CancelOrder(reused.order_id_));
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
}

TEST(Capacity, FullyFilledTakerDoesNotNeedAFreeSlot)
{
  mex::Book book(2, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 10);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 102, 10), 10);
  EXPECT_EQ(book.FreeSlotCount(), 0u);

  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 101, 5);
  ExpectTrade(taker, 5, 0, false);
  EXPECT_EQ(book.FreeSlotCount(), 0u);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Order(maker.order_id_).quantity_, mex::Quantity{5});
  EXPECT_EQ(book.Asks().back().price_, mex::Price{100});
}

TEST(Capacity, RejectsARestThatDoesNotTradeWhenThePoolIsFull)
{
  mex::Book book(2, 2);
  FillLog log;

  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 100, 10), 10);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 5), 5);
  EXPECT_EQ(book.FreeSlotCount(), 0u);
  const mex::OrderId bid_head = book.Bids().back().head_;
  const std::uint64_t bid_qty = book.Bids().back().total_qty_;
  const mex::OrderId ask_head = book.Asks().back().head_;
  const std::uint64_t ask_qty = book.Asks().back().total_qty_;

  const mex::SubmitResult rejected = SubmitLimit(book, log, mex::Side::Buy, 85, 3);
  ExpectRejected(rejected, 3);
  EXPECT_TRUE(log.Records().empty());
  EXPECT_EQ(book.FreeSlotCount(), 0u);
  EXPECT_EQ(book.Bids().back().head_, bid_head);
  EXPECT_EQ(book.Bids().back().total_qty_, bid_qty);
  EXPECT_EQ(book.Asks().back().head_, ask_head);
  EXPECT_EQ(book.Asks().back().total_qty_, ask_qty);
  ExpectBookInvariants(book);
}

TEST(Capacity, RestsAfterAFillFreesASlot)
{
  mex::Book book(2, 2);
  FillLog log;

  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 100, 10), 10);
  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 5), 5);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 15);
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

  ExpectResting(SubmitLimit(book, log, mex::Side::Buy, 90, 5), 5);
  const mex::SubmitResult best_bid = SubmitLimit(book, log, mex::Side::Buy, 95, 5);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 100, 5), 5);
  ExpectBookInvariants(book);

  const mex::SubmitResult rejected = SubmitLimit(book, log, mex::Side::Buy, 88, 3);
  ExpectRejected(rejected, 3);
  EXPECT_TRUE(log.Records().empty());
  EXPECT_EQ(book.Bids().size(), std::size_t{2});

  const mex::SubmitResult accepted = SubmitLimit(book, log, mex::Side::Sell, 95, 5);
  ExpectTrade(accepted, 5, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], best_bid.order_id_, 95, 5);
  ExpectBookInvariants(book);
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{90});
}

TEST(Capacity, JoinsAnExistingLevelWhenNoNewLevelFits)
{
  mex::Book book(4, 1);
  FillLog log;

  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Buy, 10, 1);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Buy, 10, 2);
  ExpectResting(first, 1);
  ExpectResting(second, 2);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().order_count_, 2u);
  EXPECT_EQ(book.Bids().back().total_qty_, 3u);

  ExpectRejected(SubmitLimit(book, log, mex::Side::Buy, 9, 1), 1);
  EXPECT_EQ(book.Bids().size(), std::size_t{1});

  const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::Sell, 12, 1);
  ExpectResting(ask, 1);
  ExpectRejected(SubmitLimit(book, log, mex::Side::Sell, 13, 1), 1);
  ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 12, 4), 4);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Asks().size(), std::size_t{1});
  EXPECT_EQ(book.Asks().back().total_qty_, 5u);
}

TEST(Capacity, DropsTheUnrestedRemainderWhenTheNewLevelDoesNotFit)
{
  mex::Book book(4, 1);
  FillLog log;

  const mex::SubmitResult resting_bid = SubmitLimit(book, log, mex::Side::Buy, 90, 5);
  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 5);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  ExpectTrade(taker, 5, 5, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], maker.order_id_, 100, 5);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{90});
  EXPECT_EQ(book.Bids().back().head_, resting_bid.order_id_);
  EXPECT_EQ(book.Bids().back().total_qty_, 5u);
}

TEST(Scenario, MixedOrdersKeepTheBookConsistent)
{
  mex::Book book(8, 4);
  FillLog log;

  const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::Buy, 10, 5);
  ExpectBookInvariants(book);
  const mex::SubmitResult ask_a = SubmitLimit(book, log, mex::Side::Sell, 12, 4);
  const mex::SubmitResult ask_b = SubmitLimit(book, log, mex::Side::Sell, 12, 4);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.CancelOrder(ask_a.order_id_));
  ExpectBookInvariants(book);

  const mex::SubmitResult buy = SubmitLimit(book, log, mex::Side::Buy, 12, 3);
  ExpectTrade(buy, 3, 0, false);
  ExpectBookInvariants(book);
  EXPECT_EQ(book.Order(ask_b.order_id_).quantity_, mex::Quantity{1});
  EXPECT_EQ(book.Order(bid.order_id_).quantity_, mex::Quantity{5});

  log.Clear();
  const mex::SubmitResult market = SubmitMarket(book, log, mex::Side::Sell, 9);
  ExpectTrade(market, 5, 0, false);
  ExpectFill(log.Records()[0], bid.order_id_, 10, 5);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_EQ(book.Asks().back().total_qty_, 1u);
  EXPECT_FALSE(book.CancelOrder(bid.order_id_));
  EXPECT_FALSE(book.CancelOrder(ask_a.order_id_));
  ExpectBookInvariants(book);
}

TEST(Fills, LastSubmitReplacesTheFillBuffer)
{
  mex::Book book(4, 2);
  FillLog log;

  const mex::SubmitResult first_maker = SubmitLimit(book, log, mex::Side::Sell, 100, 2);
  const mex::SubmitResult first_take = SubmitLimit(book, log, mex::Side::Buy, 100, 2);
  ExpectTrade(first_take, 2, 0, false);
  ASSERT_EQ(book.Fills().size(), std::size_t{1});
  ExpectFill(book.Fills()[0], first_maker.order_id_, 100, 2);

  log.Clear();
  const mex::SubmitResult second_maker = SubmitLimit(book, log, mex::Side::Sell, 101, 3);
  const mex::SubmitResult second_take = SubmitLimit(book, log, mex::Side::Buy, 101, 3);
  ExpectTrade(second_take, 3, 0, false);
  ASSERT_EQ(book.Fills().size(), std::size_t{1});
  ExpectFill(book.Fills()[0], second_maker.order_id_, 101, 3);
  EXPECT_EQ(log.Records().size(), std::size_t{1});
}

TEST(Matching, TakesMaxQuantityFromASmallerMaker)
{
  mex::Book book(2, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 7, 4);
  const mex::SubmitResult taker =
      SubmitMarket(book, log, mex::Side::Buy, std::numeric_limits<std::uint32_t>::max());
  ExpectTrade(taker, 4, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{1});
  ExpectFill(log.Records()[0], maker.order_id_, 7, 4);
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_TRUE(book.Bids().empty());
}

TEST(Fills, RemovedMakerIdMayNameTheNewRestingOrder)
{
  mex::Book book(2, 2);
  FillLog log;

  const mex::SubmitResult maker = SubmitLimit(book, log, mex::Side::Sell, 100, 1);
  const mex::SubmitResult taker = SubmitLimit(book, log, mex::Side::Buy, 100, 3);
  ExpectTrade(taker, 1, 2, true);
  ASSERT_EQ(book.Fills().size(), std::size_t{1});
  const mex::Fill fill = book.Fills()[0];
  EXPECT_EQ(fill.maker_id_, maker.order_id_);
  EXPECT_TRUE(fill.maker_removed_);
  EXPECT_EQ(fill.side_, mex::Side::Sell);
  EXPECT_EQ(taker.order_id_, maker.order_id_);
  EXPECT_EQ(book.Order(fill.maker_id_).side_, mex::Side::Buy);
  EXPECT_EQ(book.Order(fill.maker_id_).quantity_, mex::Quantity{2});
  ExpectBookInvariants(book);
}

TEST(Fills, OneMarketOrderCanFillEveryRestingSlot)
{
  constexpr std::uint32_t kOrders = 64;
  mex::Book book(kOrders, 1);
  FillLog log;

  for (std::uint32_t i = 0; i < kOrders; ++i)
  {
    ExpectResting(SubmitLimit(book, log, mex::Side::Sell, 100, 1), 1);
  }
  EXPECT_EQ(book.FreeSlotCount(), 0u);

  log.Clear();
  const mex::SubmitResult taker = SubmitMarket(book, log, mex::Side::Buy, kOrders);
  ExpectTrade(taker, kOrders, 0, false);
  ASSERT_EQ(book.Fills().size(), std::size_t{kOrders});
  EXPECT_EQ(log.Records().size(), std::size_t{kOrders});
  for (const mex::Fill &fill : book.Fills())
  {
    EXPECT_TRUE(fill.maker_removed_);
    EXPECT_EQ(fill.side_, mex::Side::Sell);
    EXPECT_EQ(fill.quantity_, mex::Quantity{1});
  }
  ExpectBookInvariants(book);
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_EQ(book.FreeSlotCount(), kOrders);
}

TEST(Replace, ReduceKeepsTimePriority)
{
  mex::Book book(4, 2);
  FillLog log;
  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Buy, 100, 10);

  ASSERT_TRUE(book.ReduceOrder(first.order_id_, mex::Quantity{4}));
  EXPECT_EQ(book.Bids().back().total_qty_, 14u);
  ExpectBookInvariants(book);

  ExpectTrade(SubmitLimit(book, log, mex::Side::Sell, 100, 5), 5, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], first.order_id_, 100, 4);
  ExpectFill(log.Records()[1], second.order_id_, 100, 1);
  ExpectBookInvariants(book);
}

TEST(Replace, ReduceRejectsNoReductionAndOrdersNotResting)
{
  mex::Book book(2, 2);
  FillLog log;
  const mex::SubmitResult order = SubmitLimit(book, log, mex::Side::Sell, 100, 5);

  EXPECT_FALSE(book.ReduceOrder(order.order_id_, mex::Quantity{5}));
  EXPECT_FALSE(book.ReduceOrder(order.order_id_, mex::Quantity{6}));
  EXPECT_FALSE(book.ReduceOrder(mex::kInvalidOrderId, mex::Quantity{1}));
  ASSERT_TRUE(book.CancelOrder(order.order_id_));
  EXPECT_FALSE(book.ReduceOrder(order.order_id_, mex::Quantity{1}));
  EXPECT_TRUE(book.Asks().empty());
  ExpectBookInvariants(book);
}

TEST(Replace, SamePriceIncreaseLosesTimePriority)
{
  mex::Book book(4, 2);
  FillLog log;
  const mex::SubmitResult first = SubmitLimit(book, log, mex::Side::Buy, 100, 10);
  const mex::SubmitResult second = SubmitLimit(book, log, mex::Side::Buy, 100, 10);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(first.order_id_, mex::Price{100}, mex::Quantity{12});
  ExpectResting(replaced, 12);
  EXPECT_EQ(book.Bids().back().total_qty_, 22u);
  ExpectBookInvariants(book);

  ExpectTrade(SubmitLimit(book, log, mex::Side::Sell, 100, 11), 11, 0, false);
  ASSERT_EQ(log.Records().size(), std::size_t{2});
  ExpectFill(log.Records()[0], second.order_id_, 100, 10);
  ExpectFill(log.Records()[1], replaced.order_id_, 100, 1);
}

TEST(Replace, NewPriceCanMatchAtOnceAndRestTheRest)
{
  mex::Book book(4, 3);
  FillLog log;
  const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::Sell, 101, 3);
  const mex::SubmitResult bid = SubmitLimit(book, log, mex::Side::Buy, 99, 5);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(bid.order_id_, mex::Price{101}, mex::Quantity{5});

  ExpectTrade(replaced, 3, 2, true);
  ASSERT_EQ(book.Fills().size(), std::size_t{1});
  ExpectFill(book.Fills()[0], ask.order_id_, 101, 3);
  EXPECT_TRUE(book.Asks().empty());
  ASSERT_EQ(book.Bids().size(), std::size_t{1});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{101});
  EXPECT_EQ(book.Order(replaced.order_id_).quantity_, mex::Quantity{2});
  ExpectBookInvariants(book);
}

TEST(Replace, FullMatchLeavesNothingResting)
{
  mex::Book book(4, 2);
  FillLog log;
  SubmitLimit(book, log, mex::Side::Buy, 99, 4);
  const mex::SubmitResult ask = SubmitLimit(book, log, mex::Side::Sell, 101, 4);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(ask.order_id_, mex::Price{98}, mex::Quantity{4});

  ExpectTrade(replaced, 4, 0, false);
  EXPECT_TRUE(book.Bids().empty());
  EXPECT_TRUE(book.Asks().empty());
  EXPECT_EQ(book.FreeSlotCount(), 4u);
  ExpectBookInvariants(book);
}

TEST(Replace, RejectsWithoutChangeWhenTheNewLevelCannotFit)
{
  mex::Book book(4, 2);
  FillLog log;
  const mex::SubmitResult moving = SubmitLimit(book, log, mex::Side::Buy, 100, 5);
  SubmitLimit(book, log, mex::Side::Buy, 100, 6);
  SubmitLimit(book, log, mex::Side::Buy, 99, 7);
  SubmitLimit(book, log, mex::Side::Sell, 105, 1);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(moving.order_id_, mex::Price{98}, mex::Quantity{5});

  ExpectRejected(replaced, 5);
  EXPECT_TRUE(book.Fills().empty());
  EXPECT_EQ(book.Order(moving.order_id_).quantity_, mex::Quantity{5});
  EXPECT_EQ(book.Bids().back().head_, moving.order_id_);
  EXPECT_EQ(book.Bids().back().total_qty_, 11u);
  EXPECT_EQ(book.FreeSlotCount(), 0u);
  ExpectBookInvariants(book);
}

TEST(Replace, FullLadderAllowsAMoveThatFreesTheOwnLevel)
{
  mex::Book book(4, 2);
  FillLog log;
  const mex::SubmitResult alone = SubmitLimit(book, log, mex::Side::Buy, 100, 5);
  SubmitLimit(book, log, mex::Side::Buy, 99, 7);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(alone.order_id_, mex::Price{98}, mex::Quantity{5});

  ExpectResting(replaced, 5);
  ASSERT_EQ(book.Bids().size(), std::size_t{2});
  EXPECT_EQ(book.Bids().front().price_, mex::Price{98});
  EXPECT_EQ(book.Bids().back().price_, mex::Price{99});
  ExpectBookInvariants(book);
}

TEST(Replace, RejectsAnOrderThatIsNotResting)
{
  mex::Book book(2, 2);
  FillLog log;
  const mex::SubmitResult order = SubmitLimit(book, log, mex::Side::Buy, 100, 5);
  ASSERT_TRUE(book.CancelOrder(order.order_id_));

  ExpectRejected(book.ReplaceOrder(order.order_id_, mex::Price{100}, mex::Quantity{5}), 5);
  ExpectRejected(book.ReplaceOrder(mex::kInvalidOrderId, mex::Price{100}, mex::Quantity{5}), 5);
  EXPECT_EQ(book.FreeSlotCount(), 2u);
}

// The removed order frees its slot first, so the replacement can rest in the same slot.
TEST(Replace, NewRestingOrderCanReuseTheOldSlot)
{
  mex::Book book(2, 2);
  FillLog log;
  const mex::SubmitResult order = SubmitLimit(book, log, mex::Side::Sell, 100, 5);

  const mex::SubmitResult replaced =
      book.ReplaceOrder(order.order_id_, mex::Price{101}, mex::Quantity{5});

  ExpectResting(replaced, 5);
  EXPECT_EQ(replaced.order_id_, order.order_id_);
  EXPECT_EQ(book.Order(replaced.order_id_).price_, mex::Price{101});
}

// Checks that restore preserves explicit slots, FIFO links, and the free-slot stack.
TEST(Restore, PreservesSlotsPriorityAndFreeStack)
{
  mex::Book book{4, 2};
  const std::array orders{
      mex::Book::RestingOrder{1, mex::Side::Sell, mex::Price{100}, mex::Quantity{5}},
      mex::Book::RestingOrder{3, mex::Side::Sell, mex::Price{100}, mex::Quantity{6}}};
  const std::array<mex::OrderId, 2> free_slots{2, 0};
  ASSERT_TRUE(book.Restore(orders, free_slots));
  ExpectBookInvariants(book);
  const auto rest = book.SubmitLimitOrder(mex::Side::Buy, mex::Price{99}, mex::Quantity{1});
  EXPECT_EQ(rest.order_id_, 0U);
  const auto take = book.SubmitMarketOrder(mex::Side::Buy, mex::Quantity{7});
  EXPECT_EQ(take.filled_qty_, mex::Quantity{7});
  ASSERT_EQ(book.Fills().size(), 2U);
  EXPECT_EQ(book.Fills()[0].maker_id_, 1U);
  EXPECT_EQ(book.Fills()[1].maker_id_, 3U);
  EXPECT_EQ(book.Order(3).quantity_, mex::Quantity{4});
  ExpectBookInvariants(book);
}

// Moving an order preserves its peers and the sorted ladders on either side.
TEST(Replace, MovesAcrossLevelsOnBothSides)
{
  for (const mex::Side side : {mex::Side::Buy, mex::Side::Sell})
  {
    for (const std::int64_t old_price : {90, 100, 110})
    {
      for (const std::int64_t new_price : {80, 90, 95, 100, 105, 110, 120})
      {
        for (const bool shared_level : {false, true})
        {
          SCOPED_TRACE(::testing::Message() << static_cast<int>(side) << ' ' << old_price << ' '
                                            << new_price << ' ' << shared_level);
          mex::Book book(8, 4);
          FillLog log;
          for (const std::int64_t price : {90, 100, 110})
          {
            if (price != old_price || shared_level)
            {
              ExpectResting(SubmitLimit(book, log, side, price, 5), 5);
            }
          }
          const mex::SubmitResult moving = SubmitLimit(book, log, side, old_price, 3);
          const std::uint32_t free_slots = book.FreeSlotCount();

          const mex::SubmitResult result =
              book.ReplaceOrder(moving.order_id_, mex::Price{new_price}, mex::Quantity{7});

          ExpectResting(result, 7);
          EXPECT_EQ(book.Order(result.order_id_).price_, mex::Price{new_price});
          EXPECT_EQ(book.Order(result.order_id_).side_, side);
          EXPECT_EQ(book.FreeSlotCount(), free_slots);
          EXPECT_TRUE(book.Fills().empty());
          for (mex::OrderId id = 0; id < book.MaxOrders(); ++id)
          {
            if (id != result.order_id_ && book.Order(id).quantity_ > mex::Quantity{})
            {
              EXPECT_EQ(book.Order(id).quantity_, mex::Quantity{5});
            }
          }
          ExpectBookInvariants(book);
        }
      }
    }
  }
}

// A full ladder can accept a move to an existing level and keeps that level's FIFO order.
TEST(Replace, FullLadderAcceptsExistingPriceOnBothSides)
{
  for (const mex::Side side : {mex::Side::Buy, mex::Side::Sell})
  {
    mex::Book book(3, 2);
    FillLog log;
    const mex::SubmitResult moving = SubmitLimit(book, log, side, 90, 3);
    const mex::SubmitResult peer = SubmitLimit(book, log, side, 90, 4);
    const mex::SubmitResult destination = SubmitLimit(book, log, side, 100, 5);

    const mex::SubmitResult result =
        book.ReplaceOrder(moving.order_id_, mex::Price{100}, mex::Quantity{7});

    ExpectResting(result, 7);
    EXPECT_EQ(book.FreeSlotCount(), 0u);
    ExpectBookInvariants(book);
    const bool buy = side == mex::Side::Buy;
    const mex::Side opposite = buy ? mex::Side::Sell : mex::Side::Buy;
    const std::uint32_t quantity = buy ? 6U : 10U;
    ExpectTrade(SubmitMarket(book, log, opposite, quantity), quantity, 0, false);
    ASSERT_EQ(log.Records().size(), buy ? 2U : 3U);
    if (!buy)
    {
      ExpectFill(log.Records()[0], peer.order_id_, 90, 4);
    }
    ExpectFill(log.Records()[buy ? 0U : 1U], destination.order_id_, 100, 5);
    ExpectFill(log.Records().back(), result.order_id_, 100, 1);
    ExpectBookInvariants(book);
  }
}

// Capacity rejection happens before matching, so both sides keep their orders.
TEST(Replace, FullLadderRejectsBeforeMatchingOnBothSides)
{
  for (const mex::Side side : {mex::Side::Buy, mex::Side::Sell})
  {
    mex::Book book(4, 2);
    FillLog log;
    const bool buy = side == mex::Side::Buy;
    const mex::Side opposite = buy ? mex::Side::Sell : mex::Side::Buy;
    const std::int64_t price = buy ? 120 : 80;
    const mex::SubmitResult moving = SubmitLimit(book, log, side, 100, 3);
    SubmitLimit(book, log, side, 100, 4);
    SubmitLimit(book, log, side, buy ? 90 : 110, 5);
    const mex::SubmitResult maker = SubmitLimit(book, log, opposite, price, 6);

    ExpectRejected(book.ReplaceOrder(moving.order_id_, mex::Price{price}, mex::Quantity{7}), 7);

    EXPECT_TRUE(book.Fills().empty());
    EXPECT_EQ(book.Order(moving.order_id_).quantity_, mex::Quantity{3});
    EXPECT_EQ(book.Order(maker.order_id_).quantity_, mex::Quantity{6});
    EXPECT_EQ(book.FreeSlotCount(), 0u);
    ExpectBookInvariants(book);
  }
}

} // namespace
