# o3sdk: the framework guide

Status: milestone M1 (2026-10-25). Everything here is host-tested; nothing has run on a Tracker yet.

## 1. The idea

track3r-os is an operating system that hosts modules (`.tmod`): the OS owns the screen, the transport and the memory, and a module
lives inside the limits it is given. o3sdk turns that around. **An app is the product** and the kit is a library under it:

- An app is one firmware image. It owns the whole screen, all 48 pads, all 33 buttons, the encoder, the CPU and the RAM.
- The kit supplies what every app needs and nobody wants to write twice: drivers, the audio engine, the UI toolkit, the input
  conventions, storage, MIDI, the simulator.
- Everything is linked statically. There is no ABI to freeze: the compiler optimises across the app and the kit, and hot code goes to
  SRAM with an attribute. Loadable units from the SD card can come later, when someone needs to ship a unit without a rebuild.
- Switching apps means flashing another `.ptf` through the bootloader's update path (track3r-os `platform/mk66/README.md`, "What the
  bootloader needs"). An app launcher in the kit is a later item.

## 2. Layers

```
APP      apps/<name>      one image: which devices it has, its screens, its global keys          HELLO, o3-303
DEVICE   (inside apps)    unit(s) + their sequencer + their screen + their keymap, reusable      (M3: Acid, M4: Hooks)
UNIT     units/<name>     DSP only: a parameter table and process(); the API follows CLAP        SYNTH, DELAY
KIT      kit/             core · hal · eve · ui · dsp · app; platform/ and sim/ implement the HAL
```

Link dependencies enforce the layering: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`; a unit depends on `dsp` only; an
app on `app` and its units. `platform/<name>` and `sim` implement the HAL contracts (`kit/hal`) and run an app.

| Library | What it holds |
|---|---|
| `core` | rings, fixed containers, input events, ticks, the gesture recogniser, parameter descriptors and their maths (`param.h`), placement (`placement.h`). No heap. |
| `hal` | contracts only: `Display`, `Input`, `Leds`, `Clock`, `audio::Device` and the block callback, storage, cycles. |
| `eve` | FT81x display-list encoders and the frame builder. |
| `ui` | the immediate-mode renderer, colour tokens and themes, the four faces, the components (strip, badge, soft keys, bar, meter), plots. |
| `dsp` | the unit contract, events, the transport, the engine. |
| `app` | the panel, inputs, screens, pad LEDs, the F-key + encoder convention, shared widgets, the runtime. |

## 3. Units

A unit is DSP only (`kit/dsp/include/o3sdk/dsp/unit.h`): an **instrument** (notes in, audio out) or an **effect** (audio in, audio
out).

```cpp
class Unit {
 public:
  virtual const UnitInfo& info() const noexcept = 0;         // name, kind, the parameter table, page names
  virtual void prepare(float sample_rate) noexcept = 0;      // main loop, before the engine starts: reset, take the defaults
  virtual void process(const ProcessArgs& args) noexcept = 0; // the render context, one block
};
```

- **Parameters** are a constexpr table of `core::ParamDesc`: range, default, curve (linear, log, exp, table), display (`Int`, `NormPct`,
  `Enum`, `Fixed1`, `Hz`…), encoder detents and coarse step. **The parameter id is the index in the table**: append, never insert.
- **The unit never owns the values the user sees.** The engine's main side stores them, formats them and steps them with the encoder;
  the unit receives each change as a `Param` event in `process()`, at its frame. At `Engine::start()` every unit receives all its
  stored values (a `process()` of zero frames), so both sides agree.
- **Events** (`event.h`): `NoteOn` (velocity, flags `kNoteAccent`, `kNoteSlide`), `NoteOff`, `Param`, `AllNotesOff`, each with the frame
  of the block it applies at, sorted. `for_each_run()` splits a block into runs between events: the usual shape of `process()`.
- **Big buffers** (a delay line, a sample) belong to the app, in the SDRAM (`O3SDK_BIG`, `core/placement.h`), and are handed to the unit
  at construction (see `units/delay`). They start at zero: the SDRAM section keeps no contents, so the cross compiler refuses a
  non-zero initialiser on an `O3SDK_BIG` object ("only zero initializers are allowed in section"; the host build accepts it).
- **Rules** in `process()`: no heap, no blocking, no file or SD I/O, no printf, bounded loops, no `double` (the build treats unsuffixed
  floating constants as float; the cross build is checked for the software double helpers).

Writing one: copy `units/synth` (an instrument) or `units/delay` (an effect), give it a table and a `process()`, add
`o3sdk_unit(<name> SOURCES …)`, and test it on the host against the engine (`tests/units_test.cpp`).

### Porting a plugin

Only the DSP of an open plugin ports; its GUI never does (you build the screen with the kit). The checklist:

1. **Licence.** The kit is GPL-3.0-or-later, so GPL and MIT code both fit; closed code cannot be ported at all.
2. **Precision.** Replace `double` with `float` everywhere in the render path; check the result against the original on the host (an
   oracle test, as track3r-os does for its TB-303 against Open303).
3. **Memory.** No allocation after start: size every buffer up front, big ones in the SDRAM.
4. **Parameters.** Map the plugin's parameters to a `ParamDesc` table; its `setParameter()` becomes the `Param` event.
5. **CPU.** A block is 64 frames, 261,224 cycles at 44.1 kHz. Measure on the unit before promising voices (track3r-os's TB-303 voice
   costs 15-22 % of a block).

Good candidates: Mutable Instruments' code (most of it MIT, written for Cortex-M4 at 72-168 MHz: Plaits, Rings, Clouds, Elements,
Braids, Warps), Open303, the mda plugins, DaisySP, Faust-generated C++. Out of reach: Vital, Surge, Serum-class synths, large
convolutions.

## 4. The engine

`kit/dsp/include/o3sdk/dsp/engine.h`. A fixed topology set up once:

```
clocked hooks ─events─▶ slot 0..7: source ─ insert ─ insert ─ level/pan ─┬──────────────▶ master ─ insert ─ insert ─ level ─ clip ─▶ out
(sequencers)                                                              ├─ send A ─ fx ─┘
main loop ──commands──▶                                                   └─ send B ─ fx ─┘
```

- **Setup** (main loop, before `start()`): `attach()` units (up to 16), `set_source()`, `set_insert()`, `set_send_effect()`,
  `set_master_insert()`, `add_hook()`.
- **Running** (main loop): `note_on()`, `set_param()`, `step_param()`, `set_mix()`, `play()`, `stop()`, `set_tempo()`. Each queues a
  command on a lock-free ring; the render applies it at frame 0 of its next block. A full ring refuses and counts; nothing blocks.
- **Render**: `render()`, once per block, from the platform's audio callback (`Runtime::block_callback()`).
- **Clocked hooks** (`Clocked::block(BlockContext&)`) run in the render context before the units and place events at any frame:
  sequencers, arpeggiators, a MIDI clock. Timing is sample-accurate.
- **The transport** (`transport.h`) counts Q32.32 ticks at 960 PPQN, advanced by an integer increment computed from the tempo and the
  one rate constant: no drift. `for_each_step(t, period, offset, swing, fn)` walks a grid (sixteenths are 240 ticks) and calls
  `fn(frame, step)` for each step that starts in the block; `Transport::started` and `stopped` mark the first block after Play and Stop.
- **Statistics** (`stats()`): peaks, the render's cycles (with a counter: the unit), dropped events, clips, the transport position;
  read through a sequence counter, so a snapshot is always one block's.

## 5. Apps

`kit/app/include/o3sdk/app/`. An app (`app.h`) owns its units, devices and screens as plain objects, wires them in `setup()` and
shows the first screen. Every app defines exactly one entry point:

```cpp
o3sdk::app::App& o3sdk_app() noexcept { static MyApp app; return app; }
```

- `App::input()` sees every input first (global keys: Play, the screen buttons), except Config, which is the kit's (below); `true`
  consumes it.
- A **screen** (`screen.h`) is what the display, the pads, the soft keys and the encoder mean right now: `input()`, `update()`,
  `draw()` into the content area, `soft_keys()` (labels, the hot key, dimmed keys; a Shift row if it wants one), `leds()` (a whole frame
  of 48 levels each UI frame). All in the main loop.
- `Context` (`context.h`): the engine, the panel's held state, time, `show()` to switch screens, the theme, `redraw()`.
- `Input` (`input.h`): presses, releases and turns, raw, with Shift's state. A press arrives at the press, so pads play at once.

### The controls convention (`param_keys.h`)

The kit's apps share one way to edit values, so the hand learns it once:

| Gesture | Effect |
|---|---|
| tap F1..F8 | the encoder drives that key's parameter from now on (latched) |
| hold an F key + turn | that one moves while held; at the release the latched one comes back |
| hold two F keys + turn | both move together |
| turn / Shift + turn | fine / coarse steps (the descriptor's `detents` and `coarse`) |
| a page button (Params on HELLO) | the next page of eight |

`knob_strip()` (`widgets.h`) draws the eight cells above the soft keys: name, value with unit, a bar, the focused cell highlighted.

### The system Config (the kit's, in every app)

The maintainer's decision (2026-10-25): the kit brings the machine up (clocks, SDRAM, display, input, LEDs, audio, the crash record)
and owns the **Config button**; everything else (the pads, the other buttons, the encoder, every screen) is the app's. "Por defecto
config lleva el flasheo de otro firmware": any engine built on o3sdk can change the unit to another engine.

- The runtime takes Config before `App::input()`: a press opens the system Config screen over the app's screen, Config again goes back.
  The app never sees the Config button.
- The kit's sections come first, the same in every app: FIRMWARE (the firmware switcher, section 10), AUDIO (USB audio out and the
  outputs), MIDI (channel, clock in and out, DIN and USB), DISPLAY (theme, brightness), INFO (the app's name and version, the build id,
  the render's load, the last crash record). A section whose driver does not exist yet is not shown.
- An app adds its own pages after them: `App::config_pages()` returns pages of up to eight items, each a value (a parameter
  descriptor with get and set on the main loop) or an action (run after a confirming hold). An app reads the kit's settings (the theme,
  the MIDI channel) through `Context`.
- An app can also extend or change the kit's sections ("engine puede añadir o cambiar también salidas usb", the maintainer): add items
  to a section, hide one, set its defaults. For USB audio the kit streams the master by default, and an app can declare more streams
  (the dry voice, a send, stems) up to what full-speed USB carries (about eight channels at 44.1 kHz, 16 bits). FIRMWARE and INFO stay
  in every app, so any unit can always be switched to another engine and tell what it runs (confirmed by the maintainer, 2026-10-25).
- The kit stores its settings and the app's Config values on the card (M2b); until then they last until power-off.

Status: planned, the first slice of M2b. HELLO and o3-303 move their own use of Config (a theme toggle) into it.

### Pad LEDs (`pad_leds.h`)

Monochrome, one level a pad. Shared levels give the same meaning on every screen: `kLedDim` (something is here), `kLedLow` (a second
kind), `kLedMid` (selected), `kLedOn` (now: the playhead, a held pad). `PadLeds::blink()` gives a blink phase.

## 6. The runtime

`runtime.h`. The same main loop on every platform:

```cpp
Runtime rt(o3sdk_app(), engine, platform);
rt.start();                                // the UI's bitmaps, App::setup, Engine::start
audio_device.start(rt.block_callback());   // the render context calls the engine once per block (int16 in and out)
for (;;) rt.poll();                        // input -> app -> screen; a frame every 33 ms (or sooner after redraw()); LEDs
```

A frame is the header strip (app name, screen title, tempo, transport, the render's load on the unit), the screen's content and the
soft-key row, as one display list (2048 words at most; HELLO peaks under 900).

## 7. The simulator

`sim/README.md`. Each app gets `<name>_sim`: the app on the host with a fake clock, scripted input, the FT81x interpreter for the
screen, a drawing of the pads with their LED levels, and the audio as a WAV. Scenarios are text (`apps/hello/scenarios/tour.txt`). Use
it for every screen change and every sound change before a unit sees it; it is not a model of a particular FT812, and its timing is
not the unit's.

## 8. Rules

- Real-time paths (the render, ISRs, everything they call): no allocation, no SD I/O, no blocking, bounded loops, no `double`.
- No heap anywhere in the kit, the units or the apps; no RTTI, no exceptions.
- The CPU is the binding constraint: measure every per-block feature on the unit before relying on it.
- Warning-free under `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion`, with `-Werror`.
- Pure logic (DSP maths, sequencer arithmetic, formats) gets host tests.

## 9. What M2 needs (the MK66 platform)

From track3r-os `platform/mk66`, adapted: the reset sequence (clocks, SDRAM, caches, crash record), the FT812 driver, the two TCA8418
key scanners and the encoder, the IS31FL3731 pad LEDs, the I2S + eDMA + SGTL5000 audio pipeline behind `hal::audio::Device`, the
linker script (with `.sdram_bss` placed in the SDRAM, NOLOAD, cleared before the constructors), and a `main` that runs `Runtime`. Then
the SD card (FatFs), USB (CDC console, MIDI) and the firmware update hand-over. The cut point is `platform/mk66/src/app_main.cpp`,
which wires the tracker; the drivers below it are reusable, but the platform library links the tracker's audio engine today
(`audio/diag.h`, `stem_map.h`, `pipeline.h`), and those ties are what M2 replaces with `hal::audio::BlockCallback` and the kit's engine.

## 10. Capabilities: the whole hardware, for every app

The kit's promise is that an app can use everything the unit has, through a contract, without touching a register: an engine or
machine (the 303, HOOKS, a port) picks what it needs. Each capability has a driver in `platform/mk66`, a contract in `kit/hal`
(or the kit layer that owns it), an API an app reaches through its `Context`, and a fake on the host so the simulator runs it.

| Capability | Hardware | Contract | What an app gets | Milestone |
|---|---|---|---|---|
| Display | FT812, 800×480, SPI + eDMA | `hal::Display` | screens, widgets, themes (`kit/ui`, `kit/app`) | M1 kit, M2 driver (implemented in M2, unit test pending) |
| Buttons, pads, encoder | 2× TCA8418, FTM2 quadrature | `hal::Input` | `Input` events, `Panel` state, F-key + encoder (`ParamKeys`) | M1 kit, M2 driver (implemented in M2, unit test pending) |
| Pad LEDs | IS31FL3731, 48 monochrome | `hal::Leds` | `PadLeds` per screen | M1 kit, M2 driver (implemented in M2, unit test pending) |
| Audio out | SGTL5000, I2S0 + eDMA, 44.1 kHz | `hal::audio::Device` | the engine: units, slots, sends, master, transport | M1 engine, M2 driver (implemented in M2, unit test pending) |
| CPU and memory | M4F 180 MHz, 256 KiB SRAM, 16 MiB SDRAM | `hal::cycles`, `O3SDK_BIG` | the render's load in the header; SDRAM buffers; later an SDRAM arena | M2 |
| Crash record, status LED | noinit SRAM, PTC19 | platform | a blink code and the record of the last crash | M2 (implemented in M2, unit test pending) |
| USB console and update | USB0 device, CDC | `hal::firmware` | `diag` over USB: version, crash, CPU, screenshot; a `.ptf` sent from the computer (no card swapping) | M2b |
| Firmware switcher | the bootloader (`.ptf` from `Firmware/`), track3r-os's update module (verified on the unit 2026-10-12) | kit system screen, every app | Config, FIRMWARE: every app built on o3-sdk lists the `.ptf` images in a card folder (name, version and build id from each image), checks the chosen one (the static image checks), confirms with a hold, stages it in `Firmware/` and asks the bootloader to flash it, so the unit changes engine by changing image; the official image too, if it is on the card. The maintainer's requirement, 2026-10-25 | M2b |
| USB audio | USB0 device, audio class 1 (isochronous, 44.1 kHz, 16-bit stereo), composite with the console and USB MIDI | kit, every app | the master output streamed to the computer as a class-compliant audio interface (no driver); input from the computer later. The same chip as the Teensy 3.6, whose USB audio works on USB0; not yet tried on the Tracker. The maintainer's requirement for o3-303, 2026-10-25 | M2b |
| SD card | SDHC, FAT (FatFs) | `hal::storage` | files: patterns, kits, samples, settings; atomic saves | M2b |
| MIDI | DIN in/out (UART), USB MIDI | kit `midi` (new) | notes and CCs into the engine as events, clock in/out on the transport, MIDI learn | M2b |
| Power switch | PTC3 | `core::PowerHold` | a clean shutdown: silent, dark, card released | M2b |
| Audio in | SGTL5000 ADC: line in, microphone | `hal::capture` | input planes in every block (already), input select and gain, recording into SDRAM | M2c (the RX ring runs in M2, reaches the callback with line in selected; unit test pending; recording and input select are M2c) |
| FM radio | Si4703 into the codec input | `hal::capture` | tune, seek, RDS; a source to sample | M2c |
| USB audio | USB0 isochronous (UAC2) | platform | the unit as a sound card: stems to the computer, return to the engine | later |

Rules for adding one: the contract is designed from what an app needs (not from the register map), it has a host fake and a
simulator command, and an app that does not use a capability pays nothing in the render and as little flash as possible.

## 11. Using o3sdk from another project

An app can live in its own repository (o3-303 is the first) and use o3sdk as a sibling checkout: no copy, and no edit of o3sdk from
there. The project's top-level `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
set(O3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../o3-sdk" CACHE PATH "The o3sdk framework")
include("${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")   # BEFORE project(): O3SDK_PLATFORM, the toolchain, the compile settings
project(myapp LANGUAGES C CXX)
enable_testing()                                    # the project's own root, for its ctests
add_subdirectory(${O3SDK_DIR} o3sdk)
add_subdirectory(units)                             # o3sdk_unit(...)
add_subdirectory(app)                               # o3sdk_app(myapp SOURCES ... DEPENDS ...)
add_subdirectory(tests)                             # o3sdk_test_suite(...), host only
```

- **The platform file** decides what has to be decided before `project()`: `-DO3SDK_PLATFORM=host|mk66`, the Arm toolchain file
  (`-DARM_GNU_ROOT=...`, CLAUDE.md), and the compile settings every target shares (C++17, no RTTI, no exceptions, the audio clock
  define, the cross flags, `O3SDK_SANITIZE`). Included after `project()` it stops the configure with a message.
- **What gets built** from the outside: the kit, o3sdk's units (`unit_synth`, `unit_delay`) and the platform (the HAL fakes and the
  simulator on the host; the drivers and the image pipeline on mk66). o3sdk's own apps (HELLO), its tests and its tools' self-tests are
  built only when o3sdk is the top-level project: `O3SDK_BUILD_APPS` and `O3SDK_BUILD_TESTS` default to that, and `-D...=ON` runs them
  from the outside too.
- **The helpers** (`cmake/O3sdkTargets.cmake`) run in the caller's directory, so sources and `include/` are the project's own:
  `o3sdk_unit`, `o3sdk_library`, `o3sdk_app` (the library `app_<name>`; on the host the simulator `<name>_sim` in the app's binary
  directory; on mk66 `<name>.elf`, `.hex`, `.bin`, `.ptf` there too, and the ctests `<name>_image`, `<name>_flashconfig`,
  `<name>_ramfunc`) and `o3sdk_test_suite` (doctest: one executable, one ctest per `*_test.cpp`).
- **A path inside o3sdk** is `O3SDK_ROOT` (set by the platform file), never `PROJECT_SOURCE_DIR`, which belongs to the project that
  calls the function. A variable a helper reads must be a cache entry (`O3SDK_PYTHON`, `O3SDK_SINGLE_PRECISION_LAYERS`).
- **The build identity** in the image (`gen_build_info.cmake`) is two git states: the kit's (o3sdk's own directory, wherever the project
  keeps it) and the app's (the top-level project's; the same tree when the app is one of o3sdk's own). `O3SDK_IMAGE_LABEL` joins them,
  "o3sdk d8c4ede2c609" or "<app label> / o3sdk <kit label>", and is the boot screen's build line and the image's `g_build_label`
  (`strings` on the `.bin` or the `.elf` shows it; the HEX and the `.ptf` store hex digits).
