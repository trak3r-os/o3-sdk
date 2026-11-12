// SPDX-License-Identifier: GPL-3.0-or-later
// The tract processor: what both units (o3-303's TRACT page and o3-voc03er's FORMANT effect) wrap. A mono signal goes in as the voice box's
// buzz, the tube (tube.h) shapes it into vowels, a mono signal comes out, mixed with the dry one. README.md explains the idea.
//
// The tube runs decimated: Quality::Full at fs/2 with 22 sections (formants right up to 11 kHz), Quality::Lite at fs/4 with 11 sections
// (up to 5.5 kHz). Half-band FIRs (halfband.h) take the signal down and up; the wet path is band-limited and 22 (Full) or 66 (Lite) frames
// late at the half-band stages' group delays, the dry path is not delayed. The geometry is fixed in samples: the formants of the table are
// those of fs = 44,100 Hz (the Tracker's rate); prepare() takes other rates only for the times of the ramps.
//
// Frame by frame, one state machine: the position inside the group of 2 (Full) or 4 (Lite) frames decides which frame feeds the tube, so
// any block size gives the same samples. Every kPeriod (16) frames a control step moves the one-poles of the vowel, the bends and the
// closures, and only if the shape moved recomputes the diameters and the reflections (N evaluations of a polynomial bump and N divisions);
// it also zeroes tiny states and clears the tube when one is not finite or huge.
//
//   set_vowel      0..1 through A E I O U (shape.h), one pole of GLIDE
//   set_bend       tongue, jaw, lips each -1..+1, added to the vowel's shape (jaw lowers the height), clamped, one pole of GLIDE
//   set_closure    lips (0), tip (1), back (2): moves in 12 ms whatever GLIDE says; a closure shuts the section and takes the wet gain to 1 - closure (silence)
//   set_glide_ms   5..500
//   set_size       -2..+2: N = N0 - 2 s (Full) or N0 - s (Lite): giant (-2, formants x0.85) to child (+2, x1.22); the change runs a 5 ms dip
//                  of the wet gain around the switch of N (the state is kept, the new tail is zeroed)
//   set_res        0..1: rG = rL = 0.60 + 0.37 r (the ringing of the formants); the wet gain follows (kMakeup / (1 - rL)^1.35: the lip outlet
//                  passes 1 - rL of the flow, and the formants narrow)
//   level          a slow level match (about 60 ms) keeps the wet RMS near the dry one over the vowels and RES (the tube's gain varies by
//                  more than 20 dB between /i/ and /u/, which no fixed makeup can follow): the ratio of two followers (the tube's input and
//                  output) trims the wet gain within -30..+24 dB; it holds its value while a closure is on or moving (the closure must
//                  silence) and while the source is under -70 dB (a note's attack meets the gain of the last one)
//   set_hiss       0..1: noise at the narrowest section, only while that section is under 0.3 wide (a consonant's turbulence); it follows the
//                  level of the source (not of the tube's own output: that would feed the noise back into itself)
//   set_mix        0..1, a 5 ms ramp; below 1e-3 and settled the signal passes bit for bit and no state runs (bypassed())
//   set_areas      the other way to drive the tube: n relative areas (2..kMaxAreas, section 0 the glottis, n - 1 the lips) in place of the
//                  vowel, the bends and the shape's closures. The areas are normalised (the widest section becomes the open tract, diameter
//                  1.5 in the shape's unit, whatever the scale the caller used), each diameter clamped to kMinDiameter..1.5, and stretched over
//                  the tube's current N (linearly in diameter, so a different N or a size change needs no new call). The diameters follow with
//                  the same one pole as the vowel (GLIDE), every control step, so a stream of new areas (a live analysis, one per few ms) is
//                  smooth. A NaN, an infinity, an area <= 0, a null pointer or n < 2 makes the call a no-op: the last areas stay. The
//                  closures still act on the wet gain (a stop silences) but no longer shape the tube; the hiss still follows the narrowest
//                  section. use_shape() goes back: the diameters glide from the areas to the vowel's shape (GLIDE) and then the vowel path
//                  runs as if set_areas had never been called. The vowel, the bends and the closures keep moving while the areas rule, so
//                  the way back lands on where they are by then
// A NaN parameter takes its default. All setters are real-time safe (a few float operations; set_glide_ms, set_res and prepare call
// expf or sqrtf once).
//
// Cost [measured by instruction trace]: X3 counted the cross build (-O2, the unit_tract_dsp archive) running on qemu (mps2-an386, Cortex-M4)
// frame by frame over 2,048 frames of a fixed test input with the control step included, and costed each executed instruction with the model below.
// A unit measurement is still owed.
//   Quality::Full (N = 22): about 256 cycles per frame including the control step, 6.3 % of the CPU at 44.1 kHz with a 64-frame block (40.8
//   cycles per frame are 1 %). X3 took it from about 401 in the same harness (X2's disassembly count said about 390).
//   Quality::Lite (N = 11): about 220 per frame including the control step, 5.4 % (from about 320; X2 said 310).
//   The model: 2 cycles per load or store, 1 per ALU or FPU operation, 14 per division or square root, 3 per taken branch, 4 per call, 1 + n
//   per ldm/stm/push/pop; it leaves out the load pipelining, so the unit should read lower (with a pipelined model the figures are about 226
//   Full and 186 Lite). While the shape moves, update_shape adds N bump evaluations and N divisions every 16 frames (not counted).
//   What X3 changed, output bit for bit the same: the three passes are one template per quality with the dip state machine on its own path;
//   the tube runs two or four samples of a group as lanes of a wavefront (Tube::tick2, Tube::tick4: lane j trails lane 0 by j sections, so
//   the left-going wave stays in registers and one load of b, one of k and one store serve every lane); the half-band stages are inline; the
//   wet gain and the control step use integer tests (sanitize) and a three-way max instead of fmax calls.
//
// The area path is a per-control-step branch (LT1): process() and run() are untouched, a processor never given areas takes the branch
// `area_mode_ != 0` (one byte compared, once per 16 frames) and runs the code it always ran, bit for bit (tests/areas_test.cpp pins a digest
// taken before the entry existed). In area mode a control step costs about 22 one-pole steps (about 90 cycles when settled) and, while the
// areas move, the N divisions of Tube::set_diameters and the narrowest-section search [est: about 800 cycles a step, 3,200 a block of 64 at
// Full while a live shape moves on every step; a unit measurement is owed]; set_areas itself is about 200 cycles at 13 areas.
//
// No heap, no double, all state is members (under 1.5 KB); the processor is not copyable on purpose (a unit holds one).
#pragma once

#include <o3sdk/extras/tract/halfband.h>
#include <o3sdk/extras/tract/tube.h>

#include <cstdint>

namespace o3sdk::extras::tract {

class Processor {
 public:
  enum class Quality : std::uint8_t { Full, Lite };

  static constexpr unsigned kPeriod = 16;        // frames between two control steps
  static constexpr float kMakeup = 1.2f;         // the static part of the wet gain (see the level trim): the middle of the vowels at RES 0.5
  static constexpr unsigned kMaxAreas = 24;      // the most areas set_areas takes

  explicit Processor(Quality q) noexcept;
  Processor(const Processor&) = delete;
  Processor& operator=(const Processor&) = delete;

  void prepare(float sample_rate) noexcept;      // 8,000 .. 192,000; resets
  void reset() noexcept;                         // clears every state and lands every smoother on its target

  void set_vowel(float v) noexcept;
  void set_bend(float tongue, float jaw, float lips) noexcept;
  void set_closure(unsigned which, bool on) noexcept;
  void set_glide_ms(float ms) noexcept;
  void set_size(int steps) noexcept;
  void set_res(float r) noexcept;
  void set_hiss(float h) noexcept;
  void set_mix(float m) noexcept;
  void set_areas(const float* area, unsigned n) noexcept;   // drive the tube from n areas (see the header); invalid input is ignored
  void use_shape() noexcept;                     // back to the vowel path, through a glide of the diameters

  // Mono, any n, `in` may equal `out`.
  void process(const float* in, float* out, std::uint32_t n) noexcept;

  bool bypassed() const noexcept { return mix_t_ < 1e-3f && mix_ <= 0.0f; }   // Mix 0 and settled
  unsigned sections() const noexcept { return tube_.size(); }
  Quality quality() const noexcept { return quality_; }
  bool areas_active() const noexcept { return area_mode_ != 0; }   // set_areas rules (or the way back to the shape is still gliding)

 private:
  void control() noexcept;
  template <bool Lite>
  void run(const float* in, float* out, std::uint32_t n) noexcept;   // process() of one quality
  void update_shape() noexcept;
  void shape_diameters(float* d, unsigned n) const noexcept;   // the vowel path's diameters for the current smoothed values
  void commit_diameters(unsigned n) noexcept;    // diam_ to the tube; the narrowest section and the hiss
  void area_step() noexcept;                     // the control step of area mode
  void resample_areas(unsigned n) noexcept;      // src_d_ stretched over n sections into dt_
  void wake() noexcept;                          // clears the DSP state (not the smoothers' targets) and snaps the shape
  unsigned size_to_sections(int steps) const noexcept;
  void apply_ends() noexcept;

  Quality quality_;
  unsigned group_;                               // 2 or 4: frames per tract sample
  unsigned n0_;                                  // 22 or 11

  Tube tube_;
  HalfbandDown down_[2];
  HalfbandUp up_[2];
  alignas(8) float queue_[4] = {};               // the up-sampled tract sample: one value per frame of the group
  alignas(8) float diam_[Tube::kMaxSections] = {};
  // Area mode (set_areas): the caller's diameters (normalised, clamped), their stretch over the tube, and the bookkeeping. All of it is
  // touched from the unit's one context (set_areas and process are called from the same thread, like every setter).
  alignas(8) float src_d_[kMaxAreas] = {};
  alignas(8) float dt_[Tube::kMaxSections] = {};

  // Targets (set from the unit's thread, read in process: the unit calls both from one context) and smoothed values.
  float vowel_t_ = 0.0f, vowel_ = 0.0f;
  float bend_t_[3] = {0.0f, 0.0f, 0.0f}, bend_[3] = {0.0f, 0.0f, 0.0f};
  float clos_t_[3] = {0.0f, 0.0f, 0.0f}, clos_[3] = {0.0f, 0.0f, 0.0f};
  float res_ = 0.5f, hiss_ = 0.0f;
  float mix_t_ = 1.0f, mix_ = 1.0f;
  float lvl_t_ = 1.0f, lvl_ = 1.0f;              // the wet gain of RES, smoothed once a control step
  float gain_ = 1.0f;                            // lvl_ * trim_ * seal_, refreshed once a control step
  float seal_ = 1.0f;                            // 1 - the largest closure: the wet gain of a closure (a sealed tube still rings)
  float dip_ = 1.0f;                             // the wet gain of a size change (a 5 ms dip)

  float fs_ = 44100.0f;
  float glide_ms_ = 20.0f;
  float glide_a_ = 0.0f, clos_step_ = 0.0f, mix_step_ = 0.0f, dip_step_ = 0.0f;
  float hiss_amt_ = 0.0f, env_ = 0.0f;
  float agc_a_ = 0.0f, ed_ = 0.0f, ew_ = 0.0f, trim_ = 1.0f;   // the level trim: followers of the tube's input and output power, the gain
  std::uint32_t rng_ = 1u;

  unsigned phase_ = 0;                           // position of the next frame inside its group
  unsigned ctl_ = 0;                             // frames to the next control step
  unsigned hiss_j_ = 0;                          // the narrowest section
  unsigned n_pending_;                           // the size asked for
  unsigned src_n_ = 0;                           // the number of areas in src_d_
  unsigned live_n_ = 0;                          // the section count diam_ and dt_ are laid out for in area mode; 0 or another N: snap
  std::uint8_t dip_state_ = 0;                   // 0 idle, 1 falling to 0, 2 rising to 1
  std::uint8_t area_mode_ = 0;                   // 0 the vowel path, 1 areas rule, 2 gliding back to the shape
  bool area_new_ = false;                        // src_d_ has new values to stretch
  bool shape_dirty_ = true;
  bool dormant_ = false;                         // the last block was a bypass
};

static_assert(sizeof(Processor) <= 1536, "the processor is under 1.5 KB of state");

}  // namespace o3sdk::extras::tract
