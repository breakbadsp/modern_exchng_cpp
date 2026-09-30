#include "book.hpp"

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <new>
#include <vector>

namespace
{

std::uint64_t g_new_calls = 0;

} // namespace

void *operator new(std::size_t p_size)
{
  ++g_new_calls;
  void *const memory = std::malloc(p_size == 0 ? 1 : p_size);
  if (memory == nullptr)
  {
    std::abort();
  }
  return memory;
}

void *operator new[](std::size_t p_size) { return ::operator new(p_size); }

void operator delete(void *p_memory) noexcept { std::free(p_memory); }

void operator delete(void *p_memory, std::size_t) noexcept { std::free(p_memory); }

void operator delete[](void *p_memory) noexcept { ::operator delete(p_memory); }

void operator delete[](void *p_memory, std::size_t p_size) noexcept
{
  ::operator delete(p_memory, p_size);
}

TEST(Alloc, MixedSubmitAndCancelDoNotHeapAllocate)
{
  constexpr std::uint32_t kCapacity = 1024;
  constexpr std::uint32_t kLevels = 64;
  constexpr std::uint32_t kOps = 200000;
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

  const std::uint64_t news_before = g_new_calls;
  std::uint32_t failures = 0;
  for (std::uint32_t i = 0; i < kOps; ++i)
  {
    const bool full = live.size() >= kCapacity;
    const bool cancel = !live.empty() && (full || (next() % 5U) == 0U);
    if (cancel)
    {
      const std::size_t index = static_cast<std::size_t>(next() % live.size());
      if (!book.CancelOrder(live[index]))
      {
        ++failures;
      }
      live[index] = live.back();
      live.pop_back();
      continue;
    }

    const mex::Price price{static_cast<std::int64_t>(next() % kLevels)};
    const mex::SubmitResult result = book.SubmitLimitOrder(mex::Side::Buy, price, mex::Quantity{1});
    if (result.order_id_ == mex::kInvalidOrderId)
    {
      ++failures;
      continue;
    }
    live.push_back(result.order_id_);
  }
  const std::uint64_t news_after = g_new_calls;
  EXPECT_EQ(failures, 0u);
  EXPECT_EQ(news_after, news_before);
}
