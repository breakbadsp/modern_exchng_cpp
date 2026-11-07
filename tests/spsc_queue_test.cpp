#include "spsc_queue.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <optional>
#include <thread>

namespace
{

using mex::SpscQueue;

TEST(SpscQueueTest, EmptyQueuePopsNothing)
{
  SpscQueue<std::uint64_t> queue{4};

  EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(SpscQueueTest, FullQueueRejectsPushAndKeepsContents)
{
  SpscQueue<std::uint64_t> queue{4};
  for (std::uint64_t i = 0; i < 4; ++i)
  {
    ASSERT_TRUE(queue.TryPush(i));
  }

  EXPECT_FALSE(queue.TryPush(99));

  for (std::uint64_t i = 0; i < 4; ++i)
  {
    const std::optional<std::uint64_t> item = queue.TryPop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, i);
  }
  EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(SpscQueueTest, PopFreesSpaceForOnePush)
{
  SpscQueue<std::uint64_t> queue{2};
  ASSERT_TRUE(queue.TryPush(1));
  ASSERT_TRUE(queue.TryPush(2));
  ASSERT_FALSE(queue.TryPush(3));

  ASSERT_TRUE(queue.TryPop().has_value());

  EXPECT_TRUE(queue.TryPush(3));
  EXPECT_FALSE(queue.TryPush(4));
}

TEST(SpscQueueTest, KeepsFifoOrderAcrossManyWraps)
{
  constexpr std::size_t kCapacity = 8;
  SpscQueue<std::uint64_t> queue{kCapacity};
  std::uint64_t next_push = 0;
  std::uint64_t next_pop = 0;

  // Push 5, pop 3 per round, so the fill level and the slot offset both move.
  for (int round = 0; round < 1000; ++round)
  {
    for (int i = 0; i < 5; ++i)
    {
      if (queue.TryPush(next_push))
      {
        ++next_push;
      }
    }
    for (int i = 0; i < 3; ++i)
    {
      const std::optional<std::uint64_t> item = queue.TryPop();
      ASSERT_TRUE(item.has_value());
      ASSERT_EQ(*item, next_pop);
      ++next_pop;
    }
  }
  EXPECT_GT(next_pop, kCapacity * 100);
}

TEST(SpscQueueTest, CapacityIsTheConstructorValue)
{
  const SpscQueue<std::uint64_t> queue{1024};

  EXPECT_EQ(queue.Capacity(), 1024U);
}

// A message larger than one word, so a torn copy shows as a field mismatch.
struct Message
{
  std::uint64_t seq_ = 0;
  std::uint64_t check_ = 0;
  std::uint64_t pad_[2]{};
};

constexpr std::uint64_t CheckOf(std::uint64_t p_seq) { return ~p_seq * 0x9E3779B97F4A7C15ULL; }

// One producer and one consumer thread. The consumer must see every message,
// once, in order, with intact contents. A small ring forces many full and
// empty transitions.
void RunTwoThreadStress(std::size_t p_capacity, std::uint64_t p_count)
{
  SpscQueue<Message> queue{p_capacity};

  std::jthread producer{[&queue, p_count]
                        {
                          for (std::uint64_t seq = 0; seq < p_count; ++seq)
                          {
                            const Message message{.seq_ = seq, .check_ = CheckOf(seq)};
                            while (!queue.TryPush(message))
                            {
                              std::this_thread::yield();
                            }
                          }
                        }};

  std::uint64_t expected = 0;
  std::uint64_t mismatches = 0;
  while (expected < p_count)
  {
    const std::optional<Message> message = queue.TryPop();
    if (!message.has_value())
    {
      std::this_thread::yield();
      continue;
    }
    if (message->seq_ != expected || message->check_ != CheckOf(expected))
    {
      ++mismatches;
    }
    ++expected;
  }
  producer.join();

  EXPECT_EQ(mismatches, 0U);
  EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(SpscQueueTest, TwoThreadsTinyRing) { RunTwoThreadStress(2, 200'000); }

TEST(SpscQueueTest, TwoThreadsLargeRing) { RunTwoThreadStress(1024, 2'000'000); }

} // namespace
