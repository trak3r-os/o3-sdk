# o3sdk: the framework guide

Status (2026-10-25): the kit, the simulator and the MK66 platform exist, and HELLO and o3-303 were installed and run on a unit over USB the
same day (`docs/hw/2026-10-25-usb.md`). Section 10 says, per capability, what is verified on the unit, what is done on the host only and what is
planned. This file is the reference; `docs/NEW-APP.md` is the path for a new instrument (zero to an image on the unit), `AGENTS.md` the guide
for coding agents.

## 1. The idea

track3r-os is an operating system that hosts modules (`.tmod`): the OS owns the screen, the transport and the memory, and a module
lives inside the limits it is given. o3sdk turns that around. **An app is the product** and the kit is a library under it:

- An app is one firmware image. It owns the whole screen, all 48 pads, all 33 buttons, the encoder, the CPU and the RAM.
- The kit supplies what every app needs and nobody wants to write twice: drivers, the audio engine, the UI toolkit, the input
  conventions, storage, MIDI, the simulator.
- Everything is linked statically. There is no ABI to freeze: the compiler optimises across the app and the kit, and hot code goes to
  SRAM with an attribute. Loadable units from the SD card can come later, when someone needs to ship a unit without a rebuild.
- Switching apps means flashing another `.ptf` through the bootloader's update path (track3r-os `platform/mk66/README.md`, "What the
  bootloader needs"): today over the USB console (`o3console.py --put`, then `--update`; verified on the unit, `docs/NEW-APP.md` section 11)
  or by card. The on-screen firmware switcher and an app launcher are planned (section 10).

## 2. Layers

```
APP      apps/<name>      one image: which devices it has, its screens, its global keys          HELLO, o3-303
DEVICE   (inside apps)    a convention, not a kit type: the unit(s), sequencer, screen and keymap that belong together
UNIT     units/<name>     DSP only: a parameter table and process(); the API follows CLAP        SYNTH, DELAY
KIT      kit/             core · hal · eve · ui · dsp · app · usb; platform/ and sim/ implement the HAL
```

Link dependencies enforce the layering: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`, `core <- usb` (the USB device library, linked by
the MK66 platform and never by an app); a unit depends on `dsp` only; an app on `app` and its units. `platform/<name>` and `sim` implement the HAL contracts (`kit/hal`) and run an app.

| Library | What it holds |
|---|---|
| `core` | rings, fixed containers, input events, ticks, the gesture recogniser, parameter descriptors and their maths (`param.h`), placement (`placement.h`). No heap. |
| `hal` | contracts only: `Display`, `Input`, `Leds`, `Clock`, `audio::Device` and the block callback, storage, cycles. |
| `eve` | FT81x display-list encoders and the frame builder. |
| `ui` | the immediate-mode renderer, colour tokens and themes, the four faces, the components (strip, badge, soft keys, bar, meter), plots. |
| `dsp` | the unit contract, events, the transport, the engine. |
| `app` | the panel, inputs, screens, pad LEDs, the F-key + encoder convention, shared widgets, the runtime, the console's hooks (`diag.h`). |
| `usb` | the portable USB device library: descriptors, control pipe, CDC, the `diag` shell, the log ring, USB audio. The MK66 platform links it; an app reaches the console only through `App::describe_state` and `diag_commands`. |

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

Writing one: `docs/NEW-APP.md` section 3 is the step list, and `templates/app/units/voice/` the smallest worked unit (an instrument with
three parameters, a click-free level, a test against the engine). `units/synth` (an instrument) and `units/delay` (an effect with SDRAM
buffers) are the kit's; o3-303's `units/tb303` is a port with an oracle test. Add `o3sdk_unit(<name> SOURCES …)` and test the unit on the
host against the engine (`tests/units_test.cpp`).

**Smoothing is the unit's.** The engine never ramps a parameter: `set_param` and `step_param` clamp the value, keep it as the target
(`param()` always reports it) and send it as one `Param` event, a step, at frame 0 of the next block (a clocked hook's at its own frame).
A value that clicks when it jumps (a level, a cutoff) is ramped inside `process()` (`dsp/smooth.h`: `Ramp`, `OnePole`). The descriptor
flag `core::kParamSmooth` (declared in `kit/core/include/o3sdk/core/param.h`) is **advisory metadata and nothing more**: no kit code reads
it. Set, it is the unit author's statement that the unit smooths this parameter itself, so a UI or host may change it at any rate and by
any amount (an encoder turned fast, a motion lane, automation) without a click; the unit's test should prove it. Clear, the unit makes no
promise and applies the value at the event's frame (a switch, a choice, a time, a value meant to jump). A UI or host may take the flag as
the author's word only; the kit does not enforce it, and a missing flag is not a defect. (`ParamChange::ramp_samples` in the same header is
a leftover of track3r-os's engine that o3sdk's does not use.)

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
clocked hooks ─events─▶ slot 0..7: source ─ insert ×4 ─ level/pan ─┬──────────────▶ master ─ insert ×4 ─ level ─ clip ─▶ out
(sequencers)                                                        ├─ send A ─ fx ─┘
main loop ──commands──▶                                             └─ send B ─ fx ─┘
```

Each slot and the master have up to `kMaxInserts` = 4 inserts in series (an unused one costs nothing).

- **Setup** (main loop, before `start()`): `attach()` units (up to 16), `set_source()`, `set_insert()`, `set_send_effect()`,
  `set_master_insert()`, `add_hook()`. A refused call (a slot or index out of range, an id never attached, an effect given as a source,
  the engine full, a call after `start()`) returns `false` (`attach`: `kNoUnit`) as it always did, and is also **recorded**:
  `Engine::setup_errors()` is the count (0 for a setup that went through), `setup_error(i)` the i-th record (`SetupError`: the call,
  the `SetupReason`, the slot or bus, the insert index and the unit id it was given; the first `kMaxSetupErrors` = 8 are kept) and
  `setup_call_name()` / `setup_reason_name()` give text. So `(void)e.set_source(...)` no longer hides a mistake: `sim::Harness::start()`
  returns `false` when the count is not 0, writes the refusals to stderr (`setup refused: set_source(slot 9, unit 0): slot, bus or index
  out of range`) and `<app>_sim` exits 1 with that message before it runs anything. Setup-time bookkeeping only: nothing in the render
  path reads it, and an app whose setup succeeds sees no change. The image on the Tracker does not show it yet (the count is
  kept and not reported: a follow-up for the console's `state`).
- **Running** (main loop): `note_on()`, `set_param()`, `step_param()`, `set_mix()`, `play()`, `stop()`, `set_tempo()`. Each queues a
  command on a lock-free ring; the render applies it at frame 0 of its next block. A full ring refuses and counts; nothing blocks.
- **Render**: `render()`, once per block, from the platform's audio callback (`Runtime::block_callback()`).
- **Clocked hooks** (`Clocked::block(BlockContext&)`) run in the render context before the units and place events at any frame:
  sequencers, arpeggiators, a MIDI clock. Timing is sample-accurate.
- **A hook writes the mixer** with `BlockContext::set_mix(slot, field, value)` (a motion lane, a slew; `field` is a `MixField`: Level,
  Pan, SendA, SendB, Mute). The value is clamped as `Engine::set_mix` clamps it, becomes the field's target at once, and the mixer
  ramps the gain to it across the block, from where the last block left it, as for a main-loop write: a jump never clicks. It costs a
  few instructions and has no queue, so a hook may call it as often as it likes, but the mixer has one ramp per block (up to 64 frames, not
  frame-accurate like an event): several writes to a field in a block collapse to the last, so a slew written every 16 frames
  lands at the block's rate. `BlockContext::mix(slot, field)` reads the target as the render side stands (a slew starts from it).
  **The rule: each block applies the main loop's queued commands first, then runs the hooks in the order added, then the mixer reads
  the targets; for a field, the last write in that order wins.** A hook's write beats a main-loop write applied in the same block, a
  later hook's beats an earlier one's, a later call beats an earlier one. The value then stays (nothing restores the main loop's)
  until another write replaces it: the main loop's next write, applied at the start of a later block, does; a hook that writes the
  field every block (a lane) holds it against the main loop, and one that stops writing hands it back at the next main-loop write.
  `Engine::mix()` keeps reporting the main loop's last value (a hook's write is the render side's, as a `Param` event is for
  `param()`). A `slot` >= `kMaxSlots` or a `field` that is not a `MixField` is refused: `false`, nothing written (a value is clamped,
  an index never). A slot with no source takes the write (it is only a target).
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

- `App::input()` sees every input first (global keys: Play, the screen buttons; Config too today, and will not once the kit's system Config exists, below); `true`
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

### Diagnostics: the console's hooks (`diag.h`)

The platform's USB console (`state`, and the commands an app adds) reaches an app through two optional `App` methods, so `kit/app`
depends on no USB code. `App::describe_state(ctx, out, cap)` writes the app's `key=value` pairs (`StateWriter`; a value has no space;
a pair that does not fit is dropped whole); `Runtime::state_line()` puts the kit's pairs first, `app=<name> screen=<title>
playing=0|1 tempo=<mbpm> cpu=<pct> frames=<n>`, then the app's. `App::diag_commands(rows)` returns a table of `DiagCommand` (name, help
line, `void run(const DiagArgs&, DiagOut&) noexcept`); the console takes at most 16, a row whose name is not 2-7 lowercase letters,
is a kit command's, or carries a word of the update path (`update flash boot ...`) is skipped and logged. Handlers run in the main loop
and reach their app through `o3sdk_app()`. HELLO's `describe_state` is the example.

### The system Config (the kit's, in every app) — planned, not built

**Status: planned (M2b, after the remote-install round: the console is the way to change image for now); none of this exists yet.** Today the Config button reaches `App::input()` like any other
button (HELLO and o3-303 use it for a theme toggle); a new app does not give it a meaning, so that nothing breaks when the system screen lands.
What follows is the maintainer's decision (2026-10-25) for what it will be: the kit brings the machine up (clocks, SDRAM, display, input,
LEDs, audio, the crash record) and owns the **Config button**; everything else (the pads, the other buttons, the encoder, every screen) is
the app's. "Por defecto config lleva el flasheo de otro firmware": any engine built on o3sdk can change the unit to another engine.

- The runtime will take Config before `App::input()`: a press opens the system Config screen over the app's screen, Config again goes back.
  The app will never see the Config button.
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
- The kit will store its settings and the app's Config values on the card (M2b); until then they last until power-off. HELLO and o3-303 move
  their own use of Config (a theme toggle) into it when it lands.

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
not the unit's. Its PNGs are 800 x 697 (the screen above the pads); the unit's `o3console.py --shot` is the 800 x 480 screen alone. The
`--out` directory must exist. The same app runs in tests through `sim::Harness` (`sim/include/o3sdk/sim/harness.h`: `start`, `run_ms`,
`press`, `pad_down`, `turn`, `audio`, `leds`, `runtime().state_line()`), as `templates/app/tests/app_test.cpp` and `tests/sim_hello_test.cpp` do.
`Harness::start()` and the `<app>_sim` runner fail on an app whose setup had a call the engine refused (section 4, "Setup").

## 8. Rules

- Real-time paths (the render, ISRs, everything they call): no allocation, no SD I/O, no blocking, bounded loops, no `double`.
- No heap anywhere in the kit, the units or the apps; no RTTI, no exceptions.
- The CPU is the binding constraint: measure every per-block feature on the unit before relying on it.
- Warning-free under `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion`, with `-Werror`.
- Pure logic (DSP maths, sequencer arithmetic, formats) gets host tests.

## 9. The MK66 platform

`platform/mk66`, ported from track3r-os `platform/mk66` (whose README is the long account of every hardware fact and its source): the reset
sequence (clocks, SDRAM, caches, the crash record), the FT812 driver, the two TCA8418 key scanners and the encoder, the IS31FL3731 pad LEDs,
the I2S + eDMA + SGTL5000 audio pipeline behind `hal::audio::Device`, the linker script (with `.sdram_bss` placed in the SDRAM, NOLOAD, cleared
before the constructors), the USB console, USB audio, the SD card read path and the firmware update path (M2b-R), and `src/main.cpp`, the
composition root that wires them to `Runtime`. `o3sdk_app()` links an app into an image (`<name>.elf`, `.hex`, `.bin`, `.ptf` behind the
static checks; ctests `<name>_image`, `_flashconfig`, `_ramfunc`, `_usb`). Its boot path, memory map, blink codes, options and what is verified:
`platform/mk66/README.md`; the host tools and static checks: `platform/mk66/tools/README.md`; the plans and who owned what: `docs/M2-PLAN.md`,
`docs/M2B-PLAN.md`. The drivers are the ones the unit proved in track3r-os and stay as they are there (`AGENTS.md`, "Rules").

## 10. Capabilities: the whole hardware, for every app

The kit's promise is that an app can use everything the unit has, through a contract, without touching a register: an engine or
machine (the 303, HOOKS, a port) picks what it needs. Each capability has a driver in `platform/mk66`, a contract in `kit/hal`
(or the kit layer that owns it), an API an app reaches through its `Context`, and a fake on the host so the simulator runs it.

**State, as of 2026-10-25.** Three values, and nothing in between:

- **Verified on the unit**: run on a Tracker and read back or recorded over USB by the test session (`docs/hw/2026-10-25-usb.md`: HELLO and
  o3-303, installed over the console). Where a person is still needed (eyes, hands, ears) the row says so: that part is not verified.
- **Host only**: code and host tests exist (against fakes); it has not run on a unit, or the unit has not exercised it.
- **Planned**: not built, or a contract header with no driver.

| Capability | Hardware | Contract | What an app gets | State |
|---|---|---|---|---|
| Display | FT812, 800×480, SPI + eDMA | `hal::Display` | screens, widgets, themes (`kit/ui`, `kit/app`) | **Verified on the unit** (M2): boots, draws every frame, `--shot` returns a clean 800×480 picture. Not looked at by eye; the shot's colours differ from the simulator's (the theme or the shot's colour order: open); the FT812's per-line fill rate is untested (`apps/fillrate`) |
| Buttons, pads, encoder | 2× TCA8418, FTM2 quadrature | `hal::Input` | `Input` events, `Panel` state, F-key + encoder (`ParamKeys`) | **Verified on the unit** as far as the console reaches (M2): no blink code at boot (13 would be a scanner that did not answer), no ghost input in 10 s, injected `key`/`pad`/`enc` drive the app. Real presses by hand: not verified |
| Pad LEDs | IS31FL3731, 48 monochrome | `hal::Leds` | `PadLeds` per screen | **Verified on the unit** by readback (M2): online, 0 errors, in sync, a pad's level follows its press. By eye: not verified |
| Audio out | SGTL5000, I2S0 + eDMA, 44.1 kHz | `hal::audio::Device` | the engine: units, slots, sends, master, transport | **Verified on the unit** (M2): the pipeline runs with 0 underruns and 0 faults over a 10-minute soak; the engine's master was recorded over USB (HELLO a pad at 523 Hz; o3-303 playing). The analogue outputs by ear: not verified |
| CPU and memory | M4F 180 MHz, 256 KiB SRAM, 16 MiB SDRAM | `hal::cycles`, `O3SDK_BIG` | the render's load in the header and `o3console.py cpu`; SDRAM buffers; later an SDRAM arena | **Verified on the unit** (M2): HELLO renders 19.6 % of a block at idle; o3-303 at most 39.8 % playing; SDRAM test and `.sdram_bss` buffers run |
| Crash record, status LED | noinit SRAM, PTC19 | platform | a blink code and the record of the last crash | **Host only** (M2): the images boot without a blink code, but a crash or a code was never provoked on the unit, so the record's survival of a reset is not verified |
| USB console and update | USB0 device, CDC | `hal::firmware` | `diag` over USB: version, state, cpu, log, leds, screenshot, input injection, `fw put/arm/update`; an app adds its own state and commands (`App::describe_state`, `diag_commands`) | **Verified on the unit** (M2b-R): images put, installed and read back; the way back to track3r-os v1.0.0 in about 45 s |
| USB audio out | USB0 device, isochronous, 44.1 kHz, 16-bit, composite with the console | platform, every app | the master streamed to the computer as a class-compliant interface (channels 7-8 of 8; 1-6 are silent, kept for an app's stems) | **Verified on the unit** (M2b-R): AUDIO set, profile 8, 8 out / 2 in, configured, recorded. Stems beyond the master and the computer's return stream into the engine: planned |
| SD card | SDHC, FAT (FatFs) | `hal::storage` (read-only) | the card is mounted and read; `fw put` writes `Firmware/` | **Verified on the unit** for the read path and for the update path's writes (M2b-R). **An app has no file API**: `hal::storage` is read-only by contract, `hal::storage_write` is declared with no implementation, and the update path is the card's only writer (`check_writers.py`). Saving patterns, kits and settings: **planned** |
| Firmware switcher | the bootloader (`.ptf` from `Firmware/`), track3r-os's update module | kit system screen, every app | Config, FIRMWARE: every app lists the `.ptf` images in a card folder, checks one, confirms with a hold, stages it and asks the bootloader to flash it, so the unit changes engine by changing image (the maintainer's requirement, 2026-10-25) | **Planned**. What exists is the same path driven from the computer (USB console row), which is verified on the unit |
| System Config screen | — | kit, every app | the Config button and the kit's pages (section 5) | **Planned** |
| MIDI | DIN in/out (UART), USB MIDI | kit `midi` (new) | notes and CCs into the engine as events, clock in/out on the transport, MIDI learn | **Planned** (M2b): the USB descriptors keep the MIDI interfaces and the port is silent; DIN is not ported |
| Power switch | PTC3 | `core::PowerHold` | a clean shutdown: silent, dark, card released | **Planned** (M2b): the pure logic is host-tested; no driver |
| Audio in | SGTL5000 ADC: line in, microphone | `hal::capture` | input planes in every block (`in_l`, `in_r` of a unit; `BlockContext::input_l()` of a hook), input select and gain, recording into SDRAM | **Host only** for the input planes (the RX ring runs in the pipeline; line in on the unit is not verified). Input select, gain, microphone and recording: **planned** (M2c): `hal::capture` is a contract with no driver |
| FM radio | Si4703 into the codec input | `hal::capture` | tune, seek, RDS; a source to sample | **Planned** (M2c): contract only |
| Computer return stream | USB0 isochronous (UAC2) | platform | the computer's audio as an engine source | **Planned** (later) |

Not a hardware capability but part of the promise: the **engine** (units, slots, sends, the transport, clocked hooks) is verified on the unit
through HELLO's riff and o3-303's 303 voice and sequencer; the **simulator** and the **gate** (`fw-gate-check`) are host tools.

Rules for adding one: the contract is designed from what an app needs (not from the register map), it has a host fake and a
simulator command, and an app that does not use a capability pays nothing in the render and as little flash as possible.

## 11. Using o3sdk from another project

An app can live in its own repository (o3-303 is the first) and use o3sdk as a sibling checkout: no copy, and no edit of o3sdk from
there. **Start from the template**: `python3 -I tools/new_app.py <dir> <app-id>` writes a repository laid out like o3-303's (`templates/app`:
CMake, one app and screen, one unit, tests, a scenario, `CLAUDE.md`, `AGENTS.md`, the CI scripts and the agent harness); `docs/NEW-APP.md` is the
tutorial. What the template's top-level `CMakeLists.txt` does, in its essentials:

```cmake
cmake_minimum_required(VERSION 3.24)
set(O3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../o3-sdk" CACHE PATH "The o3sdk framework")
include("${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")   # BEFORE project(): O3SDK_PLATFORM, the toolchain, the compile settings
project(myapp LANGUAGES C CXX)
enable_testing()                                    # the project's own root, for its ctests
add_subdirectory(${O3SDK_DIR} o3sdk)
add_subdirectory(units)                             # o3sdk_unit(...)
add_subdirectory(app)                               # o3sdk_app(myapp SOURCES ... DEPENDS ...)
add_subdirectory(tests)                             # o3sdk_app_test_suite(myapp ...), host only
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
  `<name>_ramfunc` and, for an image with the console, `<name>_usb`; `LOGO`, below), `o3sdk_test_suite` (doctest: one executable, one ctest per
  `*_test.cpp`; a test that names `o3sdk_app()` links the app library whole, `DEPENDS "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<name>>"`) and
  `o3sdk_app_test_suite(<name> SOURCES ... [DEPENDS ...])`, the same with an app's wiring done: the executable `<name>_tests` linked with
  `app`, `sim`, `platform_host`, the DEPENDS and the app library whole, and `../app/scenarios` as the macro `APP_SCENARIO_DIR`
  (`SCENARIO_DIR`, `SCENARIO_DEFINE` and `EXE` change that). The explicit `o3sdk_test_suite` form keeps working.
- **The boot logo** (`LOGO <png> [LOGO_COLOR 0xRRGGBB]` in `o3sdk_app`; OS-003f): the first frame the display shows is the logo centred on black with
  the app's name and build line under it (`ui/boot_screen.h`). The kit's default is the O3 logo (`logo/o3.png`, a halftone: 684 x 420, one bit a pixel
  at its own size, 36,120 bytes of flash). An app gives its own: the build runs `tools/logo_convert.py` on the PNG (a path from the app's directory)
  into the app's build directory and links that bitmap instead; without `LOGO` the app has the default. The converter chooses the native size, the scale
  and the format: the finest native size whose bitmap, padding and palette fit `kBootLogoMaxBytes` (64 KiB, `ui/boot_logo.h`: the RAM_G region of the map
  and the most flash a logo costs), shown at most 800 x 420 px, at an integer scale (1 up to 16); a nearly black-and-white picture (a halftone, line art)
  is **L1**, any other grey picture **L2 or L4**, and a picture with colour (any pixel with chroma over 32) **PALETTED565**: an 8-bit index a pixel and a
  palette of at most 256 colours (index 0 black, median cut, no dithering) after it in the same blob, drawn in one pass with `PALETTE_SOURCE`, which costs
  512 bytes more and one more display-list word. A grey logo is drawn white, or in `LOGO_COLOR` (`COLOR_RGB`; refused for a picture with colour). A
  picture over a limit fails the build with the converter's message, and so does a header that disagrees with the tool. The draw code reads width, height,
  format, stride, scale, palette and tint from the linked data (`ui::BootLogo`, `boot_logo()`): the generated file defines `boot_logo()` strongly and the kit's
  default is weak, so there is one definition in an image and no duplicate symbol. `python3 -I tools/logo_convert.py --png <png> --report --preview <out.png>`
  says what it chose and writes the native bitmap at the size it is shown; the simulator's `boot <name>` command draws the boot frame (`sim/README.md`).
  Hardware facts a unit still has to show: the 565 palette's channel widening, that `COLOR_RGB` white leaves a palette untouched, and the block edges of a
  scale that is not a power of two.
- **Where o3sdk is**: `-DO3SDK_DIR=<path>` or the environment variable `O3SDK_DIR` (the template reads it; `tools/ci.sh` too), default the
  sibling `../o3-sdk`. A cache entry keeps the value of the first configure.
- **A path inside o3sdk** is `O3SDK_ROOT` (set by the platform file), never `PROJECT_SOURCE_DIR`, which belongs to the project that
  calls the function. A variable a helper reads must be a cache entry (`O3SDK_PYTHON`, `O3SDK_SINGLE_PRECISION_LAYERS`).
- **The build identity** in the image (`gen_build_info.cmake`) is two git states: the kit's (o3sdk's own directory, wherever the project
  keeps it) and the app's (the top-level project's; the same tree when the app is one of o3sdk's own). `O3SDK_IMAGE_LABEL` joins them,
  "o3sdk d8c4ede2c609" or "<app label> / o3sdk <kit label>", and is the boot screen's build line and the image's `g_build_label`
  (`strings` on the `.bin` or the `.elf` shows it; the HEX and the `.ptf` store hex digits).
