// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A unit: DSP only, no screen, no buttons (docs/FRAMEWORK.md, "Units"). An instrument (notes in, audio out) or an effect (audio in,
// audio out). The shape follows CLAP on purpose, so that porting a plugin's DSP is mostly mechanical: a table of parameters,
// one process() call per block with the block's events, nothing else.
//
// Parameters. A unit declares them as a constexpr table of core::ParamDesc (core/param.h): range, default, curve, display. The
// parameter id IS the index in that table: ids are stable, a new parameter is appended, never inserted (a saved value is found by
// its id). The unit never owns the values the user sees: the engine's main side keeps them (EngineLink::param), formats them and
// steps them with the encoder; the unit receives changes as Param events in process(), at their frame. At prepare() the unit
// takes its defaults; the engine then delivers every stored value as an event in the first block, so both sides agree.
//
// Threads. prepare() runs in the main loop before the engine starts. process() runs in the render context (an interrupt on the
// unit): no heap, no blocking, no file or SD I/O, no printf, bounded loops, no double. The two never overlap.
//
// Memory. No heap anywhere: a unit is a plain object the app declares (statically), with all its state inside.

#include <o3sdk/core/param.h>
#include <o3sdk/dsp/event.h>
#include <o3sdk/dsp/transport.h>

#include <cstdint>

namespace o3sdk::dsp {

enum class UnitKind : std::uint8_t {
  Instrument,  // notes in, audio out (in_l / in_r carry the codec's input: a sampler may record it)
  Effect,      // audio in, audio out
};

struct UnitInfo {
  const char* name;                 // short, upper case: "SINE", "TB303"
  UnitKind kind;
  const core::ParamDesc* params;    // the parameter table; id = index
  std::uint16_t param_count;
  const char* const* pages;         // names of the parameter pages, indexed by ParamDesc::group; null when there is one page
  std::uint8_t page_count;
};

struct ProcessArgs {
  const Event* events;         // this block's events for this unit, sorted by frame
  std::uint32_t event_count;
  const float* in_l;           // Effect: the signal to process. Instrument: the codec input (line in / mic), for a recorder
  const float* in_r;
  float* out_l;                // the unit writes all `frames` samples; an effect may run in place (out == in)
  float* out_r;
  std::uint32_t frames;        // at most kMaxBlockFrames (engine.h)
  const Transport* transport;  // never null
};

class Unit {
 public:
  virtual const UnitInfo& info() const noexcept = 0;
  // Main loop, before the engine starts: reset every piece of state, take the defaults of the parameter table.
  virtual void prepare(float sample_rate) noexcept = 0;
  // Render context: one block.
  virtual void process(const ProcessArgs& args) noexcept = 0;

 protected:
  Unit() = default;
  ~Unit() = default;  // never deleted through the interface: no heap
  Unit(const Unit&) = delete;
  Unit& operator=(const Unit&) = delete;
};

// Splits a block into runs between events, the usual shape of process():
//
//   for_each_run(args,
//       [&](std::uint32_t from, std::uint32_t to) { render frames [from, to) },
//       [&](const Event& e) { apply e });
//
// Events at the same frame are applied in order before the run that starts there. Events past the block are applied at its end.
template <typename Render, typename Apply>
void for_each_run(const ProcessArgs& args, Render&& render, Apply&& apply) noexcept {
  std::uint32_t at = 0;
  for (std::uint32_t i = 0; i < args.event_count; ++i) {
    const Event& e = args.events[i];
    const std::uint32_t frame = e.frame < args.frames ? e.frame : args.frames;
    if (frame > at) {
      render(at, frame);
      at = frame;
    }
    apply(e);
  }
  if (at < args.frames) {
    render(at, args.frames);
  }
}

}  // namespace o3sdk::dsp
