#pragma once

#include "types.hpp"

#include <cstdint>

namespace mex
{

// One price on one side. Orders at this price form a FIFO list in the order pool.
struct PriceLevel
{
    std::int64_t price = 0;
    OrderId head = kInvalidOrderId;
    OrderId tail = kInvalidOrderId;
    std::uint32_t order_count = 0;
    std::uint64_t total_qty = 0;
};

}  // namespace mex
