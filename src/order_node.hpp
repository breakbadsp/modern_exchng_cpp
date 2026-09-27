#pragma once

#include "types.hpp"

#include <cstdint>

namespace mex
{

// A resting limit order. The pool index is the order id, so it is not stored again.
struct OrderNode
{
  std::int64_t price = 0;
  std::uint32_t quantity = 0;
  OrderId prev = kInvalidOrderId;
  OrderId next = kInvalidOrderId;
  Side side = Side::kBuy;
};

} // namespace mex
