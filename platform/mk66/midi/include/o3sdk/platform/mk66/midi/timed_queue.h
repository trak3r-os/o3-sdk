// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A bounded queue of items ordered by a 64-bit time key (OS-005c): the "due sample times go into a timed queue" of ADR 0013 D5. It is the
// staging area of one context (the render): items arrive in any order, leave in key order, and items with equal keys leave in the order
// they came in (stable). Not thread-safe; a context boundary is an SpscRing behind it (core/ring.h), which the render fills only with items
// that are about to be due, in order.
//
// No heap: N slots (a power of two) in a circular array. push is O(1) when the key is not earlier than the last one (the usual case) and
// otherwise shifts the later items by one slot; pop is O(1). Everything is bounded by N.

#include <cstdint>
#include <type_traits>

namespace o3sdk::platform::mk66::midi {

template <typename T, std::uint32_t N>
class TimedQueue {
  static_assert(N >= 2u && (N & (N - 1u)) == 0u && N <= 0x8000u, "TimedQueue capacity is a power of two between 2 and 32768");
  static_assert(std::is_trivially_copyable<T>::value, "TimedQueue items are moved bytewise");

 public:
  static constexpr std::uint32_t capacity() noexcept { return N; }

  std::uint32_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0u; }
  bool full() const noexcept { return size_ == N; }
  void clear() noexcept {
    head_ = 0;
    size_ = 0;
  }

  // False (nothing stored) when full.
  bool push(std::uint64_t key, const T& item) noexcept {
    if (size_ == N) {
      return false;
    }
    // Where it goes: after the last item whose key is <= `key`, found by scanning back from the end.
    std::uint32_t pos = size_;
    while (pos != 0u && slot(pos - 1u).key > key) {
      --pos;
    }
    for (std::uint32_t i = size_; i > pos; --i) {
      slot(i) = slot(i - 1u);
    }
    slot(pos).key = key;
    slot(pos).item = item;
    ++size_;
    return true;
  }

  // The earliest item, nullptr when empty. Valid until the next push, pop or cancel.
  const T* front() const noexcept { return size_ != 0u ? &slots_[head_].item : nullptr; }
  std::uint64_t front_key() const noexcept { return slots_[head_].key; }  // only when !empty()

  void pop() noexcept {
    if (size_ != 0u) {
      head_ = (head_ + 1u) & (N - 1u);
      --size_;
    }
  }

  // Removes every item whose key is at least `from_key` and for which pred(item) is true, keeping the order of the rest. Returns how many
  // went. (The sequencer drops the pending ticks of a transport that stopped.)
  template <typename Pred>
  std::uint32_t cancel_if(std::uint64_t from_key, Pred pred) noexcept {
    std::uint32_t kept = 0;
    for (std::uint32_t i = 0; i < size_; ++i) {
      if (slot(i).key >= from_key && pred(slot(i).item)) {
        continue;
      }
      if (kept != i) {
        slot(kept) = slot(i);
      }
      ++kept;
    }
    const std::uint32_t removed = size_ - kept;
    size_ = kept;
    return removed;
  }

 private:
  struct Slot {
    std::uint64_t key;
    T item;
  };
  Slot& slot(std::uint32_t i) noexcept { return slots_[(head_ + i) & (N - 1u)]; }
  const Slot& slot(std::uint32_t i) const noexcept { return slots_[(head_ + i) & (N - 1u)]; }

  Slot slots_[N]{};
  std::uint32_t head_ = 0;
  std::uint32_t size_ = 0;
};

}  // namespace o3sdk::platform::mk66::midi
