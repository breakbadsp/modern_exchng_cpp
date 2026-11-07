#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

namespace mex
{

// Bounded single-producer, single-consumer ring of fixed-size messages.
// Exactly one thread calls TryPush and exactly one other thread calls TryPop.
// The ring is allocated once in the constructor and never grows.
//
// tail_ and head_ only increase. A 64-bit index does not wrap in practice
// (2^64 messages), so used size is always tail_ - head_.
template <typename T>
  requires std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>
class SpscQueue final
{
public:
  explicit SpscQueue(std::size_t p_capacity) pre(std::has_single_bit(p_capacity))
      : mask_{p_capacity - 1}, slots_{std::make_unique<T[]>(p_capacity)}
  {
  }

  // Producer thread only. Returns false and changes nothing if the ring is full.
  [[nodiscard]] bool TryPush(const T &p_item)
  {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ > mask_)
    {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ > mask_)
      {
        return false;
      }
    }
    slots_[tail & mask_] = p_item;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Returns nullopt if the ring is empty.
  [[nodiscard]] std::optional<T> TryPop()
  {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_)
    {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_)
      {
        return std::nullopt;
      }
    }
    const T item = slots_[head & mask_];
    head_.store(head + 1, std::memory_order_release);
    return item;
  }

  [[nodiscard]] std::size_t Capacity() const { return mask_ + 1; }

private:
  // Not a std::hardware_destructive_interference_size: GCC warns that its value
  // can differ between translation units. 64 bytes is the x86-64 line size.
  static constexpr std::size_t kCacheLine = 64;

  // Written once in the constructor, then read by both threads.
  const std::size_t mask_;
  const std::unique_ptr<T[]> slots_;

  // Producer line: tail_ is written only by the producer; cached_head_ is the
  // producer's last read of head_, so a push reads head_ only when the ring looks full.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_ = 0;

  // Consumer line: the mirror of the producer line.
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_ = 0;
};

} // namespace mex
