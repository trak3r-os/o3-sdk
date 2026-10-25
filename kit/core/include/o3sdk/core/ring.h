// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace o3sdk::core {

// Lock-free single-producer / single-consumer ring buffer of N items (N a power of two, all N usable).
// No heap, no locks, no blocking: push() and pop() are wait-free and O(1), so either side may run in an ISR.
//
// Contexts. Exactly ONE execution context may call the producer side (push) and exactly ONE the consumer
// side (pop, empty). Typical use: an ISR (or any single interrupt priority) pushes, the main loop pops.
// The reverse direction (main loop pushes, ISR pops) is equally valid. Two ISRs at different priorities
// that both push are two producers and are NOT allowed; neither is re-entering one side from a higher
// priority interrupt. Break these rules and items are lost or duplicated.
//
// Protocol and memory ordering. `head_` counts items ever pushed and is written only by the producer;
// `tail_` counts items ever popped and is written only by the consumer (single writer per index). Both
// are free-running uint32_t counters (they wrap at 2^32, a multiple of N, so `head - tail` is always the
// fill level and slot = counter & (N - 1)).
//   push: load head (relaxed, own index); load tail (acquire); if full return false; write the slot;
//         store head + 1 (release).
//   pop:  load tail (relaxed, own index); load head (acquire); if empty return false; read the slot;
//         store tail + 1 (release).
// The release store of an index publishes the slot access made before it; the other side's acquire load of
// that index makes the access visible before it touches the slot. So the consumer never reads a slot the
// producer is still writing, and the producer never overwrites a slot the consumer is still reading.
// `buf_` is plain (non-volatile) memory: ordering comes from the atomics alone.
//
// Cortex-M4 (ARMv7-M): 32-bit atomics are lock-free and inline (ldr/str plus dmb for acquire/release), so
// no libatomic is needed; the static_assert below turns any target where that is false into a build error.
template <typename T, std::uint32_t N>
class SpscRing {
  static_assert(N >= 2u && (N & (N - 1u)) == 0u && N <= 0x80000000u,
                "SpscRing capacity must be a power of two between 2 and 2^31");
  static_assert(std::is_trivially_copyable<T>::value, "SpscRing items are copied bytewise inside the ISR");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                "SpscRing needs lock-free 32-bit atomics (no libatomic on the target)");

 public:
  // TEST-ONLY argument: `start_index` just moves the free-running counters (an empty ring stays empty) so
  // a test can start near 2^32 and exercise the counter wrap. Production code uses the default.
  constexpr explicit SpscRing(std::uint32_t start_index = 0) noexcept : head_(start_index), tail_(start_index) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  static constexpr std::uint32_t capacity() noexcept { return N; }

  // Producer side. Returns false (item not stored) when the ring is full.
  [[nodiscard]] bool push(const T& item) noexcept {
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= N) {
      return false;
    }
    buf_[head & kMask] = item;
    head_.store(head + 1u, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false (`out` untouched) when the ring is empty.
  [[nodiscard]] bool pop(T& out) noexcept {
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    out = buf_[tail & kMask];
    tail_.store(tail + 1u, std::memory_order_release);
    return true;
  }

  // Snapshot of the fill level. Called by the consumer it is a lower bound (the producer may add more
  // right after); called by the producer it is an upper bound (the consumer may drain right after).
  // In both of those cases the result is in 0..N. From any OTHER context the bound does not hold: head and
  // tail are read at different instants, so the pair may never have been true together and the result can
  // exceed N. What is guaranteed everywhere is that it cannot underflow: `tail` is read before `head`, and
  // head never falls behind a tail read earlier.
  std::uint32_t size() const noexcept {
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    return head - tail;
  }

  // Consumer-side emptiness check (a true result may be stale by the next instruction).
  bool empty() const noexcept { return size() == 0u; }

 private:
  static constexpr std::uint32_t kMask = N - 1u;

  std::atomic<std::uint32_t> head_;  // written by the producer only
  std::atomic<std::uint32_t> tail_;  // written by the consumer only
  T buf_[N]{};
};

}  // namespace o3sdk::core
