#include "book.hpp"
#include "market_data.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <print>

namespace mex
{
namespace
{

constexpr std::uint32_t kMaxOrders = 128;
constexpr std::uint32_t kMaxSteps = kMaxOrders + 2;

void PrintDepth(MarketBook &p_channel, const Book &p_book)
{
  p_channel.Publish(p_book);
  const auto depth = p_channel.Take();
  if (!depth)
  {
    return;
  }
  std::print("B");
  for (std::size_t index = 0; index < depth->bid_count_; ++index)
  {
    const auto &level = depth->bids_[index];
    std::print(" {}:{}", level.price_.ticks_, level.quantity_);
  }
  std::print(" | A");
  for (std::size_t index = 0; index < depth->ask_count_; ++index)
  {
    const auto &level = depth->asks_[index];
    std::print(" {}:{}", level.price_.ticks_, level.quantity_);
  }
  std::println();
}

// Observe depth through the same handoff that feeds the live publisher.
[[nodiscard]] bool ReplayDepth()
{
  Book book{kMaxOrders, kMaxOrders};
  MarketBook channel;
  std::array<OrderId, kMaxOrders> ids{};
  ids.fill(kInvalidOrderId);
  std::uint32_t count{};
  if (!(std::cin >> count) || count > kMaxSteps)
  {
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index)
  {
    char action{};
    unsigned side{};
    std::uint32_t id{};
    std::int64_t price{};
    std::uint32_t quantity{};
    if (!(std::cin >> action >> id >> side >> price >> quantity) || id >= ids.size())
    {
      return false;
    }
    if (action == 'C')
    {
      if (!book.CancelOrder(ids[id]))
      {
        return false;
      }
      continue;
    }
    if (action != 'L' || (side != 1 && side != 2))
    {
      return false;
    }
    const auto order =
        book.SubmitLimitOrder(side == 1 ? Side::Buy : Side::Sell, Price{price}, Quantity{quantity});
    if (order.order_id_ == kInvalidOrderId)
    {
      return false;
    }
    ids[id] = order.order_id_;
  }
  PrintDepth(channel, book);
  return true;
}

} // namespace
} // namespace mex

int main() { return mex::ReplayDepth() ? 0 : 1; }
