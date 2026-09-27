#pragma once

#include "order_node.hpp"
#include "price_level.hpp"
#include "types.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace mex
{

// One symbol. Storage is fixed at construction: a full pool or a full
// price ladder rejects the order instead of allocating.
// Throughput: ./build/bench_book (-O3). See docs/performance.md.
class Book
{
public:
  explicit Book(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels);

  template <typename OnFill>
  [[nodiscard]] SubmitResult SubmitLimitOrder(Side p_side, std::int64_t p_price,
                                              std::uint32_t p_quantity, OnFill &&p_on_fill);

  template <typename OnFill>
  [[nodiscard]] SubmitResult SubmitMarketOrder(Side p_side, std::uint32_t p_quantity,
                                               OnFill &&p_on_fill);

  [[nodiscard]] bool CancelOrder(OrderId p_order_id);

  [[nodiscard]] const OrderNode &Order(OrderId p_order_id) const;

  [[nodiscard]] const std::vector<PriceLevel> &Bids() const;
  [[nodiscard]] const std::vector<PriceLevel> &Asks() const;

  [[nodiscard]] std::uint32_t FreeSlotCount() const;
  [[nodiscard]] std::uint32_t MaxOrders() const;

private:
  using FillCallback = void (*)(OrderId p_maker_id, std::int64_t p_price, std::uint32_t p_quantity,
                                void *p_context);

  [[nodiscard]] SubmitResult SubmitLimit(Side p_side, std::int64_t p_price,
                                         std::uint32_t p_quantity, FillCallback p_on_fill,
                                         void *p_context);

  [[nodiscard]] SubmitResult SubmitMarket(Side p_side, std::uint32_t p_quantity,
                                          FillCallback p_on_fill, void *p_context);

  // limit_price is absent for a market order, which crosses every level.
  [[nodiscard]] std::uint32_t Match(Side p_aggressor, std::optional<std::int64_t> p_limit_price,
                                    std::uint32_t p_quantity, FillCallback p_on_fill,
                                    void *p_context);

  [[nodiscard]] bool CanRest(Side p_side, std::int64_t p_price) const;

  OrderId Rest(Side p_side, std::int64_t p_price, std::uint32_t p_quantity);

  void Unlink(PriceLevel &p_level, OrderId p_order_id);
  void Release(OrderId p_order_id);

  [[nodiscard]] std::vector<PriceLevel> &Levels(Side p_side);
  [[nodiscard]] const std::vector<PriceLevel> &Levels(Side p_side) const;

  std::vector<OrderNode> pool_;
  std::vector<OrderId> free_list_;
  std::vector<PriceLevel> bids_;
  std::vector<PriceLevel> asks_;
  std::uint32_t max_price_levels_;
};

template <typename OnFill>
SubmitResult Book::SubmitLimitOrder(Side p_side, std::int64_t p_price, std::uint32_t p_quantity,
                                    OnFill &&p_on_fill)
{
  using Callback = std::remove_reference_t<OnFill>;
  auto thunk =
      [](OrderId p_maker_id, std::int64_t p_fill_price, std::uint32_t p_fill_qty, void *p_context)
  { (*static_cast<Callback *>(p_context))(p_maker_id, p_fill_price, p_fill_qty); };
  return SubmitLimit(p_side, p_price, p_quantity, thunk, &p_on_fill);
}

template <typename OnFill>
SubmitResult Book::SubmitMarketOrder(Side p_side, std::uint32_t p_quantity, OnFill &&p_on_fill)
{
  using Callback = std::remove_reference_t<OnFill>;
  auto thunk =
      [](OrderId p_maker_id, std::int64_t p_fill_price, std::uint32_t p_fill_qty, void *p_context)
  { (*static_cast<Callback *>(p_context))(p_maker_id, p_fill_price, p_fill_qty); };
  return SubmitMarket(p_side, p_quantity, thunk, &p_on_fill);
}

} // namespace mex
