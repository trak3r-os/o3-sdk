// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Debounce and publishing state of up to 64 keys (OS-007d): a pure state machine, no clock of its own, no heap.
//
// Raw edges come from a chip (`edge`). A key's state is kept three times: RAW (what the chip last said), ACCEPTED (what the rest of
// the system is to believe) and PUBLISHED (what the consumer has actually been told, through a queue that may be full).
//
// Lockout debounce: a change of RAW is accepted by `service()` at once when the key has been quiet for the lockout, otherwise it
// waits for the quiet time to pass. So the first edge of a press costs no latency, a bounce inside the lockout is absorbed (RAW
// comes back to ACCEPTED and nothing was accepted), and a release that follows a press by less than the lockout is delayed by the
// remainder, never lost. A raw edge that repeats the state (a press of a key already down) is a consistency error and is counted.
//
// ORDER. Edges are published in the order the chip reported them, not in key order: Shift pressed before F8 must reach the gesture layer
// first, or Shift + F8 (redo) arrives as a plain F8 (undo). Each raw edge takes the next number of a counter; the number of the edge
// that was accepted travels with the accepted state, and the oldest unpublished one is offered first. (The counters are 16 bits and
// compare modulo 65,536: right while the unpublished edges lie within 32,768 raw edges of each other, which a consumer that has
// stopped for half a minute of continuous chatter would be the first to break, and then only the order, never a state.)
//
// Publishing is by comparison, not by a list of edges: the keys whose ACCEPTED and PUBLISHED differ are exactly the edges still to
// be told. A queue that refuses an event therefore loses nothing, the edge is offered again on the next pass; and a press and its
// release both refused collapse to nothing (the consumer never saw the key, and the two states agree). The consumer's picture
// always converges to the truth: a lost release can never leave a key (Shift) stuck down.

#include <o3sdk/core/tick.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input {

template <unsigned N>
class KeyFilter {
  static_assert(N >= 1u && N <= 64u, "a KeyFilter is a 64-bit mask of keys");

 public:
  explicit KeyFilter(std::uint32_t lockout_us) noexcept : lockout_(lockout_us) {
    for (core::Tick& t : changed_) {
      t = 0;
    }
  }

  // The chip says `key` is now down or up. A key out of range, or a state the key is already raw-in, is refused and counted.
  void edge(unsigned key, bool down) noexcept {
    if (key >= N) {
      ++filtered_;
      return;
    }
    const std::uint64_t bit = std::uint64_t{1} << key;
    if (((raw_ & bit) != 0u) == down) {
      ++filtered_;  // a press of a key that is down, a release of a key that is up
      return;
    }
    raw_ ^= bit;
    raw_order_[key] = ++seq_;
    if (((raw_ ^ accepted_) & bit) == 0u) {
      ++filtered_;  // back to the accepted state before the lockout ended: the pair of edges was a bounce
    }
  }

  // Accepts every raw change whose key has been quiet for the lockout. Call it once a pass, after the edges of the pass.
  void service(core::Tick now) noexcept {
    std::uint64_t pending = raw_ ^ accepted_;
    while (pending != 0u) {
      const unsigned key = lowest(pending);
      const std::uint64_t bit = std::uint64_t{1} << key;
      pending &= ~bit;
      // A key that has never changed is not locked out, whatever the clock says (the clock starts at zero and wraps).
      if ((ever_changed_ & bit) == 0u || core::tick_elapsed(now, changed_[key]) >= lockout_) {
        accepted_ ^= bit;
        changed_[key] = now;
        ever_changed_ |= bit;
        accepted_order_[key] = raw_order_[key];  // the place of this edge among the chip's: the order of the raw edge that was accepted
      }
    }
  }

  // The oldest accepted edge the consumer has not been told (the order the chip reported them in). `time` is when the key was accepted.
  bool next_unpublished(unsigned& key, bool& down, core::Tick& time) const noexcept {
    std::uint64_t todo = accepted_ ^ published_;
    if (todo == 0u) {
      return false;
    }
    unsigned best = lowest(todo);
    todo &= todo - 1u;
    while (todo != 0u) {
      const unsigned k = lowest(todo);
      todo &= todo - 1u;
      if (static_cast<std::int16_t>(static_cast<std::uint16_t>(accepted_order_[k] - accepted_order_[best])) < 0) {
        best = k;
      }
    }
    key = best;
    down = ((accepted_ >> best) & 1u) != 0u;
    time = changed_[best];
    return true;
  }
  // The edge of `key` was handed over (the published state takes the accepted one).
  void mark_published(unsigned key) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << key;
    published_ = (published_ & ~bit) | (accepted_ & bit);
  }

  // Forgets what is held, except the keys in `keep`: every other key that is down becomes raw-up (the chip lost a FIFO or its power),
  // so it is released through the normal path, after its lockout. `keep` is for keys whose latest state the chip has just reported
  // (their final event is the truth); the rest of what is believed down may have lost its release. Returns how many keys it let go.
  unsigned release_except(std::uint64_t keep) noexcept {
    std::uint64_t drop = raw_ & ~keep;
    const unsigned count = popcount((raw_ | accepted_) & ~keep);
    while (drop != 0u) {
      const unsigned key = lowest(drop);
      drop &= drop - 1u;
      raw_ &= ~(std::uint64_t{1} << key);
      raw_order_[key] = ++seq_;
    }
    return count;
  }
  unsigned release_all() noexcept { return release_except(0u); }

  std::uint64_t held() const noexcept { return accepted_; }
  bool is_held(unsigned key) const noexcept { return key < N && ((accepted_ >> key) & 1u) != 0u; }
  std::uint32_t filtered() const noexcept { return filtered_; }
  static constexpr unsigned size() noexcept { return N; }

 private:
  static unsigned lowest(std::uint64_t mask) noexcept {
    unsigned n = 0;
    while ((mask & 1u) == 0u) {
      mask >>= 1;
      ++n;
    }
    return n;
  }
  static unsigned popcount(std::uint64_t mask) noexcept {
    unsigned n = 0;
    for (; mask != 0u; mask &= mask - 1u) {
      ++n;
    }
    return n;
  }

  std::uint32_t lockout_;
  std::uint64_t raw_ = 0;        // what the chip last said
  std::uint64_t accepted_ = 0;   // what is believed
  std::uint64_t published_ = 0;  // what the consumer has been told
  std::uint64_t ever_changed_ = 0;  // the keys whose `changed_` time means something
  core::Tick changed_[N];        // when each key was last accepted
  std::uint16_t raw_order_[N] = {};       // the number of each key's latest raw edge
  std::uint16_t accepted_order_[N] = {};  // the number of the raw edge that its accepted state came from
  std::uint16_t seq_ = 0;
  std::uint32_t filtered_ = 0;
};

}  // namespace o3sdk::platform::mk66::input
