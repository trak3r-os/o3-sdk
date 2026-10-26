// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The engine: runs the app's units once per block and mixes them (docs/FRAMEWORK.md, "Engine").
//
//   clocked hooks ──events──▶ ┌ slot 0: source unit ─ insert ─ insert ─ level/pan ─┬──────────────▶ master ─ insert ─ insert ─ level ─ clip ─▶ out
//   (sequencers)              │ slot 1 ...                                         ├─ send A ─ fx ─┘
//   main loop ───commands───▶ └ slot 7                                           └─ send B ─ fx ─┘
//
// The topology is fixed at setup: up to kMaxSlots sources, each with up to kMaxInserts insert effects, kSendCount send buses with
// one effect each, kMaxInserts master inserts. Every unit is attached once and addressed by its UnitId.
//
// Three phases and three contexts:
//   Setup     main loop, before start(): attach units, route them, add the clocked hooks. Not allowed after start(). A setup call
//             that is refused returns false (attach: kNoUnit) and is also recorded: setup_errors() says how many, setup_error(i)
//             which, so an app that ignores the return values still has a refusal to find (the simulator fails on one).
//   Running   main loop: notes, parameter changes, mixer values, transport commands. Each call queues a command on a lock-free ring
//             (one producer: the main loop) and returns at once; the render applies it at frame 0 of its next block. A full ring
//             refuses the command (false) and counts it; nothing blocks.
//   Render    render(): the platform's audio callback (an interrupt on the unit, the simulator's loop on the host), once per block.
//             No heap, no blocking, bounded. In a block: the queued commands, then the clocked hooks (events and mixer writes, see
//             BlockContext), then the units and the mixer.
//
// Parameter values live on the main side (param(), set_param(), step_param(), format_param()), as the user sees them; the unit
// gets each change as an event. At start() every unit receives all its stored values (a process() call of zero frames).
//
// Statistics (stats()) are written by the render and read by the main loop through a sequence counter: a read that overlapped a
// write is retried, so the snapshot is always one block's.
//
// MIDI (docs/M2B2-PLAN.md section 3). The engine knows no device: the runtime (Runtime::render_block) hands it what a device reported,
// in the render context, before render(): follow() the transport of the block (a master's clock) and inject() the notes, and takes back
// midi_out() and block_frame() after it. With a master followed, the block's Transport is the master's and the engine's own advance
// is parked; a clocked hook, which walks ctx.transport(), follows with no change. While following, play() and stop() are requests for the
// device (take_transport_request()), and playing() and tempo() report the master's.

#include <o3sdk/core/midi_types.h>
#include <o3sdk/core/param.h>
#include <o3sdk/core/ring.h>
#include <o3sdk/dsp/event.h>
#include <o3sdk/dsp/transport.h>
#include <o3sdk/dsp/unit.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::dsp {

inline constexpr std::uint32_t kMaxBlockFrames = 64;
inline constexpr unsigned kMaxUnits = 16;
inline constexpr unsigned kMaxSlots = 8;
inline constexpr unsigned kMaxInserts = 4;
inline constexpr unsigned kSendCount = 2;
inline constexpr unsigned kMaxHooks = 4;
inline constexpr unsigned kMaxParamsPerUnit = 64;
inline constexpr unsigned kMaxEventsPerUnit = 32;  // a unit's events in one block; the rest are dropped and counted
inline constexpr std::uint32_t kCommandRingSize = 512;
inline constexpr unsigned kMaxCommandsPerBlock = 256;  // a bound on the drain, so a flooded ring cannot stretch a block
inline constexpr unsigned kMaxInjectPerUnit = kMaxEventsPerUnit / 2;  // a unit's events a block that inject() takes (routed notes): the rest of its list is the hooks'
inline constexpr unsigned kMaxMidiOut = 32;  // messages the hooks may send in one block (BlockContext::send_midi); the rest are dropped and counted

using UnitId = std::uint8_t;
inline constexpr UnitId kNoUnit = 0xFF;

// A refused setup call (Engine::setup_error). The record is main-loop data: written by the setup calls, read by the main loop.
enum class SetupCall : std::uint8_t { Attach, SetSource, SetInsert, SetSendEffect, SetMasterInsert, AddHook };
enum class SetupReason : std::uint8_t {
  AfterStart,     // start() has run: the setup is over
  Full,           // attach: kMaxUnits are attached; add_hook: kMaxHooks are added
  TooManyParams,  // attach: the unit declares more than kMaxParamsPerUnit parameters
  OutOfRange,     // a slot, bus or insert index outside the topology
  UnknownUnit,    // the unit id was never returned by attach (kNoUnit, after an attach that was refused, for one)
  WrongKind,      // a source that is not an instrument, or an effect slot given a unit that is not an effect
};
struct SetupError {
  SetupCall call;
  SetupReason reason;
  std::uint8_t slot;   // the call's slot, or the send bus; 0 where the call has none
  std::uint8_t index;  // the insert index (set_insert, set_master_insert); 0 elsewhere
  std::uint8_t unit;   // the UnitId the call was given (kNoUnit for attach, add_hook)
};
inline constexpr unsigned kMaxSetupErrors = 8;  // records kept; the count goes on past it

// A slot's mixer values. Level and the sends are linear gains (0..1.5, 1 = unity), pan -1 (left) .. 1 (right), mute 0 or 1.
enum class MixField : std::uint8_t { Level, Pan, SendA, SendB, Mute };
inline constexpr unsigned kMixFieldCount = 5;

// What a clocked hook may do inside a block: read the transport, place events for any unit, write a slot's mixer.
class BlockContext {
 public:
  const Transport& transport() const noexcept { return *transport_; }
  // Queues `e` (its frame inside this block) for `unit`. false: unknown unit, or the unit's list for this block is full (counted).
  bool emit(UnitId unit, const Event& e) noexcept;
  // Writes one mixer field of `slot` (a sequencer's motion lane, a slew). The value is clamped as Engine::set_mix clamps it (NaN gives
  // the field's default) and is the field's target from here: the mixer ramps the gain from where the last block left it to the
  // target across this whole block, exactly as for a main-loop write, so a jump is click-free. A few instructions, no queue, no
  // limit on the calls; the mixer has one ramp per block, so several writes to a field in one block collapse to the last.
  // Order, the one rule: each block applies the main loop's queued commands first, then runs the hooks in the order added, then
  // the mixer reads the targets. For a field the last write in that order wins: a hook's write beats a main-loop write applied in
  // the same block, a later hook's beats an earlier one's, a later call of the same hook beats its earlier one. The value stays the
  // target (it is not restored) until another write replaces it, and the main loop's next write, applied at the start of a later
  // block, replaces it. Engine::mix() keeps reporting the main loop's last value: a hook's write is the render side's, as a Param
  // event is for param(). false, nothing written: `slot` >= kMaxSlots or `field` not a MixField (refused, not clamped).
  bool set_mix(unsigned slot, MixField field, float value) noexcept;
  // The target of that field as the render side stands now (what the last write left, the main loop's or a hook's); 0 for a slot or
  // field out of range, as Engine::mix(). A slew reads it to start from where the field is.
  float mix(unsigned slot, MixField field) const noexcept;
  // Queues a MIDI message for the device to send after this block (hal::midi::Device::end_block): at its `frame` (clamped into the
  // block), on its `ports` (0: the user's Config::out_ports). At most kMaxMidiOut a block, shared by every hook; false past it (counted in
  // EngineStats::dropped_midi). With no MIDI device the messages go nowhere. A copy into a short array: no queue beyond the block.
  bool send_midi(const core::MidiMessage& m) noexcept;
  // The codec's input of this block (frames samples each), for a hook that listens (a recorder's threshold).
  const float* input_l() const noexcept { return in_l_; }
  const float* input_r() const noexcept { return in_r_; }

 private:
  friend class Engine;
  class Engine* engine_ = nullptr;
  const Transport* transport_ = nullptr;
  const float* in_l_ = nullptr;
  const float* in_r_ = nullptr;
};

// A sequencer, an arpeggiator, a MIDI clock: called once per block in the render context, before the units, in the order added.
class Clocked {
 public:
  virtual void block(BlockContext& ctx) noexcept = 0;

 protected:
  Clocked() = default;
  ~Clocked() = default;
  Clocked(const Clocked&) = delete;
  Clocked& operator=(const Clocked&) = delete;
};

struct EngineStats {
  std::uint32_t blocks = 0;            // blocks rendered since start()
  float peak_l = 0.0f;                 // master output peak of the last block, before the clip (1.0 = full scale)
  float peak_r = 0.0f;
  float slot_peak[kMaxSlots] = {};     // each slot's peak after its inserts and level, last block
  std::uint32_t cycles = 0;            // cycles of the last render (set_cycle_counter); 0 without a counter
  std::uint32_t cycles_max = 0;        // the most since start()
  std::uint32_t dropped_events = 0;    // events refused because a unit's list was full, since start()
  std::uint32_t clipped = 0;           // blocks whose output reached the clipper, since start()
  bool playing = false;
  std::uint32_t tempo_mbpm = 0;
  std::uint64_t pos = 0;               // Q32.32 transport position at the end of the last block
  std::uint32_t dropped_midi = 0;      // send_midi() messages refused past kMaxMidiOut a block, since start()
};

class Engine {
 public:
  Engine() noexcept;

  // ---- Setup (main loop, before start()) --------------------------------------------------------------------------------
  // Prepares the unit (prepare(sample rate)) and stores its parameter defaults. kNoUnit: the engine is full, the unit declares more
  // than kMaxParamsPerUnit parameters, or start() has run.
  UnitId attach(Unit& unit) noexcept;
  bool set_source(unsigned slot, UnitId instrument) noexcept;
  bool set_insert(unsigned slot, unsigned index, UnitId effect) noexcept;
  bool set_send_effect(unsigned bus, UnitId effect) noexcept;
  bool set_master_insert(unsigned index, UnitId effect) noexcept;
  bool add_hook(Clocked& hook) noexcept;
  // Optional: a cycle counter for the render's statistics (hal::cycles on a platform).
  void set_cycle_counter(std::uint32_t (*counter)() noexcept) noexcept { cycles_ = counter; }
  // Ends the setup: every unit receives its stored parameter values. Then render() may run.
  void start() noexcept;
  bool started() const noexcept { return started_; }
  // The refused setup calls (attach, set_source, set_insert, set_send_effect, set_master_insert, add_hook), counted since construction:
  // 0 for an app whose setup went through. A call made after start() is refused and counted too. Nothing changes for the caller of the
  // setup calls; this is the way to see a refusal that was not checked. Main loop only, like the setup itself.
  unsigned setup_errors() const noexcept { return setup_error_count_; }
  // The i-th refusal in order (the first kMaxSetupErrors are kept), nullptr for i >= min(setup_errors(), kMaxSetupErrors).
  const SetupError* setup_error(unsigned i) const noexcept {
    return i < setup_error_count_ && i < kMaxSetupErrors ? &setup_error_log_[i] : nullptr;
  }
  // The names for a message: "set_source", "slot out of range" (static text).
  static const char* setup_call_name(SetupCall call) noexcept;
  static const char* setup_reason_name(SetupReason reason) noexcept;

  // ---- Running (main loop) ----------------------------------------------------------------------------------------------
  bool send(UnitId unit, const Event& e) noexcept;
  bool note_on(UnitId unit, std::uint8_t note, std::uint8_t velocity, std::uint8_t flags = 0) noexcept {
    return send(unit, dsp::note_on(note, velocity, flags));
  }
  bool note_off(UnitId unit, std::uint8_t note) noexcept { return send(unit, dsp::note_off(note)); }

  unsigned unit_count() const noexcept { return unit_count_; }
  const UnitInfo* unit_info(UnitId unit) const noexcept;
  const core::ParamDesc* param_desc(UnitId unit, std::uint16_t id) const noexcept;
  float param(UnitId unit, std::uint16_t id) const noexcept;  // the stored value (the default for an unknown id: 0)
  // Clamps through the descriptor, stores and sends. false: unknown unit or id, or the ring is full (the value is stored anyway, and
  // the next change of the same parameter carries it).
  bool set_param(UnitId unit, std::uint16_t id, float value) noexcept;
  // `detents` encoder steps away (core::param_step), `coarse` multiplies them by the descriptor's coarse step. Returns the new value.
  float step_param(UnitId unit, std::uint16_t id, int detents, bool coarse = false) noexcept;
  // The value as text, without the unit text ("1.20k", "-12.0", "SAW"); 0 and "" for an unknown parameter.
  unsigned format_param(UnitId unit, std::uint16_t id, char* out, unsigned cap) const noexcept;

  bool set_mix(unsigned slot, MixField field, float value) noexcept;
  float mix(unsigned slot, MixField field) const noexcept;
  bool set_master_level(float level) noexcept;
  float master_level() const noexcept { return master_level_main_; }

  bool play() noexcept;   // from the top: position 0 at the next block
  bool stop() noexcept;   // position back to 0; the next block has Transport::stopped set (hooks end their notes)
  bool set_tempo(std::uint32_t tempo_mbpm) noexcept;
  // While a MIDI master is followed (following()), play() and stop() are not commands but requests for the device (Play starts the run on
  // the master's next clock; take_transport_request()), set_tempo() is kept for when the master is let go, and tempo() and playing() report
  // the master's, as of the last block. Otherwise tempo() is what the main loop set and playing() what it asked for last (false after a
  // master was let go, until the next play()).
  std::uint32_t tempo() const noexcept { return following() ? followed_tempo_.load(std::memory_order_relaxed) : tempo_main_; }
  // The engine's own tempo, what set_tempo() last stored, whether or not a master is followed: the base for a step of "tempo + 1 BPM" (tempo() + 1
  // BPM would store the master's tempo as the own one).
  std::uint32_t own_tempo() const noexcept { return tempo_main_; }
  bool playing() const noexcept;
  // The engine's transport is a master's (the last block's Engine::follow had `follow` set). Any context; a flag the render publishes.
  bool following() const noexcept { return following_.load(std::memory_order_relaxed); }
  // The play() or stop() the main loop made while following, once (None otherwise): the runtime hands it to the device. Main loop only.
  core::TransportRequest take_transport_request() noexcept {
    const core::TransportRequest r = request_;
    request_ = core::TransportRequest::None;
    return r;
  }

  EngineStats stats() const noexcept;
  std::uint32_t dropped_commands() const noexcept { return dropped_commands_; }

  // ---- Render ------------------------------------------------------------------------------------------------------------
  // One block of `frames` (1..kMaxBlockFrames) samples. `in_l` / `in_r` may be null (silence).
  void render(const float* in_l, const float* in_r, float* out_l, float* out_r, std::uint32_t frames) noexcept;

  // ---- MIDI (render side: the render context, between two render() calls) --------------------------------------------------------------
  // The transport of the next render(): with `f.follow` set it is the frame's (a master's position, increment and tempo, its Start, Continue
  // and Stop) instead of the engine's own advance, for that block only. The first block of it ends the engine's own run (a hook sees
  // Transport::stopped, and `playing` again when the master plays), and the first block without it, after one that had it, ends the master's:
  // the transport stops, once, and the engine's own tempo is back. A frame is sanitised: the increment into what a legal tempo can be, a
  // stop that the frame forgot to flag is flagged. See hal/midi.h for what a device promises.
  void follow(const core::ClockFrame& f) noexcept;
  // Queues `e` for `unit` at its own frame, for the next render() (a routed note). At most kMaxInjectPerUnit a unit and block, so a flood of
  // notes cannot fill the unit's list and push out a hook's note-off (which would hang the note): false for the rest, and for an unknown unit,
  // counted in dropped_events. An event that falls past the block is applied at its end.
  bool inject(UnitId unit, const Event& e) noexcept;
  // After render(): the messages the hooks sent in that block (BlockContext::send_midi), valid until the next render().
  const core::MidiMessage* midi_out(unsigned& n) const noexcept {
    n = midi_out_n_;
    return midi_out_;
  }
  // After render(): the transport as that block ran it (its first frame's position, flags and increment, `follow` set when it was a master's):
  // what the device sends the clock and the transport from.
  const core::ClockFrame& block_frame() const noexcept { return block_; }

 private:
  friend class BlockContext;

  enum class Op : std::uint8_t { UnitEvent, Mix, MasterLevel, Play, Stop, Tempo };
  struct Command {
    Op op;
    std::uint8_t target;  // unit id or slot
    std::uint8_t field;   // MixField
    std::uint8_t pad;
    float value;          // mixer value, master level
    std::uint32_t arg;    // tempo
    Event event;
  };
  static_assert(sizeof(Command) == 24, "a command is six words");

  struct Slot {
    UnitId source = kNoUnit;
    UnitId inserts[kMaxInserts] = {kNoUnit, kNoUnit, kNoUnit, kNoUnit};
    // Render side: the targets and the gains reached at the end of the last block (ramped across a block when they change).
    float level = 1.0f;
    float pan = 0.0f;
    float send[kSendCount] = {0.0f, 0.0f};
    bool mute = false;
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    float send_gain[kSendCount] = {0.0f, 0.0f};
  };

  bool refuse(SetupCall call, SetupReason reason, unsigned slot, unsigned index, unsigned unit) noexcept;  // records, returns false
  bool push(const Command& c) noexcept;
  bool queue_event(UnitId unit, const Event& e) noexcept;  // render side
  bool queue_midi(const core::MidiMessage& m) noexcept;    // render side
  void apply_mix(unsigned slot, MixField field, float value) noexcept;  // render side: `value` already clamped, `slot` and `field` valid
  void drain() noexcept;
  void apply_follow() noexcept;    // render side: the frame follow() was given becomes the block's transport
  void release_follow() noexcept;  // render side: the master is let go
  void run_unit(UnitId unit, const float* in_l, const float* in_r, float* out_l, float* out_r, std::uint32_t frames) noexcept;
  void publish(float peak_l, float peak_r, const float* slot_peaks, std::uint32_t cycles, bool clipped) noexcept;

  // Setup and main side.
  Unit* units_[kMaxUnits] = {};
  unsigned unit_count_ = 0;
  Clocked* hooks_[kMaxHooks] = {};
  unsigned hook_count_ = 0;
  UnitId send_fx_[kSendCount] = {kNoUnit, kNoUnit};
  UnitId master_inserts_[kMaxInserts] = {kNoUnit, kNoUnit, kNoUnit, kNoUnit};
  bool started_ = false;
  float values_[kMaxUnits][kMaxParamsPerUnit] = {};
  float mix_main_[kMaxSlots][kMixFieldCount] = {};
  float master_level_main_ = 1.0f;
  std::uint32_t tempo_main_ = 120'000;
  bool playing_main_ = false;
  std::uint32_t released_seen_ = 0;  // main loop: `released_` when playing_main_ was last set
  core::TransportRequest request_ = core::TransportRequest::None;  // main loop: a play() or stop() made while following
  std::uint32_t dropped_commands_ = 0;
  unsigned setup_error_count_ = 0;
  SetupError setup_error_log_[kMaxSetupErrors] = {};
  std::uint32_t (*cycles_)() noexcept = nullptr;
  core::SpscRing<Command, kCommandRingSize> ring_;

  // Render side.
  Slot slots_[kMaxSlots];
  Transport transport_;
  float master_level_ = 1.0f;
  float master_gain_ = 1.0f;  // reached at the end of the last block
  Event events_[kMaxUnits][kMaxEventsPerUnit];
  std::uint8_t event_count_[kMaxUnits] = {};
  std::uint8_t injected_[kMaxUnits] = {};  // inject()'s events in the lists of the next block
  std::uint32_t dropped_events_ = 0;
  std::uint32_t blocks_ = 0;
  std::uint32_t cycles_max_ = 0;
  std::uint32_t clipped_ = 0;
  BlockContext ctx_;

  // MIDI, render side. The engine's own tempo is kept apart from the transport's, which a master's overwrites while it is followed.
  core::ClockFrame follow_in_;      // follow()'s frame for the next render()
  bool follow_pending_ = false;     // follow() was given a frame with `follow` set since the last render()
  bool followed_ = false;           // the last block ran on a master's transport
  std::uint32_t own_tempo_ = 120'000;
  std::uint64_t own_inc_ = 0;
  std::uint64_t inc_min_ = 0;       // the increments of the slowest and the fastest legal tempo: a master's is held between them
  std::uint64_t inc_max_ = 0;
  core::ClockFrame block_;          // block_frame()
  core::MidiMessage midi_out_[kMaxMidiOut] = {};
  unsigned midi_out_n_ = 0;
  std::uint32_t dropped_midi_ = 0;
  // Published by the render, read by the main loop (single writer, relaxed: flags and numbers a block old at worst).
  std::atomic<bool> following_{false};
  std::atomic<std::uint32_t> followed_tempo_{0};
  std::atomic<bool> followed_playing_{false};
  std::atomic<std::uint32_t> released_{0};  // masters let go so far; the main loop's playing_main_ is void once it has moved

  // Statistics: written by the render, read by stats() (sequence counter: odd while a write is in progress).
  std::atomic<std::uint32_t> stats_seq_{0};
  EngineStats stats_;
};

// The interleaved int16 frames of the HAL (hal/audio.h: L, R, L, R ...) to and from the engine's float planes.
void s16_to_planes(const std::int16_t* lr, float* l, float* r, std::uint32_t frames) noexcept;
void planes_to_s16(const float* l, const float* r, std::int16_t* lr, std::uint32_t frames) noexcept;

}  // namespace o3sdk::dsp
