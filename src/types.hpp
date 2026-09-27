#pragma once

#include <cstdint>
#include <limits>

namespace mex
{

enum class Side : std::uint8_t
{
  kBuy,
  kSell,
};

enum class SubmitStatus : std::uint8_t
{
  kAccepted,
  kRejected,
};

using OrderId = std::uint32_t;

constexpr OrderId kInvalidOrderId = std::numeric_limits<OrderId>::max();

// TODO: Replace std::int64_t prices with a strong Price type, in integer ticks, so a price cannot
// be passed as a quantity or another int64.
// TODO: Replace std::uint32_t order sizes with a strong Quantity type so a quantity cannot be
// passed as a price, an order count, or a pool index. Level total_qty is a uint64 sum of those
// quantities and should stay wide enough to hold that sum.

struct SubmitResult
{
  SubmitStatus status = SubmitStatus::kRejected;
  std::uint32_t filled_qty = 0;
  std::uint32_t remaining = 0;
  OrderId order_id = kInvalidOrderId;
};

} // namespace mex
