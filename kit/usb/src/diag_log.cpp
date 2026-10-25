// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/usb/diag_log.h>

namespace o3sdk::usb::diag {
namespace {

// ThreadSanitizer does not model std::atomic_thread_fence, so under it the word accesses carry the ordering themselves (as in ring.h).
#if defined(__SANITIZE_THREAD__)
constexpr std::memory_order kWordStore = std::memory_order_release;
constexpr std::memory_order kWordLoad = std::memory_order_acquire;
#else
constexpr std::memory_order kWordStore = std::memory_order_relaxed;
constexpr std::memory_order kWordLoad = std::memory_order_relaxed;
#endif

void release_fence() noexcept {
#if !defined(__SANITIZE_THREAD__)
  std::atomic_thread_fence(std::memory_order_release);
#endif
}
void acquire_fence() noexcept {
#if !defined(__SANITIZE_THREAD__)
  std::atomic_thread_fence(std::memory_order_acquire);
#endif
}

}  // namespace

void LogRing::init(LogRecord* slots, std::uint32_t count) noexcept {
  if (slots == nullptr || count < 2u || count > (1u << 20) || (count & (count - 1u)) != 0u) {
    return;  // refused: the ring stays not ready
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    slots[i].seq.store(0, std::memory_order_relaxed);
    slots[i].ms.store(0, std::memory_order_relaxed);
    for (std::size_t k = 0; k < kLogTextWords; ++k) {
      slots[i].text[k].store(0, std::memory_order_relaxed);
    }
  }
  mask_ = count - 1u;
  head_.store(0, std::memory_order_relaxed);
  release_fence();
  slots_ = slots;
}

void LogRing::write(std::uint32_t ms, const char* text) noexcept {
  if (slots_ == nullptr) {
    return;
  }
  // The text is packed first (a cut copy, zero filled), outside the slot, so the claim-to-publish window is a handful of stores.
  std::uint32_t words[kLogTextWords] = {};
  for (std::size_t i = 0; i < kLogTextMax && text[i] != '\0'; ++i) {
    words[i / 4u] |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(text[i])) << (8u * (i % 4u));
  }
  const std::uint32_t n = head_.fetch_add(1u, std::memory_order_relaxed);
  LogRecord& r = slots_[n & mask_];
  r.seq.store(0, std::memory_order_relaxed);  // being rewritten: a reader must not trust the old contents
  release_fence();
  r.ms.store(ms, kWordStore);
  for (std::size_t k = 0; k < kLogTextWords; ++k) {
    r.text[k].store(words[k], kWordStore);
  }
  r.seq.store(n + 1u, std::memory_order_release);
}

bool LogRing::read(std::uint32_t sequence, LogEntry& out) const noexcept {
  if (slots_ == nullptr) {
    return false;
  }
  const std::uint32_t head = head_.load(std::memory_order_acquire);
  if (sequence >= head || head - sequence > capacity()) {
    return false;
  }
  const LogRecord& r = slots_[sequence & mask_];
  if (r.seq.load(std::memory_order_acquire) != sequence + 1u) {
    return false;
  }
  out.sequence = sequence;
  out.ms = r.ms.load(kWordLoad);
  for (std::size_t k = 0; k < kLogTextWords; ++k) {
    const std::uint32_t w = r.text[k].load(kWordLoad);
    for (unsigned b = 0; b < 4u; ++b) {
      out.text[4u * k + b] = static_cast<char>((w >> (8u * b)) & 0xFFu);
    }
  }
  out.text[kLogTextMax] = '\0';
  acquire_fence();
  return r.seq.load(std::memory_order_relaxed) == sequence + 1u;  // unchanged while we copied
}

}  // namespace o3sdk::usb::diag
