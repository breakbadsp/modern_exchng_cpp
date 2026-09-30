#pragma once

#include "types.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace mex
{

// Fixed buffer of price levels. Size may change; capacity never does after construct.
class PriceLadder
{
public:
  PriceLadder() = default;
  explicit PriceLadder(std::uint32_t p_capacity);

  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint32_t Capacity() const;

  [[nodiscard]] PriceLevel &operator[](std::size_t p_index) pre(p_index < size_);
  [[nodiscard]] const PriceLevel &operator[](std::size_t p_index) const pre(p_index < size_);

  [[nodiscard]] PriceLevel &front() pre(size_ > 0);
  [[nodiscard]] const PriceLevel &front() const pre(size_ > 0);
  [[nodiscard]] PriceLevel &back() pre(size_ > 0);
  [[nodiscard]] const PriceLevel &back() const pre(size_ > 0);

  [[nodiscard]] PriceLevel *begin();
  [[nodiscard]] PriceLevel *end();
  [[nodiscard]] const PriceLevel *begin() const;
  [[nodiscard]] const PriceLevel *end() const;

  void Insert(std::uint32_t p_index, PriceLevel p_level) pre(size_ < capacity_)
      pre(p_index <= size_);
  void Erase(std::uint32_t p_index) pre(p_index < size_);
  void PopBack() pre(size_ > 0);

private:
  std::unique_ptr<PriceLevel[]> storage_{};
  std::uint32_t size_ = 0;
  std::uint32_t capacity_ = 0;
};

// One symbol. Storage is allocated once at construction: a full pool or a full
// price ladder rejects the order instead of allocating.
// Throughput: ./build/bench_book (-O3). See docs/performance.md.
//
// Contracts catch caller bugs and broken book structure. Zero quantity and a
// full pool are normal rejects, not failures.
//
// Fills from the last submit are in Fills() until the next submit. Matching
// does not call into user code.
// clang-format off
class Book
{
public:
  explicit Book(std::uint32_t p_max_orders, std::uint32_t p_max_price_levels)
    pre (p_max_orders > 0);

  [[nodiscard]] SubmitResult SubmitLimitOrder(Side p_side, Price p_price, const Quantity p_quantity)
    post (r: static_cast<std::uint64_t>(r.filled_qty_.units_) + r.remaining_.units_ == p_quantity.units_);

  [[nodiscard]] SubmitResult SubmitMarketOrder(Side p_side, const Quantity p_quantity)
    post (r: r.filled_qty_ <= p_quantity && r.remaining_ == Quantity{} && r.order_id_ == kInvalidOrderId);

  [[nodiscard]] bool CancelOrder(const OrderId p_order_id)
    post (r: !r || (p_order_id < MaxOrders() && Order(p_order_id).quantity_ == Quantity{}));

  [[nodiscard]] const OrderNode &Order(OrderId p_order_id) const
    pre (p_order_id < MaxOrders());

  [[nodiscard]] const PriceLadder &Bids() const;
  [[nodiscard]] const PriceLadder &Asks() const;

  [[nodiscard]] std::span<const Fill> Fills() const;

  [[nodiscard]] std::uint32_t FreeSlotCount() const;
  [[nodiscard]] std::uint32_t MaxOrders() const;

private:
  void ClearFills();

  [[nodiscard]] Quantity Match(Side p_aggressor, const Price *p_limit_price, Quantity p_quantity);

  [[nodiscard]] bool CanRest(Side p_side, Price p_price) const;

  OrderId Rest(Side p_side, Price p_price, Quantity p_quantity)
    pre (free_count_ > 0)
    pre (p_quantity > Quantity{});

  void Unlink(PriceLevel &p_level, OrderId p_order_id)
    pre (p_order_id < pool_size_);

  void Release(OrderId p_order_id)
    pre (p_order_id < pool_size_)
    pre (free_count_ < pool_size_);

  [[nodiscard]] PriceLadder &Levels(Side p_side);
  [[nodiscard]] const PriceLadder &Levels(Side p_side) const;

  std::unique_ptr<OrderNode[]> pool_{};
  std::uint32_t pool_size_ = 0;
  std::unique_ptr<OrderId[]> free_ids_{};
  std::uint32_t free_count_ = 0;
  PriceLadder bids_{};
  PriceLadder asks_{};
  std::uint32_t max_price_levels_ = 0;
  std::unique_ptr<Fill[]> fills_{};
  std::uint32_t fill_count_ = 0;
};
// clang-format on

} // namespace mex
