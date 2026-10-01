#include "book.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <new>
#include <vector>

namespace
{

std::atomic<bool> g_count_news{false};
std::atomic<std::uint64_t> g_new_calls{0};

void NoteNew()
{
  if (g_count_news.load(std::memory_order_relaxed))
  {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
  }
}

} // namespace

void *operator new(std::size_t p_size)
{
  NoteNew();
  if (p_size == 0)
  {
    p_size = 1;
  }
  void *memory = std::malloc(p_size);
  if (memory == nullptr)
  {
    std::abort();
  }
  return memory;
}

void *operator new(std::size_t p_size, const std::nothrow_t &) noexcept
{
  NoteNew();
  if (p_size == 0)
  {
    p_size = 1;
  }
  return std::malloc(p_size);
}

void *operator new(std::size_t p_size, std::align_val_t p_alignment)
{
  NoteNew();
  if (p_size == 0)
  {
    p_size = 1;
  }
  void *memory = nullptr;
  if (posix_memalign(&memory, static_cast<std::size_t>(p_alignment), p_size) != 0)
  {
    std::abort();
  }
  return memory;
}

void *operator new(std::size_t p_size, std::align_val_t p_alignment,
                   const std::nothrow_t &) noexcept
{
  NoteNew();
  if (p_size == 0)
  {
    p_size = 1;
  }
  void *memory = nullptr;
  if (posix_memalign(&memory, static_cast<std::size_t>(p_alignment), p_size) != 0)
  {
    return nullptr;
  }
  return memory;
}

void *operator new[](std::size_t p_size) { return ::operator new(p_size); }

void *operator new[](std::size_t p_size, const std::nothrow_t &p_tag) noexcept
{
  return ::operator new(p_size, p_tag);
}

void *operator new[](std::size_t p_size, std::align_val_t p_alignment)
{
  return ::operator new(p_size, p_alignment);
}

void *operator new[](std::size_t p_size, std::align_val_t p_alignment,
                     const std::nothrow_t &p_tag) noexcept
{
  return ::operator new(p_size, p_alignment, p_tag);
}

void operator delete(void *p_memory) noexcept { std::free(p_memory); }

void operator delete(void *p_memory, std::size_t) noexcept { std::free(p_memory); }

void operator delete(void *p_memory, std::align_val_t) noexcept { std::free(p_memory); }

void operator delete(void *p_memory, std::size_t, std::align_val_t) noexcept
{
  std::free(p_memory);
}

void operator delete(void *p_memory, const std::nothrow_t &) noexcept { std::free(p_memory); }

void operator delete(void *p_memory, std::align_val_t, const std::nothrow_t &) noexcept
{
  std::free(p_memory);
}

void operator delete[](void *p_memory) noexcept { std::free(p_memory); }

void operator delete[](void *p_memory, std::size_t) noexcept { std::free(p_memory); }

void operator delete[](void *p_memory, std::align_val_t p_alignment) noexcept
{
  ::operator delete(p_memory, p_alignment);
}

void operator delete[](void *p_memory, std::size_t, std::align_val_t p_alignment) noexcept
{
  ::operator delete(p_memory, p_alignment);
}

void operator delete[](void *p_memory, const std::nothrow_t &) noexcept { std::free(p_memory); }

void operator delete[](void *p_memory, std::align_val_t p_alignment,
                       const std::nothrow_t &) noexcept
{
  ::operator delete(p_memory, p_alignment);
}

TEST(AllocCount, TwoMillionMixedOpsDoNotCallOperatorNew)
{
  constexpr std::uint32_t kCapacity = 4096;
  constexpr std::uint32_t kLevels = 64;
  constexpr std::uint32_t kOps = 2000000;

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

  // Tracking which ids are live is the test's job, not the book's: a fill with
  // maker_removed_ frees a slot, and Rest may reuse that id in the same submit.
  // So the live list is rebuilt from what the book still reports as resting.
  std::uint64_t fills_seen = 0;
  std::uint64_t crossing_submits = 0;

  g_new_calls.store(0, std::memory_order_relaxed);
  g_count_news.store(true, std::memory_order_relaxed);

  for (std::uint32_t i = 0; i < kOps; ++i)
  {
    const std::uint64_t pick = next();
    const bool cancel = !live.empty() && (pick % 5U) == 0U;
    if (cancel)
    {
      const std::size_t index = static_cast<std::size_t>(next() % live.size());
      if (!book.CancelOrder(live[index]))
      {
        g_count_news.store(false, std::memory_order_relaxed);
        FAIL() << "cancel of a live order failed at op " << i;
      }
      live[index] = live.back();
      live.pop_back();
      continue;
    }

    const mex::Side side = (next() & 1U) == 0U ? mex::Side::Buy : mex::Side::Sell;
    mex::SubmitResult result;
    if ((pick % 11U) == 0U)
    {
      result =
          book.SubmitMarketOrder(side, mex::Quantity{static_cast<std::uint32_t>(1 + next() % 4U)});
    }
    else
    {
      // Prices 0..63 for both sides, so buys and sells cross regularly.
      const mex::Price price{static_cast<std::int64_t>(next() % kLevels)};
      result = book.SubmitLimitOrder(side, price,
                                     mex::Quantity{static_cast<std::uint32_t>(1 + next() % 4U)});
    }

    const std::size_t fills = book.Fills().size();
    fills_seen += fills;
    crossing_submits += fills > 0 ? 1U : 0U;

    // Makers removed by this submit are no longer live. Drop them without
    // allocating: swap-remove by value.
    for (const mex::Fill &fill : book.Fills())
    {
      if (!fill.maker_removed_)
      {
        continue;
      }
      for (std::size_t j = 0; j < live.size(); ++j)
      {
        // The new resting order (if any) may carry the same id as a removed
        // maker, and it is not in `live` yet, so any match here is the maker.
        if (live[j] == fill.maker_id_)
        {
          live[j] = live.back();
          live.pop_back();
          break;
        }
      }
    }

    if (result.order_id_ != mex::kInvalidOrderId)
    {
      live.push_back(result.order_id_);
    }
  }

  g_count_news.store(false, std::memory_order_relaxed);
  const std::uint64_t news = g_new_calls.load(std::memory_order_relaxed);
  EXPECT_EQ(news, 0u) << "operator new called " << news
                      << " times over 2M mixed operations after Book construction (expected 0)";
  // The workload must actually match, or this test says nothing about the fill buffer.
  EXPECT_GT(crossing_submits, std::uint64_t{100000});
  EXPECT_GT(fills_seen, std::uint64_t{100000});
}
