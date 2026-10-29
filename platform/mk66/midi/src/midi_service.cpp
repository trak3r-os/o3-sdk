// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI in and out for the kit (midi/midi_service.h). The drain, the follow state, the flush and the kick are the render side of track3r-os's seq::MidiLink
// (seq/src/midi_link.cpp) with the sequencer taken out; the position handed to the engine, the channel messages and the transport out are new.
#include <o3sdk/platform/mk66/midi/midi_service.h>

#include <o3sdk/dsp/transport.h>

namespace o3sdk::platform::mk66::midi {

namespace {

using audio::EventKind;

core::SQ32 floor_div(core::SQ32 a, core::SQ32 b) noexcept {  // b > 0
  core::SQ32 q = a / b;
  if ((a % b) != 0 && a < 0) {
    --q;
  }
  return q;
}

constexpr std::int64_t kOne = std::int64_t{1} << 32;  // one sample, or one tick, in Q32.32

std::int64_t clamp64(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// A saturating subtraction of a lead from a timeline sample (a due time cannot reach before the start of the timeline).
std::uint64_t back(std::uint64_t sample, std::uint64_t lead) noexcept { return sample > lead ? sample - lead : 0u; }

bool is_note(const core::MidiMessage& m) noexcept {
  const std::uint8_t kind = core::midi_kind(m.status);
  return kind == core::kMidiNoteOn || kind == core::kMidiNoteOff ||
         (kind == core::kMidiControl && (m.data1 == core::kCcAllSoundOff || m.data1 == core::kCcAllNotesOff));
}

}  // namespace

MidiService::MidiService(const MidiServiceHooks& hooks, std::uint64_t first_sample) noexcept : hooks_(hooks), block_start_(first_sample) {
  lead_samples_ = static_cast<std::uint32_t>(core::q32_round(core::q32_from_cycles(hooks_.lead_cycles)));  // (whole blocks: the cycles of a block are rounded to whole cycles)
  nominal_period_ = 8u * core::samples_per_tick_q32(kNominalBpmX100);
  inc_min_ = static_cast<std::int64_t>(dsp::ticks_per_sample_q32(dsp::kMinTempoMilliBpm));
  inc_max_ = static_cast<std::int64_t>(dsp::ticks_per_sample_q32(dsp::kMaxTempoMilliBpm));
  out_.attach(&din_, &usb_out_);
  apply_config(cfg_);  // the defaults, into the router and MIDI in
  stats_.configs = 0;
  dll_.reset(0, nominal_period_);  // (the timeline's zero: see reset_loop)
}

// ---- hal::midi::Device: main loop ------------------------------------------------------------------------------------------------

void MidiService::set_config(const hal::midi::Config& c) noexcept { box_.write(hal::midi::sanitized(c)); }

hal::midi::Status MidiService::status() const noexcept {
  hal::midi::Status s;
  const std::uint32_t f = st_flags_.load(std::memory_order_relaxed);
  s.following = (f & 1u) != 0u;
  s.locked = (f & 2u) != 0u;
  s.tempo_mbpm = st_tempo_.load(std::memory_order_relaxed);
  s.rx = st_rx_.load(std::memory_order_relaxed);
  s.tx = st_tx_.load(std::memory_order_relaxed);
  s.dropped = st_dropped_.load(std::memory_order_relaxed) + send_dropped_.load(std::memory_order_relaxed);
  return s;
}

bool MidiService::poll(core::MidiMessage& m) noexcept { return poll_ring_.pop(m); }

bool MidiService::send(const core::MidiMessage& m) noexcept {
  const hal::midi::Config& c = box_.last_written();  // the main loop is the box's writer: its own view
  const std::uint8_t ports = static_cast<std::uint8_t>((m.ports != 0u ? m.ports : c.out_ports) & core::kMidiAllPorts);
  audio::SeqEvent probe;
  if (ports == 0u || !to_event(m, probe)) {
    return false;
  }
  core::MidiMessage x = m;
  x.frame = 0;
  if (!send_ring_.push(x)) {
    send_dropped_.store(send_dropped_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);  // one writer
    return false;
  }
  return true;
}

void MidiService::request(core::TransportRequest r) noexcept { request_.store(static_cast<std::uint8_t>(r), std::memory_order_relaxed); }

// ---- the block ---------------------------------------------------------------------------------------------------------------------

unsigned MidiService::begin_block(std::uint32_t frames, core::MidiMessage* in, unsigned cap, core::ClockFrame& clock) noexcept {
  frames_ = frames;
  ++stats_.blocks;
  // Without a platform hook the counter is taken to read the timeline's own cycles (the host tests drive the emitter with timeline times directly).
  const std::uint32_t heard = hooks_.heard_at != nullptr ? hooks_.heard_at(hooks_.ctx) : static_cast<std::uint32_t>(core::cycles_from_samples(block_start_));
  ref_.publish(block_start_, heard);  // the interrupts' time base
  map_.set_ref(block_start_, heard);  // the stamps of incoming bytes
  if (!started_) {
    // What the interrupts stamped before the first block (the rings are live from the driver's start, the audio from its own) belongs to no timeline.
    started_ = true;
    discard_inputs();
  }
  if (!dll_.has_phase() && block_start_ - dll_.origin() >= kOriginRefreshSamples) {
    reset_loop();  // (an origin that follows the timeline while no clock comes, so no difference of it overflows after days)
  }
  hal::midi::Config c;
  if (box_.read_if_changed(c, box_seen_)) {
    apply_config(c);
  }
  take_request();
  const unsigned n = drain_inputs(in, cap, frames);
  manage_follow();
  clock = make_clock(frames);
  return n;
}

void MidiService::end_block(const core::ClockFrame& t, const core::MidiMessage* out, unsigned n) noexcept {
  const std::uint32_t frames = frames_ != 0u ? frames_ : 1u;
  const bool usb_up = hooks_.usb_active == nullptr || hooks_.usb_active(hooks_.ctx);
  if (usb_up != usb_up_) {
    // The USB port came or went: what its ring holds is from another time (nobody drained it, or somebody else was listening), the next start of frame discards it, and while it is
    // not alive nothing new goes there (out_ports()).
    usb_up_ = usb_up;
    usb_out_.request_flush();
    apply_ports();
  }
  // The hooks' messages at their frames, then the main loop's at frame 0.
  const unsigned count = n < kMaxMessagesPerBlock ? n : kMaxMessagesPerBlock;
  for (unsigned i = 0; i < count; ++i) {
    send_message(out[i], block_start_ + (out[i].frame < frames ? out[i].frame : frames - 1u));
  }
  core::MidiMessage m;
  for (std::uint32_t k = 0; k < kSendPerBlock && send_ring_.pop(m); ++k) {
    send_message(m, block_start_);
  }

  // The transport the engine ran, to the ports: Start at `started`, SPP + Continue where a run begins elsewhere, Stop at `stopped`, the clock while it plays. The Start
  // goes out a quarter block (16 samples, 0.36 ms) before the first clock and the SPP a block before the Continue, as track3r-os's sequencer puts them
  // (Sequencer::begin_run: start_lead), so that a receiver has them before the first note and the first clock is not behind a byte; the router's horizon is two blocks
  // beyond the end of this one, so a due time a block back is still ahead of the wire.
  if (out_ports() != 0u && t.inc != 0u) {
    if (cfg_.transport_out) {
      if (t.stopped) {
        send_transport(EventKind::MidiStop, t.playing ? back(block_start_, frames / 2u) : block_start_, 0);  // (a restart in one block: the Stop before the Start)
      }
      if (t.playing && t.started) {
        send_transport(EventKind::MidiStart, back(block_start_, frames / 4u), 0);
      } else if (t.playing && !out_playing_) {
        const std::uint64_t beats = (t.pos >> 32) / kTicksPerBeat;
        send_transport(EventKind::MidiSpp, back(block_start_, frames), static_cast<std::int32_t>(beats & 0x3FFFu));
        send_transport(EventKind::MidiContinue, back(block_start_, frames / 4u), 0);
      }
    }
    if (cfg_.clock_out && t.playing) {
      send_clocks(t, frames);
    }
  }
  out_playing_ = t.playing;
  if (t.stopped && !t.playing) {
    // The hooks ended their notes at the stop (they saw `stopped`); what is still owed after them is a note nobody told the router about.
    out_.stop_all(core::cycles_from_samples(block_start_));
  }
  out_.flush(core::cycles_from_samples(block_start_ + frames));

  const std::uint32_t handed = out_.stats().handed;
  if (handed != handed_) {
    handed_ = handed;
    if (hooks_.kick != nullptr) {
      hooks_.kick(hooks_.ctx);  // the emitter's interrupt decides from its queues: a pend is always safe
      ++stats_.kicks;
    }
  }
  block_start_ += frames;
  publish_status();
}

// ---- configuration -----------------------------------------------------------------------------------------------------------------

void MidiService::apply_config(const hal::midi::Config& c) noexcept {
  const hal::midi::Config old = cfg_;
  cfg_ = c;
  ++stats_.configs;

  // The router: where notes, clock and transport go (the switches for the clock and the transport are end_block's: MidiOut sends all five of them on clock_ports).
  MidiOut::Config oc = out_.config();
  oc.cc_channel = static_cast<std::uint8_t>(c.out_channel - 1u);
  out_.configure(oc);
  apply_ports();

  // MIDI in: configure() forgets an armed Start, so it is called only when something it holds changed.
  const Port port = c.follow_port == hal::midi::Port::Usb ? Port::Usb : Port::Din;
  MidiIn::Config ic = in_.config();
  if (ic.clock_enabled != c.follow || ic.clock_port != port) {
    ic.clock_enabled = c.follow;
    ic.clock_port = port;
    in_.configure(ic);
  }

  const bool source_changed = old.follow != c.follow || (c.follow && old.follow_port != c.follow_port);
  if (source_changed) {
    // Another timeline source: what the rings hold is the old one's (or from before anybody looked), the parsers' state belongs to it, and the loop starts over. A
    // transport that follows lets go (the engine stops it, once).
    discard_inputs();
    in_.reset();
    release_follow();
    reset_loop();
  }
}

// The ports the router sends to: the user's, less the USB port while it is not alive (usb_up_).
std::uint8_t MidiService::out_ports() const noexcept {
  return static_cast<std::uint8_t>(cfg_.out_ports & (usb_up_ ? core::kMidiAllPorts : core::kMidiDin));
}

void MidiService::apply_ports() noexcept {
  MidiOut::Config oc = out_.config();
  oc.note_ports = out_ports();
  oc.clock_ports = out_ports();
  out_.configure(oc);
}

// The loop starts over. Its origin stays where it was, so that a loop that has run for hours is as it would be (the tests count on it), unless the origin is old enough to
// be in danger: a Q32 span of the loop overflows at 2^31 samples, and the loop rebases itself at 2^30 once it runs.
void MidiService::reset_loop() noexcept {
  const bool refresh = block_start_ - dll_.origin() >= kOriginRefreshSamples;
  dll_.reset(refresh ? block_start_ : dll_.origin(), nominal_period_);
}

// ---- input -------------------------------------------------------------------------------------------------------------------------

void MidiService::discard_inputs() noexcept {
  RtItem it;
  for (std::uint32_t p = 0; p < kPortCount; ++p) {
    for (std::uint32_t i = 0; i < kRtInRingSize && in_ring_[p].pop(it); ++i) {
      ++stats_.rings_discarded;
    }
    parser_[p].reset();
  }
}

unsigned MidiService::drain_inputs(core::MidiMessage* in, unsigned cap, std::uint32_t frames) noexcept {
  // The notes that did not fit the last block come first, at frame 0.
  unsigned n = 0;
  const unsigned take = held_n_ < cap ? held_n_ : cap;
  for (; n < take; ++n) {
    in[n] = held_[n];
    in[n].frame = 0;
  }
  for (unsigned i = take; i < held_n_; ++i) {
    held_[i - take] = held_[i];
  }
  held_n_ -= take;

  TransportEvent ev[kEventCap];
  RtItem pass[kPassCap];
  std::uint32_t nev = 0;
  for (std::uint32_t p = 0; p < kPortCount; ++p) {
    // Both rings are drained in every mode, so that neither fills: with the clock not followed the timing bytes are counted and dropped by MidiIn.
    const MidiIn::Drained d = in_.drain(in_ring_[p], map_, dll_, kDrainItems, ev + nev, kEventCap - nev, pass, kPassCap);
    nev += d.events;
    for (std::uint32_t i = 0; i < d.passed; ++i) {
      const RtItem& it = pass[i];
      const std::uint32_t slot = rx_log_n_.load(std::memory_order_relaxed);
      rx_log_[slot % kRxLog].store(static_cast<std::uint32_t>(it.port) | (static_cast<std::uint32_t>(it.byte) << 8), std::memory_order_relaxed);
      rx_log_n_.store(slot + 1u, std::memory_order_relaxed);  // one writer
      // (The port is the item's: MidiIn keeps one item back when `pass` is full, and gives it to the next drain call, whichever ring that is for.)
      const std::uint32_t port = it.port < kPortCount ? it.port : 0u;
      core::MidiMessage m{};
      if (!parser_[port].feed(it.byte, m.status, m.data1, m.data2)) {
        continue;
      }
      m.ports = hal::midi::port_bit(port == 0u ? hal::midi::Port::Din : hal::midi::Port::Usb);
      m.frame = 0;
      ++stats_.messages;
      st_rx_.store(st_rx_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
      if (!poll_ring_.push(m)) {
        ++stats_.poll_dropped;
      }
      if (!is_note(m)) {
        continue;
      }
      // The frame of this block the message's arrival maps to, with a constant latency: the audio clock puts the stamp where its sound would be HEARD, which is the TX
      // ring's render-ahead (lead_samples_) before the block now rendering, and the drain collects what arrived in the block before, so a whole block more. The sum is
      // the same for every note: a constant latency of (render-ahead + a block) from the wire to the sound, and no jitter.
      const core::SQ32 rel = map_.to_rel(it.cycles, block_start_);
      const std::int64_t frame = (rel >> core::kQ32Frac) + static_cast<std::int64_t>(lead_samples_) + static_cast<std::int64_t>(frames);
      m.frame = static_cast<std::uint16_t>(clamp64(frame, 0, static_cast<std::int64_t>(frames) - 1));
      ++stats_.notes;
      if (n < cap) {
        in[n++] = m;
      } else if (held_n_ < kHeldCap) {
        held_[held_n_++] = m;
        ++stats_.held;
      } else {
        ++stats_.held_dropped;
      }
    }
  }
  for (std::uint32_t i = 0; i < nev; ++i) {
    apply_event(ev[i]);
  }
  return n;
}

// ---- the master's transport -----------------------------------------------------------------------------------------------------------

void MidiService::apply_event(const TransportEvent& e) noexcept {
  if (!cfg_.transport_in) {
    ++stats_.transport_ignored;  // TRANSPORT IN off: the clock is followed (the loop), the transport bytes are not acted on
    return;
  }
  ++stats_.transport_events;
  switch (e.kind) {
    case TransportEvent::Kind::Start:
      playing_ = true;
      begin_run_ = true;
      begin_started_ = true;
      start_tick_ = e.tick;
      base_ticks_ = 0;
      resume_ticks_ = 0;
      want_play_ = false;
      pending_since_ = block_start_;
      break;
    case TransportEvent::Kind::Continue:
      playing_ = true;
      begin_run_ = true;
      begin_started_ = resume_ticks_ == 0u;  // a Continue before anything has run starts like a Start (track3r-os OS-MIDISLV)
      start_tick_ = e.tick;
      base_ticks_ = resume_ticks_ << 32;
      want_play_ = false;
      pending_since_ = block_start_;
      break;
    case TransportEvent::Kind::Stop:
      if (!following_) {
        // A run that was waiting for the loop to attach (see make_clock) is called off: the engine never saw it, so there is no stop to report.
        playing_ = false;
        begin_run_ = false;
        want_play_ = false;
        break;
      }
      if (playing_) {
        resume_ticks_ = pos_ >> 32;  // a Continue without a Song Position resumes here
        playing_ = false;
        begin_run_ = false;
        stopped_pending_ = true;
      }
      want_play_ = false;
      break;
    case TransportEvent::Kind::Spp:
      if (!playing_) {
        resume_ticks_ = std::uint64_t{e.spp} * kTicksPerBeat;
      }
      break;
  }
}

// The engine's Play or Stop while it follows the master (Engine::take_transport_request): Play starts the run on the master's next clock, Stop stops it.
void MidiService::take_request() noexcept {
  const auto r = static_cast<core::TransportRequest>(request_.exchange(static_cast<std::uint8_t>(core::TransportRequest::None), std::memory_order_relaxed));
  if (r == core::TransportRequest::None || !following_) {
    return;
  }
  ++stats_.requests;
  if (r == core::TransportRequest::Play) {
    if (!playing_ && !want_play_) {
      want_play_ = true;
      play_floor_ = dll_.tick();  // a clock the loop accepts after this is the next one
    }
  } else {
    if (playing_) {
      resume_ticks_ = pos_ >> 32;
      playing_ = false;
      begin_run_ = false;
      stopped_pending_ = true;
    }
    want_play_ = false;
  }
}

// ---- following -----------------------------------------------------------------------------------------------------------------------

// Lets go of the master: the next frames have no `follow`, which is how the engine knows to stop its transport (once) and take its own tempo back.
void MidiService::release_follow() noexcept {
  following_ = false;
  playing_ = false;
  begin_run_ = false;
  stopped_pending_ = false;
  want_play_ = false;
  if (dll_.has_phase()) {
    reset_loop();  // a master that comes back is acquired afresh
  }
}

void MidiService::manage_follow() noexcept {
  if (!cfg_.follow) {
    if (following_) {
      release_follow();
    }
    return;
  }
  if (!dll_.has_phase()) {
    if (following_) {
      release_follow();
    }
    return;
  }
  // Two clocks make a period; the first one's is the loop's nominal.
  const core::SQ32 now_rel = (static_cast<core::SQ32>(block_start_) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const bool stale = dll_.stale(now_rel, kStaleClocks);
  const bool alive = dll_.tick() >= 1u && !stale;
  if (following_) {
    if (!alive) {
      ++stats_.stale_releases;  // the master went quiet: the transport stops, the loop is let go
      release_follow();
    }
  } else if (alive) {
    following_ = true;
    ++stats_.follow_attached;
  } else if (stale) {
    playing_ = false;  // (and the run it had begun, if it never attached)
    begin_run_ = false;
    reset_loop();  // a stale loop with nobody attached: forget it so that a master that comes back is acquired afresh
  }
}

// The fractional number of the loop's clock at `t` (Q32 samples relative to the loop's origin), Q32.32 clocks: the integer n with time_of_tick(n) <= t < time_of_tick(n + 1) and
// the fraction of that span. The loop's time_of_tick is the filtered time of its last clock, the prediction for the next, and the period beyond.
std::int64_t MidiService::clock_at(core::SQ32 t) const noexcept {
  const core::SQ32 period = static_cast<core::SQ32>(dll_.period());
  if (period <= 0) {
    return 0;
  }
  const std::uint64_t last = dll_.tick();
  std::int64_t n = static_cast<std::int64_t>(last) + 1 + floor_div(t - dll_.time_of_tick(last + 1u), period);
  n = n < 0 ? 0 : n;
  for (unsigned i = 0; i < 4u && n > 0 && dll_.time_of_tick(static_cast<std::uint64_t>(n)) > t; ++i) {
    --n;
  }
  for (unsigned i = 0; i < 4u && dll_.time_of_tick(static_cast<std::uint64_t>(n) + 1u) <= t; ++i) {
    ++n;
  }
  const core::SQ32 t0 = dll_.time_of_tick(static_cast<std::uint64_t>(n));
  const core::SQ32 span = dll_.time_of_tick(static_cast<std::uint64_t>(n) + 1u) - t0;
  std::int64_t frac = 0;
  if (t > t0 && span > 0) {
    const core::SQ32 into = t - t0;
    frac = static_cast<std::int64_t>(core::q32_div(static_cast<std::uint64_t>(into < span ? into : span - 1), static_cast<std::uint64_t>(span)));
  }
  return (n << core::kQ32Frac) + frac;
}

// The master's position at the start of timeline sample `sample`, Q32.32 ticks of the engine's grid: the song position of the run's first clock plus the clocks since it, 40 ticks each.
std::int64_t MidiService::master_ticks_at(std::uint64_t sample) const noexcept {
  const core::SQ32 t = (static_cast<core::SQ32>(sample) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const std::int64_t since = clock_at(t) - static_cast<std::int64_t>(start_tick_ << core::kQ32Frac);  // Q32.32 clocks
  const std::int64_t base = static_cast<std::int64_t>(base_ticks_);
  const std::int64_t ticks = base + since * static_cast<std::int64_t>(kTicksPerClock);
  return ticks < base ? base : ticks;
}

core::ClockFrame MidiService::make_clock(std::uint32_t frames) noexcept {
  core::ClockFrame c;
  const bool prev_playing = prev_playing_;  // the block before was a followed, playing one
  prev_playing_ = false;                    // (set again at the end of a playing block)
  if (!following_) {
    // A Start or Continue that arrived before the loop attached (the master's first clocks, or the first after a quiet spell: two clocks make a period) is the master's run that
    // begins when the loop does, as track3r-os attaches on a Start (seq/midi_link.cpp drain_inputs); it is held, not cleared, until the attach (manage_follow), a Stop, a release,
    // FOLLOW off, or kPendingRunSamples without the loop attaching (a master that started and died).
    if (!cfg_.follow || block_start_ - pending_since_ > kPendingRunSamples) {
      playing_ = false;
      begin_run_ = false;
    }
    stopped_pending_ = false;
    want_play_ = false;
    st_tempo_.store(0, std::memory_order_relaxed);
    return c;
  }
  // The loop's tempo and increment: 40 ticks a clock, the period in samples a clock.
  const std::uint32_t bpm_x100 = dll_.bpm_x100();
  // Held between the slowest and the fastest tempo the engine accepts, as Engine::follow holds it (kit/dsp/src/engine.cpp apply_follow): what the engine renders must be what this
  // service advanced its position by, or the tick ownership below no longer holds above 300 or below 20 BPM.
  const std::int64_t inc_ref = clamp64(static_cast<std::int64_t>(core::q32_div(std::uint64_t{kTicksPerClock} << core::kQ32Frac, static_cast<std::uint64_t>(dll_.period()))), inc_min_, inc_max_);
  c.follow = true;
  c.tempo_mbpm = bpm_x100 * 10u;
  c.inc = static_cast<std::uint64_t>(inc_ref);
  st_tempo_.store(c.tempo_mbpm, std::memory_order_relaxed);

  if (want_play_ && dll_.tick() > play_floor_) {  // Play: on the master's next clock
    want_play_ = false;
    playing_ = true;
    begin_run_ = true;
    begin_started_ = true;
    start_tick_ = dll_.tick();
    base_ticks_ = 0;
    resume_ticks_ = 0;
  }
  c.stopped = stopped_pending_;
  stopped_pending_ = false;
  if (!playing_) {
    return c;
  }
  const bool begin = begin_run_ || !prev_playing;  // no block of this run before this one: nothing shares a tick with it
  if (begin_run_) {
    pos_ = base_ticks_;
    c.started = begin_started_;
    begin_run_ = false;
  }
  // Where the master will be at the end of this block, and the increment that gets the position there: the loop's own, held to +-25 % of it while the position catches up with
  // the master (after a Start, a Continue, a correction of the loop). pos + inc * frames is the next block's pos, exactly.
  const std::int64_t target = master_ticks_at(block_start_ + frames);
  const std::int64_t span = inc_ref / 100 * static_cast<std::int64_t>(kSlewPercent);
  const std::int64_t want = (target - static_cast<std::int64_t>(pos_)) / static_cast<std::int64_t>(frames != 0u ? frames : 1u);
  std::int64_t inc = clamp64(clamp64(want, inc_ref - span, inc_ref + span), inc_min_, inc_max_);  // (the catch-up too stays in the engine's range)
  if (!begin) {
    // One block owns each whole tick (dsp::frame_at: the block's own increment decides which of two neighbours owns a tick near their boundary): block A owns up to
    // pos_ - inc_A, block B from pos_ - inc_B on, so a whole tick q* in the gap (pos_ - inc_A, pos_ - inc_B] would be owned by none and one in (pos_ - inc_B, pos_ - inc_A]
    // by both (a step lost or played twice, whenever the increment moves). With d the distance past the last whole tick at the boundary, B owns q* exactly when d < inc_B,
    // A when d >= inc_A: the increment of B is moved toward A's, never past it, until the two agree. It is only ever a few blocks in a hundred (d < inc), and a
    // correction of the loop is a block late then.
    const std::int64_t d = static_cast<std::int64_t>(pos_ & 0xFFFFFFFFull);
    if (d < prev_inc_) {
      inc = inc <= d ? d + 1 : inc;
    } else {
      inc = inc > d ? d : inc;
    }
  }
  prev_inc_ = inc;
  prev_playing_ = true;
  c.playing = true;
  c.pos = pos_;
  c.inc = static_cast<std::uint64_t>(inc);
  pos_ += static_cast<std::uint64_t>(inc) * frames;
  return c;
}

// ---- output ------------------------------------------------------------------------------------------------------------------------------

// A message of the hooks or of send() as the router's event. False: not a MIDI message the router knows.
bool MidiService::to_event(const core::MidiMessage& m, audio::SeqEvent& ev) const noexcept {
  ev = audio::SeqEvent{};
  ev.track = audio::kNoTrack;
  ev.flags = static_cast<std::uint8_t>(m.ports & audio::kRoutePorts);  // 0: the device's out ports
  const std::uint8_t d1 = static_cast<std::uint8_t>(m.data1 & 0x7Fu);
  const std::uint8_t d2 = static_cast<std::uint8_t>(m.data2 & 0x7Fu);
  if (core::midi_is_channel(m.status)) {
    ev.c = static_cast<std::uint8_t>(m.status & 0x0Fu);
    switch (core::midi_kind(m.status)) {
      case core::kMidiNoteOff:
        ev.kind = EventKind::MidiNoteOff;
        ev.a = d1;
        return true;
      case core::kMidiNoteOn:
        ev.kind = d2 == 0u ? EventKind::MidiNoteOff : EventKind::MidiNoteOn;
        ev.a = d1;
        ev.b = d2;
        return true;
      case core::kMidiPolyPressure:
        ev.kind = EventKind::MidiPolyAt;
        ev.a = d1;
        ev.b = d2;
        return true;
      case core::kMidiControl:
        ev.kind = EventKind::MidiCc;
        ev.a = static_cast<std::uint8_t>(audio::kMidiCcExplicit | d1);
        ev.b = d2;
        return true;
      case core::kMidiProgram:
        ev.kind = EventKind::MidiProgram;
        ev.a = d1;
        return true;
      case core::kMidiChannelPressure:
        ev.kind = EventKind::MidiChanAt;
        ev.a = d1;
        return true;
      case core::kMidiPitchBend:
        ev.kind = EventKind::MidiPitchBend;
        ev.a = d1;
        ev.b = d2;
        return true;
      default:
        return false;
    }
  }
  switch (m.status) {
    case core::kMidiSongPosition:
      ev.kind = EventKind::MidiSpp;
      ev.value = static_cast<std::int32_t>(d1 | (d2 << 7));
      return true;
    case core::kMidiClock:
      ev.kind = EventKind::MidiClock;
      return true;
    case core::kMidiStart:
      ev.kind = EventKind::MidiStart;
      return true;
    case core::kMidiContinue:
      ev.kind = EventKind::MidiContinue;
      return true;
    case core::kMidiStop:
      ev.kind = EventKind::MidiStop;
      return true;
    default:
      return false;
  }
}

void MidiService::send_message(const core::MidiMessage& m, std::uint64_t sample) noexcept {
  audio::SeqEvent ev;
  if (!to_event(m, ev)) {
    ++stats_.bad_out;
    return;
  }
  if (!usb_up_ && (ev.flags & audio::kRoutePorts) != 0u) {
    // An explicit route: without the USB port what is left of it (0 would mean the device's ports, which is another message).
    ev.flags = static_cast<std::uint8_t>(ev.flags & core::kMidiDin);
    if (ev.flags == 0u) {
      ++stats_.usb_skipped;
      return;
    }
  }
  (void)out_.send(ev, sample);
  ++stats_.sent;
  log_tx(m.ports, m.status, m.data1, m.data2);
}

void MidiService::send_transport(EventKind kind, std::uint64_t sample, std::int32_t value) noexcept {
  audio::SeqEvent ev{};
  ev.kind = kind;
  ev.track = audio::kNoTrack;
  ev.value = value;
  (void)out_.send(ev, sample);
  ++stats_.starts_out;
  const std::uint8_t status = kind == EventKind::MidiStart ? core::kMidiStart
                              : kind == EventKind::MidiContinue ? core::kMidiContinue
                              : kind == EventKind::MidiStop     ? core::kMidiStop
                                                                : core::kMidiSongPosition;
  log_tx(out_ports(), status, static_cast<std::uint8_t>(value & 0x7F), static_cast<std::uint8_t>((value >> 7) & 0x7F));
}

int MidiService::frame_of(std::uint64_t pos, std::uint64_t inc, std::uint32_t frames, std::uint64_t tick) noexcept {
  if (inc == 0u || frames == 0u) {
    return -1;
  }
  const std::uint64_t q = tick << 32;
  const std::uint64_t last = pos + inc * (frames - 1u);
  if (q + inc <= pos || q > last) {
    return -1;
  }
  return q <= pos ? 0 : static_cast<int>((q - pos + inc - 1u) / inc);
}

// A MIDI clock at every whole multiple of 40 ticks the block owns, placed to a fraction of a sample (the one event timed finer than a sample): the nearest sample, and the
// signed rest within half a sample either way (EventKind::MidiClock's `value`), which MidiOut turns into CPU cycles.
void MidiService::send_clocks(const core::ClockFrame& t, std::uint32_t frames) noexcept {
  const std::uint64_t last = t.pos + t.inc * (frames - 1u);
  std::uint64_t k = (t.pos >> 32) / kTicksPerClock;
  k = k > 0u ? k - 1u : 0u;
  for (; k * kTicksPerClock <= (last >> 32); ++k) {
    const std::uint64_t tick = k * kTicksPerClock;
    const int f = frame_of(t.pos, t.inc, frames, tick);
    if (f < 0) {
      continue;
    }
    // The exact time is f + rest samples, rest in (-1, 0]: (tick - pos) - f * inc is in (-inc, 0] ticks, divided by inc.
    const std::int64_t diff = static_cast<std::int64_t>((tick << 32) - t.pos);
    const std::int64_t rem = diff - static_cast<std::int64_t>(f) * static_cast<std::int64_t>(t.inc);
    std::int64_t rest = (rem * kOne) / static_cast<std::int64_t>(t.inc);  // Q32 samples, (-2^32, 0]
    std::int64_t sample = f;
    if (rest < -(kOne / 2)) {  // nearer the sample before
      if (sample > 0) {
        --sample;
        rest += kOne;
      } else {
        rest = -(kOne / 2);  // (the clock is before the block's first sample: held to half a sample early)
      }
    }
    audio::SeqEvent ev{};
    ev.kind = EventKind::MidiClock;
    ev.track = audio::kNoTrack;
    ev.value = static_cast<std::int32_t>(clamp64(rest, -(kOne / 2), kOne / 2 - 1));
    (void)out_.send(ev, block_start_ + static_cast<std::uint64_t>(sample));
    ++stats_.clocks_out;
  }
}

// ---- status --------------------------------------------------------------------------------------------------------------------------------

void MidiService::publish_status() noexcept {
  const std::uint32_t flags = (following_ ? 1u : 0u) | (following_ && dll_.locked() ? 2u : 0u);
  st_flags_.store(flags, std::memory_order_relaxed);
  st_tx_.store(out_.stats().handed, std::memory_order_relaxed);
  std::uint32_t lost = stats_.poll_dropped + stats_.held_dropped + out_.stats().staging_overflow;
  if (hooks_.isr_dropped != nullptr) {
    lost += hooks_.isr_dropped(hooks_.ctx);
  }
  st_dropped_.store(lost, std::memory_order_relaxed);
  st_phase_.store(dll_.has_phase() ? 1u : 0u, std::memory_order_relaxed);
  st_bpm_.store(dll_.has_phase() ? dll_.bpm_x100() : 0u, std::memory_order_relaxed);
  st_clocks_.store(in_.stats().clocks, std::memory_order_relaxed);
  float e = dll_.last_error_samples();
  e = e < 0.0f ? -e : e;
  const float us = e * (static_cast<float>(core::kRefCyclesPerSample.num) / static_cast<float>(core::kRefCyclesPerSample.den)) / 180.0f;  // cycles per sample / 180 cycles per us
  st_error_us_.store(dll_.has_phase() && us < 1.0e6f ? static_cast<std::uint32_t>(us) : 0u, std::memory_order_relaxed);
}

void MidiService::log_tx(std::uint8_t ports, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) noexcept {
  if (status == core::kMidiClock) {
    return;  // (a clock flood would hide every other message: the clocks are counted, in Stats)
  }
  const std::uint32_t slot = tx_log_n_.load(std::memory_order_relaxed);
  tx_log_[slot % kTxLog].store(static_cast<std::uint32_t>(ports) | (static_cast<std::uint32_t>(status) << 8) | (static_cast<std::uint32_t>(d1) << 16) |
                                   (static_cast<std::uint32_t>(d2) << 24),
                               std::memory_order_relaxed);
  tx_log_n_.store(slot + 1u, std::memory_order_relaxed);  // one writer
}

void MidiService::diag(Diag& d) const noexcept {
  d.status = status();
  d.config = box_.last_written();
  d.dll_phase = st_phase_.load(std::memory_order_relaxed) != 0u;
  d.dll_running = dll_.running();
  d.bpm_x100 = st_bpm_.load(std::memory_order_relaxed);
  d.clocks = st_clocks_.load(std::memory_order_relaxed);
  d.error_us = st_error_us_.load(std::memory_order_relaxed);
  d.service = stats_;
  d.in = in_.stats();
  d.out = out_.stats();
  d.dll = dll_.stats();
  d.send_dropped = send_dropped_.load(std::memory_order_relaxed);
  const std::uint32_t rn = rx_log_n_.load(std::memory_order_relaxed);
  d.rx_log_n = rn;
  const std::uint32_t rc = rn < kRxLog ? rn : kRxLog;
  for (std::uint32_t i = 0; i < rc; ++i) {
    const std::uint32_t w = rx_log_[(rn - rc + i) % kRxLog].load(std::memory_order_relaxed);
    d.rx_log[i][0] = static_cast<std::uint8_t>(w & 0xFFu);
    d.rx_log[i][1] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
  }
  const std::uint32_t tn = tx_log_n_.load(std::memory_order_relaxed);
  d.tx_log_n = tn;
  const std::uint32_t tc = tn < kTxLog ? tn : kTxLog;
  for (std::uint32_t i = 0; i < tc; ++i) {
    const std::uint32_t w = tx_log_[(tn - tc + i) % kTxLog].load(std::memory_order_relaxed);
    d.tx_log[i][0] = static_cast<std::uint8_t>(w & 0xFFu);
    d.tx_log[i][1] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
    d.tx_log[i][2] = static_cast<std::uint8_t>((w >> 16) & 0xFFu);
    d.tx_log[i][3] = static_cast<std::uint8_t>((w >> 24) & 0xFFu);
  }
}

}  // namespace o3sdk::platform::mk66::midi
