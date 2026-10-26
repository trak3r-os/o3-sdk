// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66 half of the input drivers (src/device.cpp, image only): the board's pins, the FTM2 quadrature decoder, the two I2C masters
// (the platform's, src/i2c.cpp), and the objects the image wires to the app: the scan (`hal::Input`) and the pad LEDs (`hal::Leds`).
// The declarations compile everywhere so the header check passes on every platform.
//
// How main.cpp uses it (main thread only; nothing here runs in an interrupt, and the I2C transfers it makes share I2C0 with the codec,
// which is safe because both are main-thread clients of the one master):
//
//   1. After the clock tree, the SDRAM and boot::early_pins() (the reset sequence), and after the display's bring-up:
//      const bool all = init();   pins, the encoder decoder, the control bus (I2C0; a bus a slave holds is released) and the control-button
//      key scanner. BLOCKS for a few milliseconds at most (three bounded configuration attempts, a bus recovery in between). false: that
//      scanner did not answer (last_init(); a blink code is main's business): the image runs on, the scan retries it with a backoff and
//      its keys arrive once it answers. OS-BUG-PADS: the pad bus (I2C2) is NOT touched here, so neither the pad scanner nor the pad LED
//      driver has a verdict yet; see 1b.
//   1b. audio_started(), right after the audio start (worked or not): the second condition of the pad bus's gate (pad_bus.h), which
//      opens a second after the board's power rail and after this call. The first service() pass after that initialises I2C2 and
//      configures the pad scanner, and the pad LED driver follows on its first flush. The boot chords (control buttons) do not wait.
//   2. Hand input() and &leds() to the app::Platform. Events: buttons are the kit's app::Button codes (F1 = 0 ... Right = 32), pads
//      0..47 row by row from the top left (velocity 127, 0 on release), the encoder id 0 with a signed detent delta (+ clockwise,
//      see input_map.h kEncoderCountUpIsClockwise, unconfirmed on a unit). The pad LEDs set themselves up (dark first) over their
//      first flushes (once the pad bus is open).
//   3. Every pass of the main loop, BEFORE rt.poll(): service(). It drains what is due from the two scanners into the queue the app
//      polls and reads the encoder counter. Bounded: about 0.6 ms a chip at most (INT_STAT, a clear, the count and at most four events
//      at about 115 us a register), every byte bounded at 300 us; a chip that fails is left alone for a growing backoff (10 ms to
//      320 ms; the pad chips, once the gate is open and until one has answered, every second). The Runtime flushes the LEDs once a pass (one bounded transfer of at most 13 bytes); main may flush again.
//      The loop should pass at least once a millisecond (SysTick wakes it): a key is seen within one pass of its INT, and within the
//      10 ms safety poll if the INT line is not wired as assumed.

#include <o3sdk/hal/input.h>
#include <o3sdk/hal/leds.h>
#include <o3sdk/platform/mk66/input/pad_leds.h>
#include <o3sdk/platform/mk66/input/scanner.h>

namespace o3sdk::platform::mk66::input::device {

struct InitResult {
  bool buttons_online = false;  // the control-button key scanner answered its configuration
};

// Configures the pins (the INT lines as inputs with pull-ups, the encoder pins), starts the FTM2 quadrature decoder, brings up the
// control bus (I2C0; a held bus is released first) and configures the control-button key scanner. true: it answered. Bounded: a chip
// that does not answer costs three attempts and is retried later by the scan itself. Call once (see above).
//
// OS-BUG-PADS: the pad bus (I2C2) is NOT brought up here. Its gate (pad_bus.h) opens one second after the board's power rail came up
// (boot::rail_up_ms) and after audio_started(); the first service() pass after that initialises the module and configures the pad key scanner,
// and the pad LED driver follows on its first flush. So the pads are not online when this returns, and the boot chords (control buttons) do not wait for them.
bool init() noexcept;
// What the last init() found.
InitResult last_init() noexcept;

// OS-BUG-PADS: the audio start has finished, whether it worked or not (main.cpp, right after it): the second condition of the pad bus's gate. Main loop only.
void audio_started() noexcept;

// The scan as the app's hal::Input, and the pad LEDs as its hal::Leds. Valid from init().
hal::Input& input() noexcept;
hal::Leds& leds() noexcept;

// One pass of the scan: Scanner::service().
void service() noexcept;

// The concrete objects, for what the HAL contracts do not cover: the debounced state (a boot chord), the LED driver's errors.
Scanner& scanner() noexcept;
PadLeds& pad_leds() noexcept;

}  // namespace o3sdk::platform::mk66::input::device
