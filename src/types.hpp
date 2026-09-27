#pragma once

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

// TODO: Replace std::int64_t prices with a strong Price type, in integer ticks, so a price cannot
// be passed as a quantity or another int64.
// TODO: Replace std::uint32_t order sizes with a strong Quantity type so a quantity cannot be
// passed as a price, an order count, or a pool index. Level total_qty_ is a uint64 sum of those
// quantities and should stay wide enough to hold that sum.

struct SubmitResult
{
  SubmitStatus status_ = SubmitStatus::Rejected;
  std::uint32_t filled_qty_ = 0;
  std::uint32_t remaining_ = 0;
  OrderId order_id_ = kInvalidOrderId;
};

} // namespace mex
