#pragma once

#include "types.hpp"

#include <cstdint>

namespace mex
{

// A resting limit order. The pool index is the order id, so it is not stored again.
struct OrderNode
{
  std::int64_t price_ = 0;
  std::uint32_t quantity_ = 0;
  OrderId prev_ = kInvalidOrderId;
  OrderId next_ = kInvalidOrderId;
  Side side_ = Side::Buy;
};

} // namespace mex
