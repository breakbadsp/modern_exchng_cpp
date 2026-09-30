#pragma once

#include <compare>
#include <cstdint>
#include <limits>

namespace mex
{

enum class Side : std::uint8_t
{
  Buy = 0,
  Sell,
};

enum class SubmitStatus : std::uint8_t
{
  Accepted = 0,
  Rejected,
};

using OrderId = std::uint32_t;

constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();

// Integer ticks. Distinct from Quantity so a price cannot be passed as a size.
struct Price
{
  std::int64_t ticks_ = 0;

  constexpr auto operator<=>(const Price &) const = default;
};

// Order size. Distinct from Price, OrderId, and level order_count_.
struct Quantity
{
  std::uint32_t units_ = 0;

  constexpr auto operator<=>(const Quantity &) const = default;
};

constexpr Quantity &operator+=(Quantity &p_lhs, Quantity p_rhs)
{
  p_lhs.units_ += p_rhs.units_;
  return p_lhs;
}

constexpr Quantity &operator-=(Quantity &p_lhs, Quantity p_rhs)
{
  p_lhs.units_ -= p_rhs.units_;
  return p_lhs;
}

[[nodiscard]] constexpr Quantity operator+(Quantity p_lhs, Quantity p_rhs)
{
  p_lhs += p_rhs;
  return p_lhs;
}

[[nodiscard]] constexpr Quantity operator-(Quantity p_lhs, Quantity p_rhs)
{
  p_lhs -= p_rhs;
  return p_lhs;
}

struct SubmitResult
{
  SubmitStatus status_ = SubmitStatus::Rejected;
  Quantity filled_qty_{};
  Quantity remaining_{};
  OrderId order_id_ = kInvalidOrderId;
};

// A resting limit order. The pool index is the order id, so it is not stored again.
struct OrderNode
{
  Price price_{};
  Quantity quantity_{};
  OrderId prev_ = kInvalidOrderId;
  OrderId next_ = kInvalidOrderId;
  Side side_ = Side::Buy;
};

// One price on one side. Orders at this price form a FIFO list in the order pool.
// total_qty_ is a uint64 sum of Quantity values so it can hold many orders at a level.
struct PriceLevel
{
  Price price_{};
  OrderId head_ = kInvalidOrderId;
  OrderId tail_ = kInvalidOrderId;
  std::uint32_t order_count_ = 0;
  std::uint64_t total_qty_ = 0;
};

} // namespace mex
