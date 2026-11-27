#include "book.hpp"
#include "market_data.hpp"
#include "types.hpp"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>

namespace mex
{
namespace
{

// Read live views during a burst and check that the final view survives conflation.
TEST(MarketDataHandoff, DeliversCoherentAndLatestDepth)
{
  constexpr std::uint32_t kUpdates = 100'000;
  MarketBook channel;
  Book book{2, 1};
  std::atomic<bool> finished{false};
  const std::jthread writer{
      [book_ptr = &book, channel_ptr = &channel, done = &finished]
      {
        for (std::uint32_t qty = 1; qty <= kUpdates; ++qty)
        {
          const auto bid = book_ptr->SubmitLimitOrder(Side::Buy, Price{100}, Quantity{qty});
          const auto ask = book_ptr->SubmitLimitOrder(Side::Sell, Price{101}, Quantity{qty});
          channel_ptr->Publish(*book_ptr);
          EXPECT_TRUE(book_ptr->CancelOrder(bid.order_id_));
          EXPECT_TRUE(book_ptr->CancelOrder(ask.order_id_));
        }
        done->store(true, std::memory_order_release);
      }};

  std::uint64_t last_quantity = 0;
  const auto check = [&last_quantity](const MarketDepth &p_view)
  {
    EXPECT_EQ(p_view.bid_count_, 1U);
    EXPECT_EQ(p_view.ask_count_, 1U);
    EXPECT_EQ(p_view.bids_[0].price_, Price{100});
    EXPECT_EQ(p_view.asks_[0].price_, Price{101});
    EXPECT_EQ(p_view.bids_[0].quantity_, p_view.asks_[0].quantity_);
    EXPECT_GT(p_view.bids_[0].quantity_, last_quantity);
    last_quantity = p_view.bids_[0].quantity_;
  };

  while (!finished.load(std::memory_order_acquire))
  {
    if (const auto view = channel.Take())
    {
      check(*view);
    }
  }
  if (const auto view = channel.Take())
  {
    check(*view);
  }
  EXPECT_EQ(last_quantity, kUpdates);
  EXPECT_FALSE(channel.Take());
}

} // namespace
} // namespace mex
