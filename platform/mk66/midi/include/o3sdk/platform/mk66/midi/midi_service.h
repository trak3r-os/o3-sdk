// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's MIDI in and out as the kit sees them (docs/M2B2-PLAN.md section 3, slice MD): hal::midi::Device over the timing core of this library. One object that sits
// between the engine's render (Runtime::render_block: begin_block, the engine, end_block), the main loop (Runtime::poll: poll, send, request; the Config screen: set_config,
// status) and the drivers' interrupts (UART4 RX, PIT0, USB0):
//
//   UART4 RX ISR --stamp--> din_in() --+                                                           +--> din() --PIT0 compare--> UART4 TX
//   USB0 ISR     --stamp--> usb_in() --+--> begin_block: MidiIn::drain -> ClockDll -> ClockFrame   |
//                                         \--> ChannelParser -> notes (frames) / poll ring       |
//                      the engine renders ... end_block: clock, transport, hooks' messages -> MidiOut -+--> usb_out() --SOF--> USB bulk IN
//
// It is the render side of track3r-os's seq::MidiLink (seq/midi_link.{h,cpp}) without the sequencer: the drain, the follow state, the flush and the kick are that file's, the
// transport is the engine's (Engine::follow / block_frame) instead of the sequencer's own. What a block does:
//   begin_block   1. publish the time base: the timeline sample the block starts at and the cycle-counter value at which it is HEARD (the hook), to TimeRef (the interrupts) and
//                    to the ClockMap (the stamps of incoming bytes);
//                 2. take a new Config if the main loop wrote one (seqlock, never waits) and act on it;
//                 3. take the main loop's Play / Stop request;
//                 4. drain both ports' rings: the clock of the followed port goes to the ClockDll, Start / Continue / Stop / SPP become transport events, every other byte goes
//                    to the channel-message parser: each whole message is queued for poll(), and a note, or an all-sound / all-notes-off controller, is handed to the engine at
//                    its frame (the first `cap` of them; the rest wait for the next block);
//                 5. look after the follow state: attach once the master's clock is there (two clocks seen), let go when it has been quiet for 48 clocks or the user turned FOLLOW off;
//                 6. the block's ClockFrame: the master's position at frame 0 and the increment that takes the engine to where the master will be at the end of the block.
//   end_block     the hooks' messages (BlockContext::send_midi) and the main loop's send()s into MidiOut at their frames; the clock (24 a quarter note, one at every
//                 frame_at(t, k * 40) of the block, to a fraction of a sample), Start at `started`, SPP + Continue where a run begins elsewhere, Stop at `stopped` (and
//                 MidiOut::stop_all, which releases whatever note is still owed); MidiOut::flush hands the ports what is due inside the horizon (2.9 ms); the PIT0 kick if
//                 anything went to the DIN ring.
//
// THE FOLLOW CONTRACT (hal/midi.h). The master's clock is stamped in the interrupt, mapped to the sample timeline through the audio clock (ClockMap) and filtered by the
// ClockDll, which slews and never jumps. ClockFrame::pos is the master's tick at frame 0 of the block (40 ticks of the engine's 960 ppqn grid to a MIDI clock), inc its ticks
// a sample; the next block starts where this one ends (pos + inc * frames), exactly. To keep both promises the service keeps the engine's position itself and chases the
// DLL's: each block's inc is the one that lands the position on the DLL's value at the end of the block, held to +-25 % of the DLL's own increment. In steady state the
// position is the DLL's (a step lands on the master's clock to the sample); after a Start, a Continue, or a correction of the DLL (it re-acquires after a tempo jump, it
// rebases its origin) the position approaches the master's at at most 25 % of a sample a sample, never jumping. A Start begins the run with pos 0 on the block that sees
// it: the master's clock was stamped at the instant it was HEARD, which is the render-ahead (two blocks) before the block now rendering, so the run begins about three
// blocks (4 ms) behind the master and the position catches up in a few more.
//
// CONTEXTS (each shared object has ONE writer and ONE reader, and says which):
//   render (priority 128)     begin_block, end_block and everything they call: MidiIn, ClockDll, MidiOut, the parser, the rings' consumer / producer ends below.
//   main loop                 set_config() (SeqLockBox: one writer), status(), poll() (the consumer of poll_ring_), send() (the producer of send_ring_), request() (one atomic byte),
//                             diag() (a snapshot of words the render writes, for the console: a block old, may tear).
//   UART4 RX interrupt (48)   the ONE producer of din_in() (midi/din_io.h DinRx).
//   USB0 interrupt (64)       the ONE producer of usb_in(), the ONE consumer of usb_out() (midi/usb_bind.h), a reader of time_ref().
//   PIT0 interrupt (96)       the ONE consumer of din() (midi/din_io.h DinTx), a reader of time_ref(). All three are above the render, which TimeRef's protocol needs.
//
// REAL TIME. Everything is bounded: at most kDrainItems items from each ring and kEventCap transport events a block, kPassCap channel bytes a drain, kMaxMessagesPerBlock hooks'
// messages and kSendPerBlock sends, MidiOut's own bounds. No allocation, no blocking, no printf, no double (the loop's gains are floats in the ClockDll). It runs from flash, once a
// block. Cost [estimate, to be measured on the unit with `cpu`]: about 1 k cycles a block idle, 2 to 4 k following (four 64-bit divisions and two 32-step Q32 divisions), that is
// under 1.5 % of the block.
//
// SIZE. About 24 KB when instantiated (the emitter's rings 5.3 KB, USB out 5.2 KB, the router 6.5 KB, the two midiRt rings 4.2 KB, the rings of the kit side 0.7 KB): a static in
// SRAM_U (platform/mk66/src/main.cpp), where the interrupts' latency is what it was measured with in track3r-os.

#include <o3sdk/core/midi_types.h>
#include <o3sdk/core/qtime.h>
#include <o3sdk/core/ring.h>
#include <o3sdk/hal/midi.h>
#include <o3sdk/platform/mk66/audio/events.h>
#include <o3sdk/platform/mk66/midi/clock_dll.h>
#include <o3sdk/platform/mk66/midi/config.h>
#include <o3sdk/platform/mk66/midi/din.h>
#include <o3sdk/platform/mk66/midi/in.h>
#include <o3sdk/platform/mk66/midi/message.h>
#include <o3sdk/platform/mk66/midi/out.h>
#include <o3sdk/platform/mk66/midi/time_ref.h>
#include <o3sdk/platform/mk66/midi/usb_out.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::platform::mk66::midi {

// What the platform gives the service (the hooks of seq::MidiLinkHooks, with the lead).
struct MidiServiceHooks {
  // The cycle-counter value at which the first sample of the block being rendered is HEARD (the boundary stamp of its render plus the render-ahead of the TX ring). Called once per
  // block, in the render context. Null: the timeline's own cycles (the host tests drive the emitter with timeline times directly).
  std::uint32_t (*heard_at)(void* ctx) noexcept = nullptr;
  // Pends the DIN emitter's interrupt. Called in the render context after items went to the DIN ring. May be null (a platform without a DIN port, a test).
  void (*kick)(void* ctx) noexcept = nullptr;
  // Messages the interrupts lost to a full ring (the DIN receive ring, the USB one): the sum of the drivers' counters, added to Status::dropped. May be null.
  std::uint32_t (*isr_dropped)(void* ctx) noexcept = nullptr;
  // Whether the USB MIDI port is alive: the interface is bound and configured by a host that is there (not suspended), so that start-of-frame interrupts drain what is queued for it.
  // Called once per block, in the render context. While it says no, nothing is routed to the USB port (the port's ring would fill, and the router's flush stops at a full head, which
  // would hold the DIN port's output back with it); what was queued for USB is discarded when the port goes and when it comes back. Null: always alive (the host tests).
  bool (*usb_active)(void* ctx) noexcept = nullptr;
  void* ctx = nullptr;
  // How far `heard_at` is ahead of the render's boundary stamp, in cycles (the platform's midi_cfg::kAudioLeadCycles): a note that arrives at a wire time is given the frame that
  // keeps its latency constant (see begin_block). 0 with the default `heard_at`.
  std::uint32_t lead_cycles = 0;
};

// A running-status parser for the channel messages of one port (any status 0x80 .. 0xEF). Real-time bytes (0xF8 ..) are ignored without touching the state (they may sit inside a
// message); a system common or SysEx byte cancels running status (so SysEx data is never read as a message); a status byte starts a message and data bytes complete it, with running
// status the next pair is another message of the same kind. render only.
class ChannelParser {
 public:
  // True when `b` completes a message: status, d1 and d2 (0 for a one-data-byte message) are written.
  bool feed(std::uint8_t b, std::uint8_t& status, std::uint8_t& d1, std::uint8_t& d2) noexcept {
    if (is_realtime(b)) {
      return false;
    }
    if (is_status(b)) {
      status_ = b < 0xF0u ? b : std::uint8_t{0};
      have_ = 0;
      return false;
    }
    if (status_ == 0u) {
      return false;  // a data byte of SysEx or system common, or with no status yet
    }
    if (data_bytes_of(status_) == 1u) {
      status = status_;
      d1 = b;
      d2 = 0;
      return true;
    }
    if (have_ == 0u) {
      first_ = b;
      have_ = 1;
      return false;
    }
    have_ = 0;
    status = status_;
    d1 = first_;
    d2 = b;
    return true;
  }
  void reset() noexcept { *this = ChannelParser{}; }

 private:
  std::uint8_t status_ = 0;  // the running channel status, 0 when there is none
  std::uint8_t have_ = 0;    // data bytes of a two-byte message seen so far
  std::uint8_t first_ = 0;
};

class MidiService final : public hal::midi::Device {
 public:
  static constexpr std::uint32_t kDrainItems = 128;      // items taken from each ring per block (the rest waits)
  static constexpr std::uint32_t kEventCap = 8;          // transport events per block (a Start, an SPP and a Stop are three)
  static constexpr std::uint32_t kPassCap = 32;          // channel bytes per drain call; more wait in the ring
  static constexpr std::uint32_t kStaleClocks = 48;      // a master quieter than this many clock periods is gone (two beats)
  static constexpr std::uint32_t kPollRing = 64;         // channel messages the app has not polled yet; the newest past it is dropped
  static constexpr std::uint32_t kSendRing = 32;         // send()s waiting for the render
  static constexpr std::uint32_t kSendPerBlock = 16;     // send()s a block takes
  static constexpr std::uint32_t kHeldCap = 32;          // notes past `cap` that wait for the next block
  static constexpr std::uint32_t kMaxMessagesPerBlock = 32;  // the hooks' messages end_block takes (the engine's kMaxMidiOut)
  static constexpr std::uint32_t kRxLog = 16;            // the last non-timing bytes received, for the console
  static constexpr std::uint32_t kTxLog = 8;             // the last messages sent, for the console
  static constexpr std::uint32_t kNominalBpmX100 = 12000;  // the loop's period until two clocks have been seen
  static constexpr std::uint32_t kTicksPerClock = 40;    // 960 ppqn / 24
  static constexpr std::uint32_t kTicksPerBeat = 240;    // an SPP beat is six clocks
  static constexpr std::uint64_t kPendingRunSamples = 88200;  // a Start waits this long (2 s) for the loop to attach, then is forgotten
  static constexpr std::uint32_t kSlewPercent = 25;      // the increment may differ from the loop's by this much while the position chases it
  // The loop's origin follows the timeline while no master is there, so that no difference of it overflows: it is moved when it is this far back (the loop rebases its own at
  // 2^30 samples, 6.7 hours at 44.1 kHz, and a Q32 span overflows at 2^31).
  static constexpr std::uint64_t kOriginRefreshSamples = (std::uint64_t{1} << 30) + (std::uint64_t{1} << 29);

  // `first_sample`: the timeline sample the first block starts at. 0 on the unit; the tests start late to put the clock-follow loop's arithmetic at the size it has after hours.
  explicit MidiService(const MidiServiceHooks& hooks = MidiServiceHooks{}, std::uint64_t first_sample = 0) noexcept;

  // ---- hal::midi::Device: render side -----------------------------------------------------------------------------------------
  unsigned begin_block(std::uint32_t frames, core::MidiMessage* in, unsigned cap, core::ClockFrame& clock) noexcept override;
  void end_block(const core::ClockFrame& transport, const core::MidiMessage* out, unsigned n) noexcept override;

  // ---- hal::midi::Device: main loop -------------------------------------------------------------------------------------------
  void set_config(const hal::midi::Config& c) noexcept override;
  hal::midi::Status status() const noexcept override;
  bool poll(core::MidiMessage& m) noexcept override;
  bool send(const core::MidiMessage& m) noexcept override;
  void request(core::TransportRequest r) noexcept override;

  // ---- the interrupts' ends (the platform's drivers hold these for good) -------------------------------------------------------
  DinEmitter& din() noexcept { return din_; }
  UsbMidiOut& usb_out() noexcept { return usb_out_; }
  RtInRing& din_in() noexcept { return in_ring_[0]; }
  RtInRing& usb_in() noexcept { return in_ring_[1]; }
  const TimeRef& time_ref() const noexcept { return ref_; }

  // ---- reading, for the console and the tests ---------------------------------------------------------------------------------
  const MidiOut& out() const noexcept { return out_; }
  MidiOut& out() noexcept { return out_; }
  const MidiIn& in() const noexcept { return in_; }
  const ClockDll& dll() const noexcept { return dll_; }

  struct Stats {
    std::uint32_t blocks = 0;
    std::uint32_t configs = 0;           // configurations applied
    std::uint32_t stale_releases = 0;    // the master went quiet while followed
    std::uint32_t follow_attached = 0;
    std::uint32_t rings_discarded = 0;   // bytes thrown away at the first block, and when the followed source changed (what piled up before)
    std::uint32_t messages = 0;          // channel messages received
    std::uint32_t notes = 0;             // of them: the ones handed to the engine (notes, all sound off, all notes off)
    std::uint32_t held = 0;              // notes that did not fit `cap` and waited a block
    std::uint32_t held_dropped = 0;      // lost: the wait list was full
    std::uint32_t poll_dropped = 0;      // lost: the app did not poll (the ring is kPollRing deep)
    std::uint32_t usb_skipped = 0;       // messages addressed to the USB port alone while it was not alive: dropped
    std::uint32_t bad_out = 0;           // messages of the hooks or of send() that are not MIDI messages
    std::uint32_t sent = 0;              // messages of the hooks and of send() given to the router
    std::uint32_t transport_events = 0;  // Start, Continue, Stop and SPP applied
    std::uint32_t transport_ignored = 0; // the same with TRANSPORT IN off
    std::uint32_t requests = 0;          // Play / Stop requests acted on
    std::uint32_t kicks = 0;
    std::uint32_t starts_out = 0;        // Start, Continue and Stop bytes put in the router
    std::uint32_t clocks_out = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

  // A snapshot of what the console shows (`midi`). Main loop; the figures are the render's words from a block ago at worst, and may tear against each other.
  struct Diag {
    hal::midi::Status status;
    hal::midi::Config config;      // the main loop's last set_config
    bool dll_phase = false;        // a clock has been seen
    bool dll_running = false;      // the loop has finished its fit
    std::uint32_t bpm_x100 = 0;    // the loop's tempo, whether or not it is followed
    std::uint32_t clocks = 0;      // clocks of the followed port the loop has been given
    std::uint32_t error_us = 0;    // |the loop's last error| in microseconds
    Stats service;
    MidiIn::Stats in;
    MidiOut::Stats out;
    ClockDll::Stats dll;
    std::uint32_t send_dropped = 0;
    std::uint32_t rx_log_n = 0;                         // bytes ever logged
    std::uint8_t rx_log[kRxLog][2] = {};                // oldest first among the last kRxLog: {port, byte}
    std::uint32_t tx_log_n = 0;
    std::uint8_t tx_log[kTxLog][4] = {};                // oldest first among the last kTxLog: {ports, status, d1, d2}
  };
  void diag(Diag& d) const noexcept;

  // Timeline arithmetic the tests and the console share. The first frame f of a block of `frames` with pos + f * inc >= tick (Q32.32 ticks, whole `tick`), or -1: dsp::frame_at, which
  // the tests hold equal.
  static int frame_of(std::uint64_t pos, std::uint64_t inc, std::uint32_t frames, std::uint64_t tick) noexcept;

 private:
  void apply_config(const hal::midi::Config& c) noexcept;
  void apply_ports() noexcept;
  std::uint8_t out_ports() const noexcept;
  void discard_inputs() noexcept;
  unsigned drain_inputs(core::MidiMessage* in, unsigned cap, std::uint32_t frames) noexcept;
  void apply_event(const TransportEvent& e) noexcept;
  void take_request() noexcept;
  void manage_follow() noexcept;
  void release_follow() noexcept;
  void reset_loop() noexcept;
  core::ClockFrame make_clock(std::uint32_t frames) noexcept;
  std::int64_t master_ticks_at(std::uint64_t sample) const noexcept;
  std::int64_t clock_at(core::SQ32 t) const noexcept;
  bool to_event(const core::MidiMessage& m, audio::SeqEvent& ev) const noexcept;
  void send_message(const core::MidiMessage& m, std::uint64_t sample) noexcept;
  void send_transport(audio::EventKind kind, std::uint64_t sample, std::int32_t value) noexcept;
  void send_clocks(const core::ClockFrame& t, std::uint32_t frames) noexcept;
  void publish_status() noexcept;
  void log_tx(std::uint8_t ports, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) noexcept;

  MidiServiceHooks hooks_;
  std::uint32_t lead_samples_ = 0;        // hooks_.lead_cycles in whole samples
  std::int64_t inc_min_ = 0;              // the engine's slowest and fastest increments (dsp::kMinTempoMilliBpm, kMaxTempoMilliBpm): the frames' inc is held between them
  std::int64_t inc_max_ = 0;
  core::Q32 nominal_period_ = 0;          // the loop's period until two clocks have been seen

  MidiOut out_;
  DinEmitter din_;
  UsbMidiOut usb_out_;
  RtInRing in_ring_[kPortCount];
  MidiIn in_;
  ClockDll dll_;
  ClockMap map_;
  TimeRef ref_;
  ChannelParser parser_[kPortCount];

  // The settings: the main loop writes the box, the render keeps its own applied copy.
  SeqLockBox<hal::midi::Config> box_;
  std::uint32_t box_seen_ = 0;
  hal::midi::Config cfg_{};

  // The kit side. poll_ring_: the render is the producer, the main loop (poll) the consumer; send_ring_ the other way round.
  core::SpscRing<core::MidiMessage, kPollRing> poll_ring_;
  core::SpscRing<core::MidiMessage, kSendRing> send_ring_;
  std::atomic<std::uint8_t> request_{0};  // core::TransportRequest: the main loop stores, the render takes (exchange)
  std::atomic<std::uint32_t> send_dropped_{0};

  // Render-only state.
  bool usb_up_ = true;                    // hooks_.usb_active as of the last end_block (a port that is not alive is not routed to: out_ports())
  bool started_ = false;                  // the first block has run (the rings it found are discarded)
  std::uint64_t block_start_ = 0;         // the timeline sample the block now rendering starts at
  std::uint32_t frames_ = 0;              // its length
  core::MidiMessage held_[kHeldCap] = {};
  unsigned held_n_ = 0;
  std::uint32_t handed_ = 0;              // MidiOut's handed counter at the last block: a change means the ports have something new
  // The follow state. following_: the frames have `follow`. playing_: the master's run (valid while following_).
  bool following_ = false;
  bool playing_ = false;
  bool begin_run_ = false;                // the run began in this block's events: its first frame is pos base_
  bool begin_started_ = false;            // ... and it is a Start (pos 0, `started`), not a Continue
  bool stopped_pending_ = false;          // the next frame carries `stopped`
  std::uint64_t pending_since_ = 0;       // the timeline sample a Start or Continue was applied at: a run that is still waiting for the loop is dropped after kPendingRunSamples
  bool want_play_ = false;                // a Play request waits for the master's next clock
  std::uint64_t play_floor_ = 0;          // the loop's tick when the Play was asked
  std::uint64_t start_tick_ = 0;          // the loop's clock number the run's position is counted from
  std::uint64_t base_ticks_ = 0;          // the position at that clock, Q32.32 ticks (whole ticks << 32): 0 for a Start, the song position for a Continue
  std::uint64_t resume_ticks_ = 0;        // where a Continue without an SPP resumes: where the master stopped, or the last SPP (whole ticks)
  std::uint64_t pos_ = 0;                 // the position at frame 0 of the next frame (Q32.32 ticks)
  std::int64_t prev_inc_ = 0;             // the increment of the last playing frame (make_clock keeps one block's tick ownership apart from its neighbour's)
  bool prev_playing_ = false;             // the last frame was a followed, playing one
  bool out_playing_ = false;              // the last block's transport was playing (end_block)
  Stats stats_{};

  // Published for the main loop (the render writes, the main loop reads): relaxed single words.
  std::atomic<std::uint32_t> st_flags_{0};
  std::atomic<std::uint32_t> st_tempo_{0};
  std::atomic<std::uint32_t> st_rx_{0};
  std::atomic<std::uint32_t> st_tx_{0};
  std::atomic<std::uint32_t> st_dropped_{0};
  std::atomic<std::uint32_t> st_bpm_{0};
  std::atomic<std::uint32_t> st_clocks_{0};
  std::atomic<std::uint32_t> st_error_us_{0};
  std::atomic<std::uint32_t> st_phase_{0};
  // The logs (render writes; slot words are {n, port, byte} packed).
  std::atomic<std::uint32_t> rx_log_[kRxLog] = {};
  std::atomic<std::uint32_t> rx_log_n_{0};
  std::atomic<std::uint32_t> tx_log_[kTxLog] = {};
  std::atomic<std::uint32_t> tx_log_n_{0};
};

}  // namespace o3sdk::platform::mk66::midi
