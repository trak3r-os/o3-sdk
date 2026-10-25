// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's audio path (track3r-os OS-005a): the hal::audio::Device on I2S0, two eDMA channels and the SGTL5000, with the
// pipeline (audio/pipeline.h) between the DMA interrupt and the block callback. Implemented in the MK66 image only (src/audio.cpp,
// with the I2C master of src/i2c.cpp); the declarations compile everywhere so the header check passes on every platform.
//
// What runs where (ADR 0013 D1):
//   * eDMA channel 13 plays the TX ring into the I2S0 data register, channel 12 fills the RX ring; each major loop is one block
//     (64 stereo frames, 256 bytes). The TX channel interrupts at every block boundary: that ISR (priority kAudioDma, in SRAM_L)
//     numbers the boundary, checks that the block due now was rendered, and pends the render line.
//   * The render line is IRQ 64 (the K66's software interrupt, priority kRender, handler in SRAM_L): it copies the newest RX block
//     and calls the BlockCallback once per block (the Runtime's, which runs the kit's engine from flash, or the test tone).
//   * start(), unmute() and mute() run in the main thread and block (the codec ramps). Nothing here is started by the reset
//     sequence: main() calls start().
//
// HOW main.cpp USES IT (docs/M2-PLAN.md, "The image"), in this order:
//
//   1. start, once, after the reset sequence (boot::dma_init() has run: start() panics if not), the display bring-up, the input
//      init and rt.start():
//        const auto r = audio_device().start(rt.block_callback());   // the bring-up image passes audio_test_tone() instead
//      It BLOCKS about 0.55 s: 5 ms for the master clock to settle, the codec's power-up with its 500 ms reference ramp, then up to
//      25 ms for the first two boundaries. The waits are boot::delay_ms(), which calls the idle hook once a millisecond, so a hook
//      set with boot::set_idle_hook() keeps the display serviced meanwhile. It talks to the codec on I2C0, which the input drivers
//      share: main thread only, nothing else on the bus during the call. The outputs are MUTED whatever it returns (AlreadyStarted aside). Result::Ok: the
//      block clock runs and the callback is being called once per block. Result::AlreadyStarted: a second call, which touched
//      nothing; the path of the first start is still running (not stopped, still muted or open as it was). Anything else
//      (CodecAbsent, CodecIo, ClockFault): the I2S and the DMA are stopped, nothing runs, and for a ClockFault
//      audio_clock_fault() says which wait ran out. A failed start (not AlreadyStarted) may be retried later.
//   2. audio_arm_starve_guard(), once, right before the loop (with or without a running path).
//   3. every pass of the loop:
//        audio_main_pass();                                  // the beat: once per pass, wherever in the pass, never skipped
//      and every 100 ms or so (the snapshot is a few dozen loads):
//        const audio::StatusSnapshot s = audio_status().snapshot();
//        gate.observe(s.boundaries, s.underruns, s.hw_faults);       // OutputGate gate (output_gate.h), one for the run
//        // s.boundaries that has not moved for 300 ms is a stopped block clock (an LED code, as in track3r-os);
//        // s.fault_seen / s.governor_over are the sticky audio LED codes; s.render_last / s.render_max the render's cycles
//        if (started && !opened && gate.ready(s.boundaries) && retry.ready(false, now_ms)) {   // OpenRetry retry
//          if (audio_device().unmute()) { opened = true; retry.succeeded(); } else { retry.failed(now_ms); }
//        }
//      unmute() BLOCKS about 0.2 s (the DAC's 50 ms soft ramp, then the headphone volume up one 0.5 dB step every 2 ms, then the
//      amplifier key), again through boot::delay_ms() and on I2C0. The render keeps running meanwhile (it is an interrupt); the
//      guard does not shed for it, since a light render resets its count.
//   4. audio_device().mute() whenever the outputs must close (amplifier key first, then two codec writes, well under a millisecond);
//      main thread only. The outputs then stay closed until the next unmute().
//
// The kit's engine reports its own cycles (Engine::set_cycle_counter(&hal::cycles): the header's CPU %); the status here times the
// whole render, the RX copy and the commit to the TX slot included.

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/audio/status.h>

#include <cstdint>

namespace o3sdk::platform::mk66 {

// The one audio device (an object in .bss, valid after the static constructors).
hal::audio::Device& audio_device() noexcept;

// What the audio path publishes about itself (counters, render cycles, jitter): audio_status().snapshot() from the main loop.
// Zero until start() initialises the pipeline; start() resets it.
const audio::AudioStatus& audio_status() noexcept;

// OS-MAINGUARD (audio/pipeline.h): arms the render's main-loop starvation guard, now and for every later start(). The main loop
// calls it once, before its loop, and then calls audio_main_pass() once per pass; an image whose loop does not beat must not arm it.
void audio_arm_starve_guard() noexcept;
// The beat: once per main-loop pass (and from any long main-thread wait that is not boot::delay_ms(), so it never counts as a
// starved loop). A single 16-bit store; any time, even before start().
void audio_main_pass() noexcept;

// The bring-up test tone (audio/test_tone.h): 1 kHz at -20 dBFS, the left channel alone for about 6 s, then both. Each call
// rewinds the tone and returns its callback, for audio_device().start() in place of the Runtime's: an image that isolates the
// audio path from the app. Main thread, before start().
hal::audio::BlockCallback audio_test_tone() noexcept;

// Where the last start() stopped on a ClockFault (OS-HW1): which wait ran out and the raw I2S0 MCR and MDR at that moment. `where`
// is None for every other outcome. Written by start() in the main thread; read by the main thread too.
struct AudioClockFault {
  enum Where : std::uint8_t { None, McrWait, MdrWait, NoBoundaries };
  std::uint8_t where = None;
  std::uint32_t mcr = 0;
  std::uint32_t mdr = 0;
};
AudioClockFault audio_clock_fault() noexcept;

}  // namespace o3sdk::platform::mk66
