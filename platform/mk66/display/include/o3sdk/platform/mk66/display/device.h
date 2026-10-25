// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66 half of the display driver: DSPI2 as the FT812's Link, the eDMA as its BulkEngine, and the calls the image's main makes.
// Implemented only in the MK66 image build (display/src/device.cpp); the declarations compile everywhere so the header check passes
// on every platform.
//
// How main.cpp uses it (main thread only; nothing here is for an ISR except device.cpp's own DMA completion handler):
//
//   1. Before: the reset sequence has run boot::dma_init() (DMA_CR and every DCHPRI: this driver writes neither) and
//      boot::init_interrupts() (the priority of the DMA15_DMA31 line), and boot::early_pins() left chip select and PD# high.
//   2. const BringUpResult r = bring_up();  BLOCKS about 0.2 s with a healthy display, up to about 1.6 s with no chip (every wait
//      bounded). r.up false: the display stays refused and dark, the image carries on headless (frames and uploads refused, so the
//      Runtime's asset upload reports false and its frames are skipped). r.dma false: everything goes by the polled path, slower.
//   3. Hand display() to the app::Platform as its hal::Display. The Runtime's start() uploads the assets (each 4 KiB chunk waits,
//      bounded, for the one before it: never Busy on a display that is up) and its poll() draws.
//   4. Every pass of the main loop, BEFORE rt.poll(): service(). It finishes a transfer and issues the swap behind a frame, watches
//      the swap, and runs the backlight ramp. Cheap when idle (at most one register read a millisecond); bounded otherwise, except
//      after a swap stall, where it re-initialises the chip once (blocks like bring_up(), once in the image's life). Act on its
//      answer: NeedsAssets (the chip was re-initialised: upload the assets again with ui::upload_assets(display()), then call
//      display().assets_loaded(); frames are refused until then), Failed (given up, held dark for good: show the blink code).
//   5. Around any blocking wait made with boot::delay_ms() once the display is up (the codec's start and unmute), install idle() as
//      the delay's idle hook (boot::set_idle_hook(&display::device::idle)) so the swap and the backlight ramp go on during the wait.
//      idle() does nothing while the driver itself is the one waiting (the chip's bring-up waits with the same delay), so the hook
//      may stay installed for good.
//   6. hold_dark() for the safe state (a fault found, an app that cannot start): the backlight off and held off for good.
//
// Frame pacing. begin_frame() says false while a frame is on the wire or its swap (REG_DLSWAP, at the next frame start of the
// panel's 54 Hz scan-out, within 18.5 ms) has not happened: the app skips that frame (hal/display.h). So frames never outrun the
// panel. Backlight: off until the first frame the app committed is on screen (its swap seen by service()), then a ramp of about
// 130 ms to kBacklightDefault; if no frame comes, the ramp starts 2 s after bring_up() anyway. Nothing is lit before a real frame.

#include <o3sdk/platform/mk66/display/chip.h>
#include <o3sdk/platform/mk66/display/display.h>

#include <cstdint>

#ifndef O3SDK_DISPLAY_ROTATE
// The panel orientation of this build, REG_ROTATE 0..7: the CMake cache variable O3SDK_DISPLAY_ROTATE (display/CMakeLists.txt) defines
// it for everything that links platform_mk66_display; this default is for a compile outside that. 1 (inverted landscape) was proven
// upright on the maintainer's unit with this driver (track3r-os OS-HW1); 0 (landscape) shows it upside down.
#define O3SDK_DISPLAY_ROTATE 1
#endif

namespace o3sdk::platform::mk66::display::device {

inline constexpr unsigned kMaxAttempts = 3;  // whole bring-up sequences (each with its own PD# pulse) before giving up

static_assert(O3SDK_DISPLAY_ROTATE >= 0 && O3SDK_DISPLAY_ROTATE <= 7, "REG_ROTATE is three bits");
inline constexpr Orientation kBuildOrientation = static_cast<Orientation>(O3SDK_DISPLAY_ROTATE);

struct BringUpResult {
  InitReport init;         // the report of the last attempt
  unsigned attempts = 0;   // sequences run (1..kMaxAttempts)
  bool up = false;         // the chip is up, the scan-out runs and the display accepts frames
  bool dma = false;        // the DMA path passed its self-test (false: everything goes by the polled path)
};

// What service() found, for main to act on (see above).
enum class Status : std::uint8_t {
  Ok,           // up and serviced
  Down,         // bring_up() did not bring the chip up (or was not called): nothing to service
  NeedsAssets,  // re-initialised after a stalled swap: RAM_G is presumed lost, frames are refused until assets_loaded()
  Failed,       // given up (a second stall, or the re-init failed): held dark, everything refused, for good
};

// Brings up SPI2, the eDMA channels (their descriptors, DMAMUX and interrupt line only) and the chip. Boot time, main loop
// context. Takes about 0.2 s with a healthy display, up to about 1.6 s when the chip is absent.
BringUpResult bring_up() noexcept;

// The result of the last bring_up() call, for a debugger and the diagnostics to come: whether the chip was already running at
// entry, how many sequences it took, whether the DMA path was proven.
const BringUpResult& last_bring_up() noexcept;

// The image's one display (a hal::Display) and chip. Frames and uploads are refused until bring_up() succeeded.
Ft812Display& display() noexcept;
Chip& chip() noexcept;

// Every pass of the main loop: Ft812Display::service(), then what main must know.
Status service() noexcept;

// The body of boot::delay_ms()'s idle hook: services the display once (as service() does) unless the display is not up, or the
// driver is itself inside a wait of its own (the hook is then reached from the driver, which is not re-entrant).
void idle() noexcept;

// The safe state: backlight off and held off (it never ramps again).
void hold_dark() noexcept;

}  // namespace o3sdk::platform::mk66::display::device
