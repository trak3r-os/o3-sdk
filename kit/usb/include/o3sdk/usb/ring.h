// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace o3sdk::usb {

// Overwrite ring: lock-free single-producer / single-consumer queue whose producer NEVER waits and never
// fails. When the consumer falls behind, the producer overwrites the oldest entries; the consumer notices on
// its next read, skips what was lost and counts it. This is the overrun policy of ADR 0012 ("overrun drops the
// oldest block and counts it; the audio ISR never waits for USB"), and it is why this is not core::SpscRing:
// that ring refuses the NEW item when full, and dropping the OLD one from the producer side would need the
// producer to write the consumer's index (two writers).
//
// Contexts. Exactly one context calls the producer side (write) and exactly one the consumer side (read,
// flush, size, dropped). Typical: stems = audio ISR writes, USB ISR reads; return = USB ISR writes, engine
// pull reads. Either may preempt the other (single core, ISRs at different priorities) or run on another core:
// the orderings below are the ones a weakly ordered multi-core needs; on the single-core M4 the two fences are
// one `dmb` each, in write() and read().
//
// Protocol. `head_` counts items ever written and is written only by the producer; `tail_` counts items ever
// read and is written only by the consumer (single writer per index, free-running uint32_t counters,
// slot = counter & (N-1)).
//   write: release fence (the previous publish of head_ is ordered before these slot stores: without it a
//          weakly ordered core may show the new slot contents before the old head, and the consumer's re-check
//          would then miss the overwrite); store the items into slots head..head+n-1; head_ = head + n (release).
//   read:  tail = tail_; head = head_ (acquire); avail = head - tail.
//          avail > capacity(): the producer lapped us, entries older than head - capacity() are gone;
//          skip them (lost = avail - capacity(), added to dropped() at commit).
//          Copy the items, then re-read head_: if head - tail > capacity() the producer may have started
//          overwriting the oldest slot we copied, so the copy is discarded and the read starts again (bounded
//          at kReadAttempts; a read that cannot settle returns 0 like an empty ring).
//          Otherwise tail_ = tail + taken (release).
// A chunk of at most MaxChunk items is written per write(), so the producer is never more than MaxChunk-1
// items beyond the published head: that is why capacity() = N - MaxChunk, not N.
//
// Slots are arrays of relaxed std::atomic<uint32_t> words (a plain ldr/str on ARMv7-M), so a copy that races
// with an overwrite is well defined and simply fails the validation above; no data race in the C++ sense.
// T must be trivially copyable and a multiple of 4 bytes.
template <typename T, std::uint32_t N, std::uint32_t MaxChunk = 1>
class OverwriteRing {
  static_assert(N >= 2u && (N & (N - 1u)) == 0u && N <= 0x80000000u, "N must be a power of two");
  static_assert(MaxChunk >= 1u && MaxChunk < N, "MaxChunk must be in 1..N-1");
  static_assert(std::is_trivially_copyable<T>::value, "items are copied word by word");
  static_assert(sizeof(T) % 4u == 0u, "items are copied in 32-bit words");
  static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "needs lock-free 32-bit atomics");

 public:
  OverwriteRing() = default;
  // TEST-ONLY argument: starts both free-running counters at `start_index` (an empty ring stays empty), so a
  // test can cross the 2^32 wrap. Production code uses the default.
  explicit OverwriteRing(std::uint32_t start_index) noexcept : head_(start_index), tail_(start_index) {}
  OverwriteRing(const OverwriteRing&) = delete;
  OverwriteRing& operator=(const OverwriteRing&) = delete;

  // Items a reader can rely on: older ones are overwritten.
  static constexpr std::uint32_t capacity() noexcept { return N - MaxChunk; }
  static constexpr std::uint32_t max_chunk() noexcept { return MaxChunk; }

  // Producer. Writes n items (n is limited to MaxChunk). Wait-free, O(n).
  void write(const T* src, std::uint32_t n) noexcept {
    if (n > MaxChunk) {
      n = MaxChunk;
    }
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
#if !defined(__SANITIZE_THREAD__)
    std::atomic_thread_fence(std::memory_order_release);  // orders the previous head_ publish before the stores below
#endif
    for (std::uint32_t i = 0; i < n; ++i) {
      store_slot((head + i) & kMask, src[i]);
    }
    head_.store(head + n, std::memory_order_release);
  }

  // Consumer. Copies up to max_n of the oldest items into dst and returns how many (0 when empty).
  std::uint32_t read(T* dst, std::uint32_t max_n) noexcept {
    for (std::uint32_t attempt = 0; attempt < kReadAttempts; ++attempt) {
      std::uint32_t tail = tail_.load(std::memory_order_relaxed);
      const std::uint32_t head = head_.load(std::memory_order_acquire);
      std::uint32_t avail = head - tail;
      std::uint32_t lost = 0;
      if (avail > capacity()) {
        lost = avail - capacity();
        tail += lost;
        avail = capacity();
      }
      const std::uint32_t take = avail < max_n ? avail : max_n;
      if (take == 0) {
        if (lost != 0) {  // nothing readable, but the lap is real: commit the skip
          tail_.store(tail, std::memory_order_release);
          dropped_.store(dropped_.load(std::memory_order_relaxed) + lost, std::memory_order_relaxed);
        }
        return 0;
      }
      for (std::uint32_t i = 0; i < take; ++i) {
        load_slot((tail + i) & kMask, dst[i]);
      }
#if !defined(__SANITIZE_THREAD__)
      std::atomic_thread_fence(std::memory_order_acquire);  // the copy above is ordered before the re-check below
#endif
      if (head_.load(std::memory_order_relaxed) - tail > capacity()) {
        continue;  // the oldest slot may have been overwritten while we copied: copy again
      }
      tail_.store(tail + take, std::memory_order_release);
      if (lost != 0) {
        dropped_.store(dropped_.load(std::memory_order_relaxed) + lost, std::memory_order_relaxed);
      }
      return take;
    }
    return 0;
  }

  // Consumer: readable items (0..capacity()).
  std::uint32_t size() const noexcept {
    const std::uint32_t avail = head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    return avail < capacity() ? avail : capacity();
  }

  // Producer: items written and not yet read, an upper bound (the consumer may read right after); 0..capacity().
  std::uint32_t size_from_producer() const noexcept {
    const std::uint32_t avail = head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire);
    return avail < capacity() ? avail : capacity();
  }

  // Consumer: discards everything readable now (a stream start). Not counted as dropped.
  void flush() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

  // Neither side running (before start, between tests): back to the state of a new ring. Never on a ring in use. The slots keep their words:
  // nothing below head_ is readable.
  void reset() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

  // Consumer-owned running total of items lost to overwriting, counted when the consumer notices. Any
  // context may read it (one aligned 32-bit word).
  std::uint32_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  static constexpr std::uint32_t kReadAttempts = 3;

 private:
  static constexpr std::uint32_t kMask = N - 1u;
  static constexpr std::uint32_t kWords = sizeof(T) / 4u;

  void store_slot(std::uint32_t slot, const T& item) noexcept {
    std::uint32_t w[kWords];
    std::memcpy(w, &item, sizeof(T));
    for (std::uint32_t k = 0; k < kWords; ++k) {
      words_[slot * kWords + k].store(w[k], kSlotStore);
    }
  }
  // ThreadSanitizer does not model std::atomic_thread_fence (and GCC warns about it), so under TSan the slot
  // loads are acquire loads and the slot stores release stores, which gives the same orderings as the fences in
  // read() and write().
#if defined(__SANITIZE_THREAD__)
  static constexpr std::memory_order kSlotLoad = std::memory_order_acquire;
  static constexpr std::memory_order kSlotStore = std::memory_order_release;
#else
  static constexpr std::memory_order kSlotLoad = std::memory_order_relaxed;
  static constexpr std::memory_order kSlotStore = std::memory_order_relaxed;
#endif
  void load_slot(std::uint32_t slot, T& item) const noexcept {
    std::uint32_t w[kWords];
    for (std::uint32_t k = 0; k < kWords; ++k) {
      w[k] = words_[slot * kWords + k].load(kSlotLoad);
    }
    std::memcpy(&item, w, sizeof(T));
  }

  std::atomic<std::uint32_t> head_{0};     // written by the producer only
  std::atomic<std::uint32_t> tail_{0};     // written by the consumer only
  std::atomic<std::uint32_t> dropped_{0};  // written by the consumer only
  std::atomic<std::uint32_t> words_[N * kWords]{};
};

}  // namespace o3sdk::usb
