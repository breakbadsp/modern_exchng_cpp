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
class Book
{
public:
    explicit Book(std::uint32_t max_orders, std::uint32_t max_price_levels);

    template <typename OnFill>
    [[nodiscard]] SubmitResult SubmitLimitOrder(Side side,
                                                 std::int64_t price,
                                                 std::uint32_t quantity,
                                                 OnFill&& on_fill);

    template <typename OnFill>
    [[nodiscard]] SubmitResult SubmitMarketOrder(Side side,
                                                  std::uint32_t quantity,
                                                  OnFill&& on_fill);

    [[nodiscard]] bool CancelOrder(OrderId order_id);

    [[nodiscard]] const OrderNode& Order(OrderId order_id) const;

    [[nodiscard]] const std::vector<PriceLevel>& Bids() const;
    [[nodiscard]] const std::vector<PriceLevel>& Asks() const;

    [[nodiscard]] std::uint32_t FreeSlotCount() const;
    [[nodiscard]] std::uint32_t MaxOrders() const;

private:
    using FillCallback = void (*)(OrderId maker_id,
                                  std::int64_t price,
                                  std::uint32_t quantity,
                                  void* context);

    [[nodiscard]] SubmitResult SubmitLimit(Side side,
                                            std::int64_t price,
                                            std::uint32_t quantity,
                                            FillCallback on_fill,
                                            void* context);

    [[nodiscard]] SubmitResult SubmitMarket(Side side,
                                             std::uint32_t quantity,
                                             FillCallback on_fill,
                                             void* context);

    // limit_price is absent for a market order, which crosses every level.
    [[nodiscard]] std::uint32_t Match(Side aggressor,
                                      std::optional<std::int64_t> limit_price,
                                      std::uint32_t quantity,
                                      FillCallback on_fill,
                                      void* context);

    [[nodiscard]] bool CanRest(Side side, std::int64_t price) const;

    OrderId Rest(Side side, std::int64_t price, std::uint32_t quantity);

    void Unlink(PriceLevel& level, OrderId order_id);
    void Release(OrderId order_id);

    [[nodiscard]] std::vector<PriceLevel>& Levels(Side side);
    [[nodiscard]] const std::vector<PriceLevel>& Levels(Side side) const;

    std::vector<OrderNode> pool_;
    std::vector<OrderId> free_list_;
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    std::uint32_t max_price_levels_;
};

template <typename OnFill>
SubmitResult Book::SubmitLimitOrder(Side side,
                                    std::int64_t price,
                                    std::uint32_t quantity,
                                    OnFill&& on_fill)
{
    using Callback = std::remove_reference_t<OnFill>;
    auto thunk = [](OrderId maker_id, std::int64_t fill_price, std::uint32_t fill_qty, void* context)
    {
        (*static_cast<Callback*>(context))(maker_id, fill_price, fill_qty);
    };
    return SubmitLimit(side, price, quantity, thunk, &on_fill);
}

template <typename OnFill>
SubmitResult Book::SubmitMarketOrder(Side side, std::uint32_t quantity, OnFill&& on_fill)
{
    using Callback = std::remove_reference_t<OnFill>;
    auto thunk = [](OrderId maker_id, std::int64_t fill_price, std::uint32_t fill_qty, void* context)
    {
        (*static_cast<Callback*>(context))(maker_id, fill_price, fill_qty);
    };
    return SubmitMarket(side, quantity, thunk, &on_fill);
}

}  // namespace mex
