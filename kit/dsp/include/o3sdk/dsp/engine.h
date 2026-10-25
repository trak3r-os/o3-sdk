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
//   Setup     main loop, before start(): attach units, route them, add the clocked hooks. Not allowed after start().
//   Running   main loop: notes, parameter changes, mixer values, transport commands. Each call queues a command on a lock-free ring
//             (one producer: the main loop) and returns at once; the render applies it at frame 0 of its next block. A full ring
//             refuses the command (false) and counts it; nothing blocks.
//   Render    render(): the platform's audio callback (an interrupt on the unit, the simulator's loop on the host), once per block.
//             No heap, no blocking, bounded.
//
// Parameter values live on the main side (param(), set_param(), step_param(), format_param()), as the user sees them; the unit
// gets each change as an event. At start() every unit receives all its stored values (a process() call of zero frames).
//
// Statistics (stats()) are written by the render and read by the main loop through a sequence counter: a read that overlapped a
// write is retried, so the snapshot is always one block's.

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
inline constexpr unsigned kMaxInserts = 2;
inline constexpr unsigned kSendCount = 2;
inline constexpr unsigned kMaxHooks = 4;
inline constexpr unsigned kMaxParamsPerUnit = 64;
inline constexpr unsigned kMaxEventsPerUnit = 32;  // a unit's events in one block; the rest are dropped and counted
inline constexpr std::uint32_t kCommandRingSize = 512;
inline constexpr unsigned kMaxCommandsPerBlock = 256;  // a bound on the drain, so a flooded ring cannot stretch a block

using UnitId = std::uint8_t;
inline constexpr UnitId kNoUnit = 0xFF;

// A slot's mixer values. Level and the sends are linear gains (0..1.5, 1 = unity), pan -1 (left) .. 1 (right), mute 0 or 1.
enum class MixField : std::uint8_t { Level, Pan, SendA, SendB, Mute };
inline constexpr unsigned kMixFieldCount = 5;

// What a clocked hook may do inside a block: read the transport, place events for any unit.
class BlockContext {
 public:
  const Transport& transport() const noexcept { return *transport_; }
  // Queues `e` (its frame inside this block) for `unit`. false: unknown unit, or the unit's list for this block is full (counted).
  bool emit(UnitId unit, const Event& e) noexcept;
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
  std::uint32_t tempo() const noexcept { return tempo_main_; }
  bool playing() const noexcept { return playing_main_; }  // what the main loop asked for last

  EngineStats stats() const noexcept;
  std::uint32_t dropped_commands() const noexcept { return dropped_commands_; }

  // ---- Render ------------------------------------------------------------------------------------------------------------
  // One block of `frames` (1..kMaxBlockFrames) samples. `in_l` / `in_r` may be null (silence).
  void render(const float* in_l, const float* in_r, float* out_l, float* out_r, std::uint32_t frames) noexcept;

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
    UnitId inserts[kMaxInserts] = {kNoUnit, kNoUnit};
    // Render side: the targets and the gains reached at the end of the last block (ramped across a block when they change).
    float level = 1.0f;
    float pan = 0.0f;
    float send[kSendCount] = {0.0f, 0.0f};
    bool mute = false;
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    float send_gain[kSendCount] = {0.0f, 0.0f};
  };

  bool push(const Command& c) noexcept;
  bool queue_event(UnitId unit, const Event& e) noexcept;  // render side
  void drain() noexcept;
  void run_unit(UnitId unit, const float* in_l, const float* in_r, float* out_l, float* out_r, std::uint32_t frames) noexcept;
  void publish(float peak_l, float peak_r, const float* slot_peaks, std::uint32_t cycles, bool clipped) noexcept;

  // Setup and main side.
  Unit* units_[kMaxUnits] = {};
  unsigned unit_count_ = 0;
  Clocked* hooks_[kMaxHooks] = {};
  unsigned hook_count_ = 0;
  UnitId send_fx_[kSendCount] = {kNoUnit, kNoUnit};
  UnitId master_inserts_[kMaxInserts] = {kNoUnit, kNoUnit};
  bool started_ = false;
  float values_[kMaxUnits][kMaxParamsPerUnit] = {};
  float mix_main_[kMaxSlots][kMixFieldCount] = {};
  float master_level_main_ = 1.0f;
  std::uint32_t tempo_main_ = 120'000;
  bool playing_main_ = false;
  std::uint32_t dropped_commands_ = 0;
  std::uint32_t (*cycles_)() noexcept = nullptr;
  core::SpscRing<Command, kCommandRingSize> ring_;

  // Render side.
  Slot slots_[kMaxSlots];
  Transport transport_;
  float master_level_ = 1.0f;
  float master_gain_ = 1.0f;  // reached at the end of the last block
  Event events_[kMaxUnits][kMaxEventsPerUnit];
  std::uint8_t event_count_[kMaxUnits] = {};
  std::uint32_t dropped_events_ = 0;
  std::uint32_t blocks_ = 0;
  std::uint32_t cycles_max_ = 0;
  std::uint32_t clipped_ = 0;
  BlockContext ctx_;

  // Statistics: written by the render, read by stats() (sequence counter: odd while a write is in progress).
  std::atomic<std::uint32_t> stats_seq_{0};
  EngineStats stats_;
};

// The interleaved int16 frames of the HAL (hal/audio.h: L, R, L, R ...) to and from the engine's float planes.
void s16_to_planes(const std::int16_t* lr, float* l, float* r, std::uint32_t frames) noexcept;
void planes_to_s16(const float* l, const float* r, std::int16_t* lr, std::uint32_t frames) noexcept;

}  // namespace o3sdk::dsp
