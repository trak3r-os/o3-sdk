// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DIN emitter (midi/din.h, OS-005c): the byte-time model, the arbitration "clock is sacred" (a real-time byte goes between the bytes of any message, and
// never waits for a SysEx or a controller burst), the lateness bounds of the orchestrator's decision (<= 50 us on an idle wire, <= one byte time = 320 us on a busy
// one) over fuzzed message streams, the guard window option, flushing. A virtual wire stands in for the UART: the emitter is polled at the times it asks for plus a
// random interrupt latency, and every byte it hands out is checked against the queues it was fed. Host and Cortex-M4F (`qemu_midi`).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/midi/din.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "support/rng.h"

namespace midi = o3sdk::platform::mk66::midi;
using o3sdk::test::Rng;

namespace {

constexpr std::uint64_t kByte = midi::din::kByteCycles;
constexpr std::uint64_t kHorizon = 2u * 64u * 4080u;  // two blocks of the audio clock, in cycles: how far ahead the render hands items over

#if defined(__arm__)
constexpr unsigned kScale = 4;
#else
constexpr unsigned kScale = 1;
#endif

double us(std::uint64_t cycles) { return static_cast<double>(cycles) / 180.0; }

struct Item {
  std::uint64_t due;
  bool rt;
  std::uint8_t kind;  // 0 note, 1 cc, 2 pc, 3 sysex
  std::uint8_t len;   // sysex data bytes
  std::uint8_t b[3];
  std::uint8_t sx[48];
};

// What a run measured.
struct Result {
  std::uint64_t rt_late_max = 0;
  std::uint64_t rt_late_sum = 0;
  std::uint32_t rt_count = 0;
  std::uint32_t msg_bytes = 0;
  std::uint32_t rt_inserted = 0;
  std::uint32_t errors = 0;       // a violated property
  std::uint32_t pushed_msgs = 0;
};

// The expected ordinary byte stream, FIFO.
class Expect {
 public:
  void put(std::uint8_t b) {
    buf_[(head_ + n_) & (kN - 1)] = b;
    ++n_;
  }
  bool take(std::uint8_t b) {
    if (n_ == 0 || buf_[head_] != b) {
      return false;
    }
    head_ = (head_ + 1) & (kN - 1);
    --n_;
    return true;
  }
  std::uint32_t pending() const { return n_; }

 private:
  static constexpr std::uint32_t kN = 8192;
  std::uint8_t buf_[kN];
  std::uint32_t head_ = 0;
  std::uint32_t n_ = 0;
};

struct Scenario {
  std::uint64_t run_cycles;          // length of the run
  std::uint64_t clock_period;        // 0: no clocks
  std::uint32_t msg_gap_mean_us;     // 0: no ordinary traffic
  std::uint32_t sysex_percent;       // of the messages
  std::uint32_t latency_max_cycles;  // the interrupt latency, uniform 0..max
  std::uint32_t depth;
  bool guard;
  std::uint32_t seed;
};

Result run(const Scenario& sc) {
  midi::DinEmitter em;
  midi::DinEmitter::Config cfg;
  cfg.commit_depth = sc.depth;
  cfg.guard_window = sc.guard;
  em.configure(cfg);
  Rng rng(sc.seed);
  Expect expect;
  Result r;

  // The generators: the next clock, the next ordinary message.
  std::uint64_t next_clock = sc.clock_period != 0 ? 3u * kByte + 12345u : midi::kNever;
  std::uint64_t next_msg = midi::kNever;
  Item pending_msg{};
  bool have_msg = false;
  const auto draw_msg = [&](std::uint64_t after) {
    const std::uint64_t gap = static_cast<std::uint64_t>(sc.msg_gap_mean_us) * 90u + (static_cast<std::uint64_t>(rng.next()) % (static_cast<std::uint64_t>(sc.msg_gap_mean_us) * 180u + 1u));
    Item it{};
    it.due = after + gap;
    it.rt = false;
    const std::uint32_t roll = rng.below(100);
    if (roll < sc.sysex_percent) {
      it.kind = 3;
      it.len = static_cast<std::uint8_t>(rng.below(45));
      for (std::uint32_t i = 0; i < it.len; ++i) {
        it.sx[i] = static_cast<std::uint8_t>(rng.next() & 0x7F);
      }
    } else if (roll < sc.sysex_percent + 10u) {
      it.kind = 2;
      it.b[0] = static_cast<std::uint8_t>(0xC0u | rng.below(16));
      it.b[1] = static_cast<std::uint8_t>(rng.below(128));
    } else {
      it.kind = static_cast<std::uint8_t>(rng.below(2));
      it.b[0] = static_cast<std::uint8_t>((it.kind == 0 ? 0x90u : 0xB0u) | rng.below(16));
      it.b[1] = static_cast<std::uint8_t>(rng.below(128));
      it.b[2] = static_cast<std::uint8_t>(rng.below(128));
    }
    pending_msg = it;
    have_msg = true;
    next_msg = it.due;
  };
  if (sc.msg_gap_mean_us != 0) {
    draw_msg(0);
  }

  std::uint64_t now = 0;
  std::uint64_t prev_end = 0;
  std::uint64_t rt_due_queue[64];
  std::uint32_t rt_q_head = 0, rt_q_n = 0;
  std::uint32_t guard_needed = 0;
  (void)guard_needed;

  while (now < sc.run_cycles) {
    // The producer: hand over what is due within the horizon, as MidiOut::flush does.
    while (next_clock != midi::kNever && next_clock <= now + kHorizon && next_clock < sc.run_cycles) {
      if (em.push_rt(next_clock, midi::kClock)) {
        rt_due_queue[(rt_q_head + rt_q_n) & 63] = next_clock;
        ++rt_q_n;
      } else {
        break;
      }
      next_clock += sc.clock_period;
    }
    while (have_msg && next_msg <= now + kHorizon && next_msg < sc.run_cycles) {
      bool ok = false;
      if (pending_msg.kind == 3) {
        ok = em.push_sysex(pending_msg.due, pending_msg.sx, pending_msg.len);
        if (ok) {
          expect.put(0xF0);
          for (std::uint32_t i = 0; i < pending_msg.len; ++i) {
            expect.put(pending_msg.sx[i]);
          }
          expect.put(0xF7);
        }
      } else {
        const std::uint8_t len = pending_msg.kind == 2 ? 2 : 3;
        ok = em.push_msg(pending_msg.due, midi::Msg{len, {pending_msg.b[0], pending_msg.b[1], pending_msg.b[2]}});
        if (ok) {
          for (std::uint8_t i = 0; i < len; ++i) {
            expect.put(pending_msg.b[i]);
          }
        }
      }
      if (!ok) {
        break;  // the ring is full: try again next time
      }
      ++r.pushed_msgs;
      draw_msg(pending_msg.due);
    }

    // The next time something happens: the emitter's wake, or the producer's next push.
    const std::uint64_t wake = em.next_wake(now);
    std::uint64_t push_at = midi::kNever;
    if (next_clock != midi::kNever && next_clock - kHorizon > now) {
      push_at = next_clock - kHorizon;
    }
    if (have_msg && next_msg - kHorizon > now && next_msg - kHorizon < push_at) {
      push_at = next_msg - kHorizon;
    }
    if (wake == midi::kNever && push_at == midi::kNever) {
      break;
    }
    if (push_at < wake) {
      now = push_at;
      continue;
    }
    const std::uint64_t lat = sc.latency_max_cycles != 0 ? rng.below(sc.latency_max_cycles + 1) : 0;
    now = wake + lat;
    const midi::DinEmitter::Tx tx = em.poll(now);
    if (!tx.valid) {
      if (wake <= now && now >= sc.run_cycles) {
        break;
      }
      // Nothing to send though the emitter asked to be woken: legal when a guard hold or a race with the producer moved it on. Progress is guaranteed by the next
      // next_wake() being later; if it is not, the test would loop: count it as an error and stop.
      if (em.next_wake(now) <= now && !(sc.guard)) {
        ++r.errors;
        break;
      }
      now += 1;
      continue;
    }
    // ---- properties of what went out ----
    if (tx.start < prev_end) {
      ++r.errors;  // P1: two bytes overlap on the wire
    }
    prev_end = tx.start + kByte;
    if (tx.realtime) {
      if (rt_q_n == 0 || rt_due_queue[rt_q_head] != tx.due) {
        ++r.errors;  // P4: real-time bytes go in the order they were queued
      } else {
        rt_q_head = (rt_q_head + 1) & 63;
        --rt_q_n;
      }
      const std::uint64_t late = tx.start - tx.due;
      ++r.rt_count;
      r.rt_late_sum += late;
      if (late > r.rt_late_max) {
        r.rt_late_max = late;
      }
      if (tx.byte != midi::kClock) {
        ++r.errors;
      }
    } else {
      ++r.msg_bytes;
      if (!expect.take(tx.byte)) {
        ++r.errors;  // P3: the ordinary bytes are the messages, whole and in order
      }
    }
  }
  r.rt_inserted = em.stats().rt_inserted;
  return r;
}

}  // namespace

TEST_CASE("din: the byte-time model of the wire") {
  CHECK(midi::din::kByteCycles == 57600u);         // 10 bits at 31,250 baud, at 180 MHz
  CHECK(midi::din::kByteMicros == 320u);
  CHECK(midi::din::bytes_cycles(3) == 172800u);
  CHECK(midi::din::realtime_lateness_bound(1, false) == 0u);  // an idle wire: no wait
  CHECK(midi::din::realtime_lateness_bound(1, true) == kByte);
  CHECK(midi::din::realtime_lateness_bound(2, true) == 2u * kByte);
}

TEST_CASE("din: a real-time byte on an idle wire starts on time") {
  midi::DinEmitter em;
  CHECK(em.next_wake(0) == midi::kNever);
  CHECK(em.push_rt(1000000, midi::kClock));
  CHECK(em.next_wake(0) == 1000000u);
  const midi::DinEmitter::Tx early = em.poll(999999);
  CHECK_FALSE(early.valid);  // not due yet
  const midi::DinEmitter::Tx t = em.poll(1000000);
  CHECK(t.valid);
  CHECK(t.realtime);
  CHECK(t.byte == midi::kClock);
  CHECK(t.start == 1000000u);
  CHECK(em.busy_until() == 1000000u + kByte);
  CHECK(em.stats().rt_lateness_max == 0u);
  CHECK_FALSE(em.push_rt(0, 0x90));                  // not a real-time byte: refused
  CHECK_FALSE(em.push_msg(0, midi::msg1(midi::kClock)));  // a real-time byte is not a message
  CHECK_FALSE(em.push_msg(0, midi::msg1(0xF0)));
}

TEST_CASE("din: a clock goes between the bytes of a message, not after it") {
  midi::DinEmitter em;
  CHECK(em.push_msg(0, midi::note_on(0, 60, 100)));
  CHECK(em.push_rt(100u * 180u, midi::kClock));  // due 100 us in: the first byte is on the wire until 320 us
  midi::DinEmitter::Tx t = em.poll(0);
  CHECK(t.valid);
  CHECK_FALSE(t.realtime);
  CHECK(t.byte == 0x90);
  CHECK_FALSE(em.poll(kByte - 1).valid);  // the wire is busy
  CHECK(em.next_wake(10) == kByte);       // when it frees
  t = em.poll(kByte);
  CHECK(t.valid);
  CHECK(t.realtime);  // the clock, due 220 us ago, before the second byte of the message
  CHECK(t.start - t.due == kByte - 100u * 180u);
  t = em.poll(2u * kByte);
  CHECK(t.valid);
  CHECK(t.byte == 60);
  t = em.poll(3u * kByte);
  CHECK(t.valid);
  CHECK(t.byte == 100);
  CHECK(em.stats().rt_inserted == 1u);
  CHECK_FALSE(em.message_open());
}

TEST_CASE("din: a clock never waits for a SysEx: it is late by one byte at most") {
  midi::DinEmitter em;
  std::uint8_t data[40];
  for (std::uint8_t i = 0; i < 40; ++i) {
    data[i] = i;
  }
  CHECK(em.push_sysex(0, data, 40));  // 42 bytes: 13.4 ms on the wire
  const std::uint64_t period = 3750000;  // one clock at 120 BPM: 20.8 ms; use a shorter one to put several inside the SysEx
  const std::uint64_t clk = period / 4;  // 5.2 ms
  for (int i = 1; i <= 3; ++i) {
    CHECK(em.push_rt(clk * static_cast<std::uint64_t>(i) + 777, midi::kClock));
  }
  std::uint64_t now = 0;
  std::uint32_t sysex_bytes = 0;
  std::uint32_t clocks = 0;
  std::uint64_t worst = 0;
  std::uint8_t first = 0, last = 0;
  for (int guard = 0; guard < 100; ++guard) {
    const std::uint64_t w = em.next_wake(now);
    if (w == midi::kNever) {
      break;
    }
    now = w;
    const midi::DinEmitter::Tx t = em.poll(now);
    if (!t.valid) {
      continue;
    }
    if (t.realtime) {
      ++clocks;
      worst = t.start - t.due > worst ? t.start - t.due : worst;
    } else {
      if (sysex_bytes == 0) {
        first = t.byte;
      }
      last = t.byte;
      ++sysex_bytes;
    }
  }
  CHECK(sysex_bytes == 42u);
  CHECK(first == 0xF0);
  CHECK(last == 0xF7);
  CHECK(clocks == 3u);
  CHECK(worst <= kByte);  // the orchestrator's busy-wire bound: one byte time (320 us)
  CHECK(em.stats().rt_inserted == 2u);  // the first two fall inside the 13.4 ms SysEx, the third (15.6 ms) after it
}

TEST_CASE("din: with commit depth 2 the bound is two byte times, with depth 1 it is one") {
  for (std::uint32_t depth = 1; depth <= 2; ++depth) {
    midi::DinEmitter em;
    midi::DinEmitter::Config cfg;
    cfg.commit_depth = depth;
    em.configure(cfg);
    CHECK(em.push_msg(0, midi::note_on(0, 60, 100)));
    // The UART takes the first byte, and with depth 2 the second as well, at once; only then does the clock get queued, 100 us in.
    CHECK(em.poll(0).valid);
    const bool second = em.poll(0).valid;
    CHECK(second == (depth == 2u));
    CHECK(em.push_rt(100u * 180u, midi::kClock));
    std::uint64_t now = 0;
    std::uint64_t worst = 0;
    for (int i = 0; i < 20; ++i) {
      const std::uint64_t w = em.next_wake(now);
      if (w == midi::kNever) {
        break;
      }
      now = w;
      const midi::DinEmitter::Tx t = em.poll(now);
      if (t.valid && t.realtime) {
        worst = t.start - t.due;
      }
    }
    if (depth == 1) {
      CHECK(worst <= kByte);
      CHECK(em.stats().rt_late_over_byte == 0u);
    } else {
      // The UART already holds the second byte when the clock falls due, so the clock waits for both: more than one byte time, at most two.
      CHECK(worst > kByte);
      CHECK(worst <= 2u * kByte);
      CHECK(em.stats().rt_late_over_byte == 1u);
    }
  }
}

TEST_CASE("din: the guard window keeps ordinary bytes off the wire in the byte time before a clock") {
  midi::DinEmitter em;
  midi::DinEmitter::Config cfg;
  cfg.guard_window = true;
  em.configure(cfg);
  // A message due now, a clock due 200 us later: a byte started now would still be on the wire at the clock, so it waits until the clock has gone.
  CHECK(em.push_msg(0, midi::note_on(0, 60, 100)));
  CHECK(em.push_rt(200u * 180u, midi::kClock));
  CHECK_FALSE(em.poll(0).valid);
  CHECK(em.stats().guard_holds >= 1u);
  CHECK(em.next_wake(0) == 200u * 180u);
  const midi::DinEmitter::Tx c = em.poll(200u * 180u);
  CHECK(c.valid);
  CHECK(c.realtime);
  CHECK(c.start == 200u * 180u);  // not late at all: the wire was idle
  // The message follows the clock.
  const midi::DinEmitter::Tx m = em.poll(200u * 180u + kByte);
  CHECK(m.valid);
  CHECK(m.byte == 0x90);
  // Without the guard the same stream puts the message first and delays the clock by 120 us.
  midi::DinEmitter plain;
  CHECK(plain.push_msg(0, midi::note_on(0, 60, 100)));
  CHECK(plain.push_rt(200u * 180u, midi::kClock));
  CHECK(plain.poll(0).valid);
  const midi::DinEmitter::Tx late = plain.poll(kByte);
  CHECK(late.realtime);
  CHECK(late.start - late.due == kByte - 200u * 180u);
}

TEST_CASE("din: flushing drops what was queued before and keeps what comes after; a cut SysEx is closed with F7") {
  midi::DinEmitter em;
  const std::uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK(em.push_sysex(0, data, 10));
  CHECK(em.push_msg(0, midi::note_on(0, 60, 100)));
  CHECK(em.push_rt(50, midi::kClock));
  std::uint64_t now = 0;
  // F0 and the first data bytes go out, then the producer flushes.
  midi::DinEmitter::Tx t = em.poll(now);
  CHECK(t.byte == 0xF0);
  now = em.next_wake(now);
  t = em.poll(now);
  CHECK(t.valid);
  CHECK(t.realtime);  // the clock, due 50 cycles in
  now = em.next_wake(now);
  t = em.poll(now);
  CHECK(t.byte == 1);  // the SysEx continues
  em.request_flush();
  CHECK(em.push_msg(now, midi::note_on(1, 70, 90)));  // after the flush: it survives
  std::uint8_t wire[16];
  std::uint32_t n = 0;
  for (int i = 0; i < 20; ++i) {
    now = em.next_wake(now);
    if (now == midi::kNever) {
      break;
    }
    t = em.poll(now);
    if (t.valid && n < 16u) {
      wire[n++] = t.byte;
    }
  }
  CHECK(n == 4u);
  CHECK(wire[0] == 0xF7);  // the cut SysEx is closed
  CHECK(wire[1] == 0x91);  // then the new message, whole
  CHECK(wire[2] == 70);
  CHECK(wire[3] == 90);
  CHECK(em.next_wake(now) == midi::kNever);
}

TEST_CASE("din: SysEx and chunking: the framing is added, a run is all or nothing") {
  midi::DinEmitter em;
  std::uint8_t big[3 * 256];
  for (std::uint32_t i = 0; i < sizeof big; ++i) {
    big[i] = static_cast<std::uint8_t>(i & 0x7F);
  }
  CHECK_FALSE(em.push_sysex(0, big, 3 * 256));  // too long for the ring: nothing was queued
  CHECK(em.msg_room() == midi::kMsgRingSize);
  CHECK_FALSE(em.push_sysex(0, nullptr, 4));
  CHECK(em.push_sysex(0, big, 0));  // F0 F7: an empty SysEx is legal
  std::uint8_t wire[4];
  std::uint32_t n = 0;
  std::uint64_t now = 0;
  for (int i = 0; i < 8; ++i) {
    now = em.next_wake(now);
    if (now == midi::kNever) {
      break;
    }
    const midi::DinEmitter::Tx t = em.poll(now);
    if (t.valid && n < 4u) {
      wire[n++] = t.byte;
    }
  }
  CHECK(n == 2u);
  CHECK(wire[0] == 0xF0);
  CHECK(wire[1] == 0xF7);
  // 7-bit masking of the data
  const std::uint8_t high[2] = {0xFF, 0x80};
  std::uint64_t at = em.busy_until();
  CHECK(em.push_sysex(at, high, 2));
  n = 0;
  std::uint8_t w4[4];
  for (int i = 0; i < 8; ++i) {
    const std::uint64_t w = em.next_wake(at);
    if (w == midi::kNever) {
      break;
    }
    at = w;
    const midi::DinEmitter::Tx t = em.poll(at);
    if (t.valid && n < 4u) {
      w4[n++] = t.byte;
    }
  }
  CHECK(n == 4u);
  CHECK(w4[1] == 0x7F);
  CHECK(w4[2] == 0x00);
}

TEST_CASE("din: fuzzed streams: integrity, order, and the lateness of the clock on an idle and on a busy wire") {
  const bool report = std::getenv("O3SDK_SEQ_REPORT") != nullptr;
  struct Case {
    const char* name;
    Scenario sc;
    std::uint64_t max_late_cycles;  // the bound the case must keep
  };
  const std::uint64_t kLat = 540;  // 3 us of interrupt latency, an estimate (the PIT ISR entry jitter is a HW-B item)
  const std::uint64_t run_len = 180000000ull / kScale;  // 1 s (the target runs a fraction)
  const Case cases[] = {
      // Clock only: the wire is idle, the clock starts when due, plus the interrupt's latency.
      {"idle wire, clock only", {run_len, 3750000, 0, 0, 540, 1, false, 1}, kLat},
      // Sparse notes: the wire is usually idle; a clock that meets a byte waits for it.
      {"sparse notes", {run_len, 3750000, 20000, 0, 540, 1, false, 2}, kByte + kLat},
      // A busy wire: a message every 1.4 ms on average (70 % of the wire), SysEx among them.
      {"busy wire, notes and CC", {run_len, 3750000, 1400, 0, 540, 1, false, 3}, kByte + kLat},
      {"busy wire, SysEx burst", {run_len, 3750000, 3000, 40, 540, 1, false, 4}, kByte + kLat},
      {"busy wire, 800 BPM clock", {run_len, 562500, 1500, 15, 540, 1, false, 5}, kByte + kLat},
      // Depth two, the UART double-buffered: two byte times.
      {"busy wire, commit depth 2", {run_len, 3750000, 1400, 10, 540, 2, false, 6}, 2u * kByte + kLat},
      // The guard window: the clock finds the wire idle whenever it was queued ahead, whatever the traffic.
      {"busy wire, guard window", {run_len, 3750000, 1400, 10, 540, 1, true, 7}, kLat},
      {"busy wire, guard window, 800 BPM", {run_len, 562500, 1500, 15, 540, 1, true, 8}, kLat},
  };
  // Eight seeds a case on the host (one on the target): the figure reported is the worst of all of them.
#if defined(__arm__)
  constexpr std::uint32_t kSeeds = 1;
#else
  constexpr std::uint32_t kSeeds = 8;
#endif
  for (const Case& c : cases) {
    Result r;
    for (std::uint32_t k = 0; k < kSeeds; ++k) {
      Scenario sc = c.sc;
      sc.seed += 1000u * k;
      const Result one = run(sc);
      r.errors += one.errors;
      r.rt_count += one.rt_count;
      r.rt_late_sum += one.rt_late_sum;
      r.msg_bytes += one.msg_bytes;
      r.rt_inserted += one.rt_inserted;
      if (one.rt_late_max > r.rt_late_max) {
        r.rt_late_max = one.rt_late_max;
      }
    }
    CHECK_MESSAGE(r.errors == 0u, c.name);
    CHECK_MESSAGE(r.rt_count >= 5u, c.name);
    CHECK_MESSAGE(r.rt_late_max <= c.max_late_cycles, c.name);
    if (c.sc.msg_gap_mean_us != 0u) {
      CHECK_MESSAGE(r.msg_bytes > 20u, c.name);
    }
    if (report) {
      std::printf("din  %-34s clocks %5u  late max %7.1f us  mean %6.1f us  inserted %4u  msg bytes %6u\n", c.name, static_cast<unsigned>(r.rt_count), us(r.rt_late_max),
                  r.rt_count != 0u ? us(r.rt_late_sum / r.rt_count) : 0.0, static_cast<unsigned>(r.rt_inserted), static_cast<unsigned>(r.msg_bytes));
    }
  }
}
