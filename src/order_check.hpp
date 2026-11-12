#pragma once

#include "config.hpp"
#include "protocol.hpp"
#include "types.hpp"

#include <optional>

// Order checks that need no order state. The gateway runs them before it
// queues an order. The replicated state machine runs them again, because a
// committed operation can come from a process other than this gateway.
namespace mex
{

[[nodiscard]] constexpr bool InBand(const SymbolConfig &p_route, Price p_price)
{
  return p_route.min_price_ <= p_price && p_price <= p_route.max_price_;
}

// nullopt: the order may go to the engine.
[[nodiscard]] constexpr std::optional<wire::Reason> Check(const SymbolConfig &p_route,
                                                          const wire::NewOrder &p_order)
{
  if (p_order.qty_.units_ == 0)
  {
    return wire::Reason::InvalidQuantity;
  }
  if (p_order.type_ == wire::OrderType::Limit && !InBand(p_route, p_order.price_))
  {
    return wire::Reason::PriceOutOfRange;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<wire::Reason> Check(const SymbolConfig &,
                                                          const wire::Cancel &)
{
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<wire::Reason> Check(const SymbolConfig &p_route,
                                                          const wire::Replace &p_replace)
{
  if (p_replace.new_total_qty_.units_ == 0)
  {
    return wire::Reason::InvalidQuantity;
  }
  if (!InBand(p_route, p_replace.new_price_))
  {
    return wire::Reason::PriceOutOfRange;
  }
  return std::nullopt;
}

} // namespace mex
