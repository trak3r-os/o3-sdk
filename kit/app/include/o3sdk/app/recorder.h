// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app's recording (docs/M2C-PLAN.md section 6): what Context::recorder() hands the app, on top of the platform's hal::record::Recorder (Platform::recorder; null on a platform
// or a test rig without one: then available() is false, every call is refused and nothing is recorded). The contract, the take's states and the alignments are hal/record.h's; this
// class is the app's face of it, with the save to the card.
//
//     O3SDK_BIG alignas(4) std::int16_t g_take[44100 * 10];                 // the app's own buffer, in the SDRAM: ten seconds, mono
//     ...
//     hal::record::Take t;  t.buf = g_take;  t.capacity_frames = 44100 * 10;  t.align = hal::record::Align::Bar;      // at the next bar of the transport
//     ctx.recorder().set_tap(hal::record::Tap::Master);                     // the master (a resample); Input is the codec
//     ctx.recorder().arm(t);                                                // main loop, returns at once
//     ...                                                                   // state() is Waiting, then Recording; frames() grows; overrun() stays 0
//     ctx.recorder().stop_at(hal::record::Align::Bar);                      // or disarm() now: the take ends on a bar (to the sample) / at the next block
//     ...                                                                   // state() is Done
//     ctx.recorder().save(ctx.files(), "", "take");                         // Apps/<app>/take_001.wav, a job of the files service (poll files().status())
//     ...                                                                   // once the job is Done: release(), and the buffer may be armed again
//
//   save() writes the audio of the take (clip(): a Level take without its leading ring, every other take as it is) as a 16-bit PCM WAV, mono or stereo, at the engine's rate, through
//   Files::save_wav (NewOnly, the next free `_NNN`; the folder is the app's own, a project folder, or "Samples"). The buffer must stay as it is until that job has ended.
//
// Main loop only. The render never waits for this class and is never touched by it while the recorder is idle.

#include <o3sdk/app/files.h>
#include <o3sdk/hal/record.h>

#include <cstdint>

namespace o3sdk::app {

class Runtime;

class Recorder {
 public:
  using Take = hal::record::Take;
  using Tap = hal::record::Tap;
  using Channels = hal::record::Channels;
  using Align = hal::record::Align;
  using State = hal::record::State;

  // What a Done take holds as audio: `pcm` frames of `channels` interleaved samples (a Level take: after its leading ring).
  struct Clip {
    const std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::uint8_t channels = 0;
  };

  // The platform has a recorder. false: every call below is refused (arm() and save() return false, state() is Idle).
  bool available() const noexcept { return hw_ != nullptr; }

  // ---- A take ---------------------------------------------------------------------------------------------------------------------------
  // hal::record::Recorder::arm: false when there is no recorder, it is not Idle, or the take is not usable.
  bool arm(const Take& take) noexcept;
  void stop_at(Align boundary) noexcept;
  void disarm() noexcept;
  State state() const noexcept;
  // Frames written so far; a Level take counts from the buffer's start with its ring (clip() is the audio).
  std::uint32_t frames() const noexcept;
  // Frames that did not fit in the buffer (0: none lost; the take holds everything before them).
  std::uint32_t overrun() const noexcept;
  // A Level take once triggered: the ring put in time order; returns the leading frames that are not audio. clip() does this itself.
  std::uint32_t settle() noexcept;
  // Done to Idle: the take has been used (saved, or thrown away). The buffer is the app's again.
  void release() noexcept;
  // Idle only; the tap of the next take (Input: the codec, Master: what the main output plays). Tap::Input by default.
  void set_tap(Tap tap) noexcept;
  Tap tap() const noexcept;
  // The app's pattern length in sixteenths, for Align::Pattern and stop_at(Pattern) (16 by default).
  void set_pattern_steps(std::uint32_t steps) noexcept;

  // ---- The take as audio and as a file -----------------------------------------------------------------------------------------------------
  // The audio of the take once it is Done (a Level take is settled first); an empty Clip while the take is not Done, or when it holds no audio (stopped before it began).
  Clip clip() noexcept;
  // The take as a WAV in `folder` ("" is the app's), named `<stem>_NNN.wav` (Files::save_wav): false when the take is not Done or empty, or the files service refused (its status()
  // says why). The recorder stays Done: call release() after the job has ended, not before (the file's data is the take's buffer).
  bool save(Files& files, const char* folder, const char* stem) noexcept;

 private:
  friend class Runtime;
  void bind(hal::record::Recorder* hw) noexcept { hw_ = hw; }

  hal::record::Recorder* hw_ = nullptr;
  const std::int16_t* buf_ = nullptr;  // the take armed last: its buffer and channel count, for clip()
  std::uint8_t channels_ = 0;
};

}  // namespace o3sdk::app
