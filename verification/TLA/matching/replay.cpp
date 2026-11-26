#include "book.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <print>
#include <utility>
#include <vector>

namespace mex
{
namespace
{

struct Resting
{
  std::uint32_t logical_id_;
  unsigned side_;
  std::int64_t price_;
  std::uint32_t quantity_;
};

void PrintBook(const Book &p_book, const std::vector<std::uint32_t> &p_ids)
{
  std::vector<Resting> orders;
  for (const Side side : {Side::Buy, Side::Sell})
  {
    const PriceLadder &levels = side == Side::Buy ? p_book.Bids() : p_book.Asks();
    for (const PriceLevel &level : levels)
    {
      OrderId slot = level.head_;
      for (std::uint32_t i = 0; i < level.order_count_; ++i)
      {
        const OrderNode &order = p_book.Order(slot);
        orders.push_back({p_ids.at(slot), side == Side::Buy ? 1u : 2u, order.price_.ticks_,
                          order.quantity_.units_});
        slot = order.next_;
      }
    }
  }

  std::ranges::stable_sort(orders, {}, [](const Resting &p_order)
                           { return std::pair{p_order.side_, p_order.price_}; });
  for (const Resting &order : orders)
  {
    std::print(" {}:{}:{}:{}", order.logical_id_, order.side_, order.price_, order.quantity_);
  }
  std::println();
}

bool Replay(Book &p_book, std::vector<std::uint32_t> &p_ids)
{
  char action{};
  std::uint32_t id{};
  unsigned side{};
  std::int64_t price{};
  std::uint32_t quantity{};
  while (std::cin >> action >> id >> side >> price >> quantity)
  {
    const Side book_side = side == 1 ? Side::Buy : Side::Sell;
    const auto found = std::ranges::find(p_ids, id);
    const OrderId slot = id == 0 || found == p_ids.end()
                             ? kInvalidOrderId
                             : static_cast<OrderId>(found - p_ids.begin());
    SubmitResult result{};
    switch (action)
    {
    case 'L':
      result = p_book.SubmitLimitOrder(book_side, Price{price}, Quantity{quantity});
      break;
    case 'M':
      result = p_book.SubmitMarketOrder(book_side, Quantity{quantity});
      break;
    case 'R':
      result = p_book.ReplaceOrder(slot, Price{price}, Quantity{quantity});
      break;
    case 'C':
      if (p_book.CancelOrder(slot))
      {
        p_ids.at(slot) = 0;
        result.status_ = SubmitStatus::Accepted;
      }
      break;
    case 'D':
      if (p_book.ReduceOrder(slot, Quantity{quantity}))
      {
        result.status_ = SubmitStatus::Accepted;
      }
      break;
    default:
      return false;
    }

    std::print("{} {} {} |", static_cast<int>(result.status_ == SubmitStatus::Accepted),
               result.filled_qty_.units_, result.remaining_.units_);
    if (action == 'L' || action == 'M' || action == 'R')
    {
      for (const Fill &fill : p_book.Fills())
      {
        std::print(" {}:{}:{}", p_ids.at(fill.maker_id_), fill.price_.ticks_,
                   fill.quantity_.units_);
        if (fill.maker_removed_)
        {
          p_ids.at(fill.maker_id_) = 0;
        }
      }
    }
    if (action == 'R' && result.status_ == SubmitStatus::Accepted)
    {
      p_ids.at(slot) = 0;
    }
    if (result.order_id_ != kInvalidOrderId)
    {
      p_ids.at(result.order_id_) = id;
    }
    std::print(" |");
    PrintBook(p_book, p_ids);
  }
  return std::cin.eof();
}

} // namespace
} // namespace mex

int main()
{
  std::uint32_t capacity{};
  std::uint32_t levels{};
  if (!(std::cin >> capacity >> levels) || capacity == 0)
  {
    return 1;
  }
  mex::Book book{capacity, levels};
  std::vector<std::uint32_t> ids(capacity);
  return mex::Replay(book, ids) ? 0 : 1;
}
