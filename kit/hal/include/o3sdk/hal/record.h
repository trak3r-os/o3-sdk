// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The recording HAL (docs/M2C-PLAN.md section 6): what an app asks of the platform to record a take of the audio into a buffer it owns. The audio of the take is
// written by the render, so the contract is the CONTROL side, in the main loop only; the platform's implementation is platform/mk66/audio's RecordService (the same pure code
// in the simulator, so a take there is the unit's to the sample). The app layer's face of it is Context::recorder() (app/recorder.h).
//
//   The take. An app hands over a Take: a buffer of its own (an `O3SDK_BIG` array in the SDRAM; the kit allocates nothing), its capacity in frames, whether the take is
//   mono (the mix of the two channels, or the left, or the right one) or interleaved stereo, and when it starts.
//
//   The tap. Input is the block the engine is given: what the codec delivers (or what the USB return makes of it) through the line's channel map (set_line_map(): the runtime tells the
//   recorder the map AudioIn applies, so a take of a LineLeft source is the left channel in both planes, as the units hear it; to within a rounding of the last bit, the map being integer here
//   and float in the engine). It is NOT gated: the 200 ms an input switch settles are recorded as they are. Master is the block the engine made, after the master
//   level, which is what the main output plays (a resample). Chosen with set_tap() while the recorder is idle; it applies to the next take.
//
//   Start. Free: at the next block. Step, Bar, Pattern: on the transport's grid, AT THE SAMPLE of the sixteenth (a Bar is 16 sixteenths; a Pattern is the app's pattern length,
//   set_pattern_steps(), 16 by default); the grid is the transport's own, straight (a sequencer's swing is not applied), and while the transport is stopped the take waits (Waiting)
//   and starts when it plays: nothing falls back to Free. Level: the take starts when a sample of the tapped channel reaches `threshold` (an int16 magnitude, 1..32767), with
//   `preroll_frames` of what came before it kept; it needs no transport. A Master take starts on exactly the sample of the step; an Input take is what the codec delivers from
//   that sample on, which lags the timeline by the loop's round trip (nothing here compensates it).
//
//   End. disarm() ends the take at the next block, a full buffer ends it (the frames that did not fit are counted, overrun(), and never overwrite anything), stop_at(Step | Bar |
//   Pattern) ends it at the sample of the next such step (the take is exactly the frames between the two samples), `auto_steps` ends it by itself that many sixteenths after it
//   began, and a transport that stops ends a take that has a stop_at() pending where it stops.
//
//   States. Idle (nothing; arm() is allowed), Waiting (an aligned start has not come), Listening (a Level take waits for its level; the pre-roll is being heard), Recording (the
//   render is writing), Done (the take is in the buffer, frames() of them; release() makes the recorder Idle again). A take that was stopped before it began is Done with 0 frames.
//
//   A Level take's buffer. The first `ring_frames(preroll)` frames of the buffer are the ring the pre-roll is heard into; once the trigger has come, frames() counts from the
//   buffer's start and the audio of the take is the part after the leading `settle()` frames (settle() puts the ring in time order and zeroes what is not audio). Every other take
//   has no such lead (settle() is 0). The buffer must hold ring_frames(preroll) plus one frame for a Level take.
//
// Contexts. Every call is the main loop's. The render reads the buffer's address and the settings once, from a state change the main loop published (release/acquire), writes only
// the take's frames, and publishes frames() after them. The buffer must stay valid and untouched by the app from arm() until the state is Done and the app has called release().

#include <o3sdk/hal/audio.h>

#include <cstdint>

namespace o3sdk::hal::record {

enum class Tap : std::uint8_t { Input = 0, Master };
enum class Channels : std::uint8_t { Mix = 0, Left, Right, Stereo };
enum class Align : std::uint8_t { Free = 0, Step, Bar, Pattern, Level };
enum class State : std::uint8_t { Idle = 0, Waiting, Listening, Recording, Done };
// How the line's two channels reach the engine's input (app/audio_in.h): as they are, their mean in both planes, the left in both, the right in both.
enum class LineMap : std::uint8_t { AsIs = 0, Mix, Left, Right };

constexpr unsigned channel_count(Channels c) noexcept { return c == Channels::Stereo ? 2u : 1u; }

// The frames of the buffer's front a Level take uses as its ring: the pre-roll rounded up to even, and one render block (the block the trigger is in).
constexpr std::uint32_t ring_frames(std::uint32_t preroll_frames) noexcept { return ((preroll_frames + 1u) & ~1u) + audio::kBlock; }

struct Take {
  std::int16_t* buf = nullptr;         // 4-byte aligned, room for capacity_frames x channel_count(channels) samples
  std::uint32_t capacity_frames = 0;
  Channels channels = Channels::Mix;
  Align align = Align::Free;
  std::int32_t threshold = 0;          // Level: the magnitude (int16, 1..32767) that starts the take
  std::uint32_t preroll_frames = 0;    // Level: the audio before the trigger the take keeps (the ring holds ring_frames(preroll_frames))
  std::uint32_t auto_steps = 0;        // the take ends by itself this many sixteenths after its first frame (0: it does not); an aligned take's first step is not counted
};

class Recorder {
 public:
  // false: not Idle (release() the last take first), or the take is not usable (no buffer, a buffer that is not 4-byte aligned, no room, a Level take with no threshold or no room
  // behind its ring).
  virtual bool arm(const Take& take) noexcept = 0;
  // The take ends at the sample of the next step `boundary` accepts (Step, Bar or Pattern), or where the transport stops. Free is disarm() (true). Level is no boundary: false, nothing
  // changes. Meaningful while Recording.
  virtual bool stop_at(Align boundary) noexcept = 0;
  // Ends the take at the next block (a Waiting or Listening one is Done with 0 frames).
  virtual void disarm() noexcept = 0;
  virtual State state() const noexcept = 0;
  // Frames written so far (a Level take: counted from the buffer's start, the ring included, once triggered; 0 before).
  virtual std::uint32_t frames() const noexcept = 0;
  // Frames that did not fit in the buffer (0: none lost).
  virtual std::uint32_t overrun() const noexcept = 0;
  // A Level take, once triggered: puts the ring in time order and returns the leading frames that are not audio. Idempotent; 0 for every other take.
  virtual std::uint32_t settle() noexcept = 0;
  // Done to Idle.
  virtual void release() noexcept = 0;
  // Idle only (ignored otherwise): the tap of the next take.
  virtual void set_tap(Tap tap) noexcept = 0;
  virtual Tap tap() const noexcept = 0;
  // The line's channel map for the Input tap (the runtime calls it every pass of the main loop with AudioIn's map; AsIs until then).
  virtual void set_line_map(LineMap map) noexcept = 0;
  // The app's pattern length in sixteenths, for Align::Pattern (1..255; 16 by default).
  virtual void set_pattern_steps(std::uint32_t steps) noexcept = 0;
  virtual std::uint32_t pattern_steps() const noexcept = 0;

 protected:
  Recorder() = default;
  ~Recorder() = default;  // never deleted through the interface: static instances, no heap
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;
};

}  // namespace o3sdk::hal::record
