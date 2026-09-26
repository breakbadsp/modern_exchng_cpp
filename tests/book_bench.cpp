#include "book.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

void Report(std::string_view name, std::uint64_t operations, Clock::duration elapsed)
{
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double per_second = static_cast<double>(operations) / seconds;
    const double nanos = seconds * 1.0e9 / static_cast<double>(operations);
    std::cout << name << "  " << static_cast<std::uint64_t>(per_second) << " ops/s  " << nanos << " ns/op\n";
}

[[nodiscard]] Clock::duration Time(auto&& function)
{
    const Clock::time_point start = Clock::now();
    function();
    return Clock::now() - start;
}

struct NoFill
{
    void operator()(mex::OrderId, std::int64_t, std::uint32_t) const
    {
    }
};

void BenchAppendToOneLevel()
{
    constexpr std::uint32_t kOrders = 200000;
    mex::Book book(kOrders, 4);
    const Clock::duration elapsed = Time([&]()
    {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
            const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kBuy, 100, 1, NoFill{});
            if (result.status != mex::SubmitStatus::kAccepted)
            {
                std::cerr << "append rejected\n";
                std::exit(1);
            }
        }
    });
    Report("append one price level", kOrders, elapsed);
}

void BenchRestAcrossLevels()
{
    constexpr std::uint32_t kOrders = 200000;
    constexpr std::uint32_t kLevels = 64;
    mex::Book book(kOrders, kLevels);
    const Clock::duration elapsed = Time([&]()
    {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
            const std::int64_t price = static_cast<std::int64_t>(i % kLevels);
            const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kBuy, price, 1, NoFill{});
            if (result.status != mex::SubmitStatus::kAccepted)
            {
                std::cerr << "rest rejected\n";
                std::exit(1);
            }
        }
    });
    Report("rest across 64 levels", kOrders, elapsed);
}

void BenchCancel()
{
    constexpr std::uint32_t kOrders = 100000;
    mex::Book book(kOrders, 32);
    std::vector<mex::OrderId> ids;
    ids.reserve(kOrders);
    for (std::uint32_t i = 0; i < kOrders; ++i)
    {
        const std::int64_t price = static_cast<std::int64_t>(i % 32);
        const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kSell, 1000 + price, 1, NoFill{});
        ids.push_back(result.order_id);
    }

    std::uint64_t state = 0x123456789abcdefULL;
    auto next = [&state]()
    {
        state ^= state << 7U;
        state ^= state >> 9U;
        state ^= state << 8U;
        return state;
    };
    for (std::uint32_t i = kOrders - 1; i > 0; --i)
    {
        const std::uint32_t swap_with = static_cast<std::uint32_t>(next() % (static_cast<std::uint64_t>(i) + 1));
        std::swap(ids[i], ids[swap_with]);
    }

    const Clock::duration elapsed = Time([&]()
    {
        for (const mex::OrderId id : ids)
        {
            if (!book.CancelOrder(id))
            {
                std::cerr << "cancel failed\n";
                std::exit(1);
            }
        }
    });
    Report("cancel random live orders", kOrders, elapsed);
}

void BenchTakeBest()
{
    constexpr std::uint32_t kOrders = 100000;
    constexpr std::uint32_t kLevels = 32;
    mex::Book book(kOrders, kLevels);
    for (std::uint32_t i = 0; i < kOrders; ++i)
    {
        const std::int64_t price = 10 + static_cast<std::int64_t>(i % kLevels);
        const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kSell, price, 1, NoFill{});
        if (result.order_id == mex::kInvalidOrderId)
        {
            std::cerr << "seed ask rejected\n";
            std::exit(1);
        }
    }

    const Clock::duration elapsed = Time([&]()
    {
        for (std::uint32_t i = 0; i < kOrders; ++i)
        {
            const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kBuy, 1000, 1, NoFill{});
            if (result.filled_qty != 1 || result.order_id != mex::kInvalidOrderId)
            {
                std::cerr << "take did not fill one lot\n";
                std::exit(1);
            }
        }
    });
    Report("take one lot from the touch", kOrders, elapsed);
}

void BenchMixedBook()
{
    constexpr std::uint32_t kCapacity = 100000;
    constexpr std::uint32_t kLevels = 64;
    constexpr std::uint32_t kOps = 300000;
    mex::Book book(kCapacity, kLevels);
    std::vector<mex::OrderId> live;
    live.reserve(kCapacity);

    std::uint64_t state = 0xcafef00d1234ULL;
    auto next = [&state]()
    {
        state ^= state << 7U;
        state ^= state >> 9U;
        state ^= state << 8U;
        return state;
    };

    const Clock::duration elapsed = Time([&]()
    {
        for (std::uint32_t i = 0; i < kOps; ++i)
        {
            const bool cancel = !live.empty() && (next() % 5U) == 0U;
            if (cancel)
            {
                const std::size_t index = static_cast<std::size_t>(next() % live.size());
                if (!book.CancelOrder(live[index]))
                {
                    std::cerr << "mixed cancel failed\n";
                    std::exit(1);
                }
                live[index] = live.back();
                live.pop_back();
                continue;
            }

            if (live.size() >= kCapacity)
            {
                continue;
            }
            const std::int64_t price = static_cast<std::int64_t>(next() % kLevels);
            const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::kBuy, price, 1, NoFill{});
            if (result.order_id == mex::kInvalidOrderId)
            {
                std::cerr << "mixed insert rejected\n";
                std::exit(1);
            }
            live.push_back(result.order_id);
        }
    });
    Report("mixed insert and cancel", kOps, elapsed);
}

}  // namespace

int main()
{
    std::cout << "Order book benchmark, compiled optimized\n";
    BenchAppendToOneLevel();
    BenchRestAcrossLevels();
    BenchCancel();
    BenchTakeBest();
    BenchMixedBook();
    std::cout << "Target from the design spec: 100000 to 500000 orders/s\n";
    return 0;
}
