// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The DIN MIDI emitter's state machine and the byte-time model of the wire (OS-005c; ADR 0013 D5, radar card midi-clock-sync, "Orchestrator
// decisions": "clock is sacred"). Pure: it decides WHICH byte goes to the UART next and WHEN, given the queues and the time; the MK66 driver
// of OS-005c2 only calls poll() from the interrupt that the PIT compare or the UART's transmit-complete raises and writes the byte it returns.
//
// THE WIRE. DIN MIDI is 31,250 baud, 8N1: a byte is 10 bits = 320 us = 57,600 CPU cycles at 180 MHz, exactly. A byte that has started cannot
// be interrupted, so a real-time byte (clock, start, continue, stop) that falls due while another byte is on the wire waits for the end of it:
//     lateness of a real-time byte  <=  (commit_depth - 1) bytes in the UART's hands  +  the byte in flight  =  commit_depth byte times
// and zero on an idle wire (plus the interrupt latency, which is the platform's). The targets of the orchestrator's decision: clock start
// within 50 us of its time on an idle wire, within one byte time (320 us) on a busy one. With commit_depth 1 (a byte is handed to the UART only
// when the previous one has gone: the default) the busy-wire bound IS one byte time; with 2 (the UART's data register loaded while the shift
// register still sends) it is two, which is why the default is 1. What commit_depth 1 costs is a gap of one interrupt latency between bytes of a
// message, which MIDI allows; whether the K66's UART needs a deeper commit to keep its 3-byte messages tight is a hardware item (HW-B).
//
// ARBITRATION. Two queues feed the wire. The real-time queue holds single bytes (F8, FA, FB, FC) with the time each is due; the message queue
// holds ordinary messages (notes, controllers, program changes, SysEx as runs of three-byte chunks) with the time each is due. At every byte
// boundary:
//   1. a real-time byte whose time has come goes first, even in the middle of a message (it does not cancel running status, and the emitter
//      does not use running status at all);
//   2. else the message in progress sends its next byte, back to back;
//   3. else a message whose time has come starts.
// Real-time bytes are sent in the order they were queued. A message never waits for a real-time byte except to give way at a byte boundary.
// Clock timing therefore never waits for a running SysEx or a controller burst: at worst for the one byte in flight.
//
// GUARD WINDOW (option, off by default). The card proposed that no ordinary byte may START in the 320 us before a queued real-time byte falls
// due, so that the clock always finds the wire idle; the cost is that ordinary bytes due in that window slip by 320 to 640 us. The orchestrator
// decided for plain insertion; the option exists so that the HW-B session can compare the two on the logic analyser.
//
// Time is CPU cycles of the audio timeline (core::cycles_from_samples). The rings: the render pushes, the emitter's interrupt pops: one
// producer and one consumer each (core/ring.h). The real-time queue holds 64 bytes, the message queue 256 chunks.

#include <o3sdk/platform/mk66/audio/hot.h>
#include <o3sdk/core/audio_clock.h>
#include <o3sdk/core/ring.h>
#include <o3sdk/platform/mk66/midi/message.h>

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace o3sdk::platform::mk66::midi {

namespace din {
inline constexpr std::uint32_t kBaud = 31250;
inline constexpr std::uint32_t kBitsPerByte = 10;  // start, eight data bits, stop
// CPU cycles one byte occupies the wire: 57,600 at 180 MHz.
inline constexpr std::uint64_t kByteCycles = std::uint64_t{core::kCpuHz} * kBitsPerByte / kBaud;
static_assert(std::uint64_t{core::kCpuHz} * kBitsPerByte % kBaud == 0u, "a DIN byte is a whole number of CPU cycles");
inline constexpr std::uint32_t kByteMicros = 320;
static_assert(kByteCycles == std::uint64_t{kByteMicros} * (core::kCpuHz / 1000000u), "320 us");

// The time `n` bytes occupy the wire.
constexpr std::uint64_t bytes_cycles(std::uint32_t n) noexcept { return kByteCycles * n; }

// The lateness bound of a real-time byte, in cycles, from the number of bytes the UART holds when it falls due (`commit_depth`, 1 or 2) and
// whether the wire is busy: the model the targets are checked against.
constexpr std::uint64_t realtime_lateness_bound(std::uint32_t commit_depth, bool wire_busy) noexcept {
  return wire_busy ? kByteCycles * commit_depth : 0u;
}
}  // namespace din

// An item of either queue: up to three bytes with the time they are due. A SysEx message (or any longer one) is a run of chunks: the first
// chunk has `run` = the number of chunks in the run, the others have run 0; the emitter starts a run only when the whole run is in the ring,
// so a run is never starved halfway. Plain messages and real-time bytes have run 1.
struct OutItem {
  std::uint64_t due;     // CPU cycles on the audio timeline
  std::uint8_t len;      // 1..3
  std::uint8_t run;      // see above
  std::uint8_t b[3];
  std::uint8_t epoch;    // the producer's flush epoch when it pushed (stamped by the push functions): see DinEmitter::request_flush
  std::uint8_t pad[2];
};
static_assert(sizeof(OutItem) == 16 && alignof(OutItem) == 8 && std::is_trivially_copyable<OutItem>::value, "OutItem is two words of eight");

inline constexpr std::uint32_t kRtRingSize = 64;
inline constexpr std::uint32_t kMsgRingSize = 256;
using RtRing = core::SpscRing<OutItem, kRtRingSize>;
using MsgRing = core::SpscRing<OutItem, kMsgRingSize>;

constexpr OutItem out_item(std::uint64_t due, const Msg& m) noexcept {
  return OutItem{due, m.len, 1u, {m.b[0], m.b[1], m.b[2]}, 0u, {0, 0}};
}

inline constexpr std::uint64_t kNever = ~std::uint64_t{0};

class DinEmitter {
 public:
  struct Config {
    std::uint32_t commit_depth = 1;  // bytes the UART holds at once: 1 (a byte only when the previous is gone) or 2 (double-buffered)
    bool guard_window = false;       // the card's option (see above); off: plain insertion, the orchestrator's decision
  };

  // What poll() decided: a byte to start on the wire at `start` (CPU cycles), and when it had been due (a real-time byte's lateness is
  // start - due; an ordinary byte's is the delay of the message start, or 0 inside a message).
  struct Tx {
    bool valid;
    bool realtime;
    std::uint8_t byte;
    std::uint64_t start;
    std::uint64_t due;
  };

  struct Stats {
    std::uint32_t rt_sent = 0;
    std::uint32_t msg_bytes = 0;
    std::uint32_t rt_inserted = 0;          // real-time bytes that went out in the middle of a message
    std::uint32_t rt_late_over_byte = 0;    // real-time bytes later than one byte time (impossible with commit_depth 1)
    std::uint64_t rt_lateness_max = 0;      // cycles
    std::uint64_t rt_lateness_sum = 0;
    std::uint32_t guard_holds = 0;          // ordinary bytes held back by the guard window
  };

  DinEmitter() = default;
  explicit DinEmitter(const Config& c) : cfg_(c) {}

  void configure(const Config& c) noexcept { cfg_ = c; }
  const Config& config() const noexcept { return cfg_; }

  // ---- producer: the render context -----------------------------------------------------------------------------
  // `due` is when the byte or message should start. A real-time item must be one byte, 0xF8..0xFF (anything else is refused).
  bool push_rt(std::uint64_t due, std::uint8_t byte) noexcept;
  // A message of one to three bytes.
  bool push_msg(std::uint64_t due, const Msg& m) noexcept;
  // A SysEx message (data bytes between F0 and F7, the framing is added): all of it or nothing (false when the ring has no room). `n` up
  // to 3 * (kMsgRingSize - 1) - 2.
  bool push_sysex(std::uint64_t due, const std::uint8_t* data, std::uint32_t n) noexcept;
  // Room for chunks, as the producer sees it (a lower bound).
  std::uint32_t msg_room() const noexcept { return kMsgRingSize - msg_.size(); }
  std::uint32_t rt_room() const noexcept { return kRtRingSize - rt_.size(); }

  // ---- consumer: the emitter's interrupt (one context) ------------------------------------------------------------
  // Called when the wire may take a byte: at the transmit-complete interrupt, at the PIT compare of next_wake(), or after a push. Returns
  // the byte to start (valid false: nothing to send now). Never blocks and never loops more than a few steps.
  O3SDK_AUDIO_HOT_FLAT Tx poll(std::uint64_t now) noexcept;
  // When poll() should be called next if nothing else happens: the end of the byte in flight, or the earliest due time of what is queued,
  // or kNever when both queues are empty and no message is open. Always >= `now` when something is queued.
  O3SDK_AUDIO_HOT_FLAT std::uint64_t next_wake(std::uint64_t now) noexcept;

  // Producer side, any time: everything pushed BEFORE this call is discarded by the consumer (a message in progress is cut, and closed with
  // F7 if it was a SysEx), everything pushed after it survives. This is how a stop with a panic works: request_flush(), then push the note-offs.
  // It is a handshake on a counter, not a lock: the consumer compares each item's epoch with the counter when it takes the item.
  void request_flush() noexcept { epoch_.fetch_add(1u, std::memory_order_acq_rel); }

  std::uint64_t busy_until() const noexcept { return busy_until_; }
  bool message_open() const noexcept { return cur_pos_ < cur_len_ || chunks_left_ != 0u; }
  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

 private:
  // Everything poll() and next_wake() call is in the same section (and the functions that call a ring's pop or size, or message_open(), inline those: O3SDK_AUDIO_HOT_FLAT): the emitter's interrupt is the one latency-critical context of this library (it is the candidate for
  // SRAM_L; the render-context code of midi/ and seq/ runs from flash, once a block).
  O3SDK_AUDIO_HOT_FLAT void sync_epoch() noexcept;
  O3SDK_AUDIO_HOT_FLAT void fill_rt_head(std::uint8_t ep) noexcept;
  O3SDK_AUDIO_HOT_FLAT void fill_msg_head(std::uint8_t ep) noexcept;
  O3SDK_AUDIO_HOT bool ordinary_held_by_guard(std::uint64_t start) noexcept;
  O3SDK_AUDIO_HOT std::uint64_t ready_time(std::uint64_t due, std::uint64_t free_at) const noexcept;
  O3SDK_AUDIO_HOT std::uint64_t free_at() const noexcept;

  Config cfg_{};
  std::atomic<std::uint32_t> epoch_{0};  // written by the producer, read by the consumer
  RtRing rt_;
  MsgRing msg_;
  // The message in progress (consumer-only state).
  std::uint8_t cur_[3] = {0, 0, 0};
  std::uint32_t cur_len_ = 0;
  std::uint32_t cur_pos_ = 0;
  std::uint32_t chunks_left_ = 0;  // chunks of the open run still in the ring
  std::uint64_t cur_due_ = 0;
  std::uint8_t cur_epoch_ = 0;
  bool cur_sysex_ = false;         // the open message began with F0 and has not sent F7
  std::uint64_t busy_until_ = 0;   // when the last byte handed to the wire finishes
  // Peeked heads (consumer-only): an item is popped only when it is sent, but an SpscRing has no peek, so the head is held here.
  OutItem rt_head_{};
  bool rt_head_valid_ = false;
  OutItem msg_head_{};
  bool msg_head_valid_ = false;
  Stats stats_{};
};

}  // namespace o3sdk::platform::mk66::midi
