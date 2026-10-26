# A new instrument on o3-sdk: from zero to an image on the unit

For a person or an agent who has an idea for an instrument (a drum machine, a sampler, a port of a plugin, a groovebox) and wants to
write **only its engine**: its units (DSP), its screens, its controls and its pads. The machine bring-up, the UI kit, the engine plumbing,
the build, the simulator, the tests, the image, the gate and the remote install are already here. This is the path, one command a step,
with the real files of HELLO (`apps/hello/`, in this repository) and o3-303 (`../o3-303`, the first app in its own repository) as the
references. `docs/FRAMEWORK.md` is the reference behind it.

| Step | What | Command |
|---|---|---|
| 1 | create the repository | `python3 -I tools/new_app.py ../myapp myapp` |
| 2 | build on the host, run the tests, look at the simulator | `cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host` |
| 3 | write the engine: units, screens, controls, scenarios, tests | sections 3 to 8 |
| 4 | build the image | `cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain>` then `cmake --build build-mk66 -j4` |
| 5 | the gate | `python3 -I tools/gate.py myapp --host-build build-host` |
| 6 | install over USB, and the way back | section 11 |

## 0. Before you start

- **A computer**: Linux (the host tools assume it: `/dev/serial/by-id`, ALSA), CMake 3.24 or newer, a C++17 compiler, Python 3 (standard
  library only). `tools/ci.sh` uses `ccache` when it is installed.
- **The cross toolchain**: the Arm GNU Toolchain 15.3.rel1 (`arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi`), unpacked anywhere, from Arm's
  downloads page. Name it with `-DARM_GNU_ROOT=<dir that holds bin/>` at the configure, or export `ARM_GNU_ROOT`; `tools/ci.sh` looks for
  it in `../track3r-os/.toolchains/` when neither is set. A different release may build, but this is the one the image is built and tested
  with.
- **The framework**: o3-sdk checked out next to your app (`../o3-sdk`), used as it is. Your repository never edits it
  (`docs/FRAMEWORK.md` section 11); a change the app needs is made in o3-sdk, under its own rules.
- **For the unit**: a Tracker whose running firmware has the USB console (track3r-os v1.0.0, or any o3-sdk image built with the default
  `O3SDK_MK66_CONSOLE=ON`), a USB cable, and a FAT SD card in it with a `Firmware/` folder that holds a known-good image (section 11). A unit
  that runs neither is installed by the card, by hand (`platform/mk66/README.md`, "Getting an image onto the unit"; the naming rule and the way back
  are in `docs/hw/2026-10-25.md`, "Safety first": the reset button was seen flashing only names that start with `PolyendTracker`).
- **The rules** (`AGENTS.md`, "Rules"): no code, assets or notes from the official firmware; no heap, no `double`, no blocking in the
  real-time paths; warning-free under `-Werror`; firmware reaches a unit only through the documented update path, after the gate, with the
  maintainer's go; no binaries in git.

## 1. Create the repository

```sh
cd ~/Code                                   # o3-sdk is ~/Code/o3-sdk
python3 -I o3-sdk/tools/new_app.py myapp-repo myapp --name MYAPP --git
cd myapp-repo
```

`<app-id>` (`myapp`) is lower case letters, digits and `_`, 2 to 24 characters, starting with a letter. It becomes the CMake project, the
image (`myapp.ptf`), the simulator (`myapp_sim`), the C++ namespace and an include directory, so the tool refuses a C++ keyword and the names
o3-sdk uses (`core`, `app`, `sim`, `hello`, ...). `--name` is what the header strip and the boot screen show (upper case; default: the id
in upper case). The tool never overwrites: the destination must not exist. `--git` runs `git init` (no commit); `--no-harness` leaves out
`.claude/`.

What you get, laid out like o3-303:

```
myapp-repo/
  CMakeLists.txt            uses ../o3-sdk (add_subdirectory); -DO3SDK_DIR=<path> or $O3SDK_DIR elsewhere
  app/                      the app: CMakeLists.txt (o3sdk_app), src/app.cpp, src/play_screen.{h,cpp}, include/myapp/pad_map.h
  app/scenarios/boot.txt    a simulator scenario (screens as PNG, audio as WAV)
  units/voice/              one unit (DSP): voice.h, voice.cpp, CMakeLists.txt (o3sdk_unit)
  tests/                    doctest: voice_test (the unit against the engine), pad_map_test (pure logic), app_test (the app in the simulator)
  tools/ci.sh, remote_ci.sh, gate.py   the CI scripts and the release gate, identical to o3-sdk's (docs/ci.md)
  .claude/                  the agent harness (agents, skills, hooks), copied from o3-sdk
  CLAUDE.md, AGENTS.md      for the agents that work in this repository; README.md; LICENSE (GPL-3.0-or-later)
```

The licence: the image links the GPL-3.0-or-later kit statically, so an app is GPL-compatible, and the template says GPL-3.0-or-later.

**Keeping the harness current.** `.claude/` is a copy: o3-sdk's agents, skills and hooks move on, and the copy in your repository does not.
After you pull a newer o3-sdk, compare and then repair it:

```sh
python3 -I ../o3-sdk/tools/new_app.py --sync-harness .             # report: differs / missing / extra per file; exit 3 when it differs
python3 -I ../o3-sdk/tools/new_app.py --sync-harness . --diff      # and the unified diff of each differing file
python3 -I ../o3-sdk/tools/new_app.py --sync-harness . --write     # copy o3-sdk's version of every differing or missing file over yours
```

`--write` never deletes: a skill, agent or hook you added ("extra") stays, and a file you edited is replaced by o3-sdk's version (read the
`--diff` first and keep your change somewhere else if you need it; the diff of your repository shows what changed). `settings.json` and
`settings.local.json.example` hold the hook wiring and your own permissions, so they are reported but copied only with `--with-settings`
(`--write --with-settings`); merge by hand when you have added permissions of your own. A symlinked harness directory is not written
through. `tools/ci.sh`, `remote_ci.sh` and `gate.py` are not part of it (copy them from o3-sdk's `tools/`).

### First build, tests, simulator (about a minute)

```sh
cmake -S . -B build-host
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure           # app_test, pad_map_test, voice_test
mkdir -p /tmp/myapp && build-host/app/myapp_sim --scenario app/scenarios/boot.txt --out /tmp/myapp
```

The simulator prints the PNGs and the WAV it wrote and a summary line (`frames drawn, refused, peak list n/2048 words, audio blocks,
events and commands dropped`); exit 1 means the scenario failed or a frame did not fit. Open the PNGs: that is the screen, above a drawing
of the 48 pads with their LED levels. The `--out` directory must exist.

If o3-sdk is not next to the repository: `export O3SDK_DIR=<path>` before the first configure (or `-DO3SDK_DIR=<path>`; the value is cached,
so change it with `-D` or use a fresh build directory).

## 2. The anatomy, in four objects

| Object | File in the template | Reference | What it is |
|---|---|---|---|
| **App** | `app/src/app.cpp` | `apps/hello/src/hello_app.cpp`, o3-303 `app/src/o3_303_app.cpp` | owns the units and screens as plain objects, wires them in `setup()`, defines the one entry point `o3sdk_app()` |
| **Screen** | `app/src/play_screen.cpp` | o3-303 `app/src/main_screen.cpp`, `vault_screen.cpp` | what the display, the pads, the soft keys and the encoder mean now: `input`, `update`, `draw`, `soft_keys`, `leds` |
| **Unit** | `units/voice/` | `units/synth`, `units/delay`; o3-303 `units/tb303`, `units/distortion` | DSP only: a parameter table and `process()` per block |
| **Hook** | (not in the template) | HELLO's `Riff`; o3-303 `app/seq/` | a `dsp::Clocked` that places events at any frame of the block: sequencers, arpeggiators |

The kit supplies the rest: `kit/app` (`Runtime`: the main loop, the header strip, the soft-key row, the pad LEDs, the F-key and encoder
convention, widgets), `kit/ui` (the renderer, themes, components), `kit/dsp` (the engine: slots, inserts, sends, master, the transport),
and `platform/mk66` (clocks, SDRAM, display, input, LEDs, audio, USB console, the image).

The wiring in the template's `setup()` is the whole integration:

```cpp
dsp::Engine& e = ctx.engine();
const dsp::UnitId voice = e.attach(voice_);   // prepare() runs and the parameter defaults are stored
(void)e.set_source(0, voice);                 // slot 0 plays the voice (a refused call is counted: the simulator and app_test fail on it)
play_.init(voice);
ctx.show(play_);                              // the first screen
```

## 3. Add a unit (DSP)

A unit is an instrument (notes in, audio out) or an effect (audio in, audio out). Read `kit/dsp/include/o3sdk/dsp/unit.h` and
`units/voice/` in your repository (about 230 lines with comments); `units/synth` is the same shape with eight voices and a filter, `units/delay` an effect
with big buffers.

1. **Create** `units/<name>/{CMakeLists.txt, include/<app-id>/units/<name>/<name>.h, src/<name>.cpp}`. `CMakeLists.txt` is one line,
   `o3sdk_unit(<name> SOURCES src/<name>.cpp)`; the library is `unit_<name>`. `units/CMakeLists.txt` picks up every directory that has one.
   List the library in the app's `DEPENDS` (`app/CMakeLists.txt`) and in the tests' (`tests/CMakeLists.txt`). A unit depends on `dsp` only:
   `o3sdk_unit` links it, and the kit's other layers do not resolve from a unit.
2. **The parameter table** is a `constexpr core::ParamDesc[]`: name (upper case, at most 12 characters), unit text, range, default, curve
   (`Linear`, `Log`, `Exp`, `Table`), display (`Int`, `NormPct`, `Enum`, `Fixed1`, `Hz`, `Seconds`), encoder `detents` over the range and the
   `coarse` multiplier (Shift). **The parameter id is the index in the table: append, never insert** (a saved value or a binding finds
   its parameter by id). `core::param_desc_valid()` checks a row; the template's test calls it on every row.
3. **`prepare(sample_rate)`** runs in the main loop before the engine starts: reset every piece of state and take the defaults of the table.
   At `Engine::start()` the unit then receives every stored value as an event, so both sides agree.
4. **`process(args)`** runs in the render context, once per block of at most 64 frames (261,224 cycles at 44.1 kHz and 180 MHz, shared by
   everything). Use `dsp::for_each_run` to render up to each event, apply it and go on. Write all `frames` samples of `out_l` and `out_r`. An
   effect may run in place.
5. **The rules in `process()`**: no heap, no blocking, no file or SD access, no `printf`, bounded loops, **no `double`** (the build treats
   unsuffixed constants as `float`; the cross build is checked for `__aeabi_d*`). No `libm` in the sample loop if you can avoid it (`sinf`,
   `powf` and `tanf` are software on the M4F): coefficients per block or per event, `core/fast_math.h`, a table.
6. **Smoothing is yours.** The engine delivers a changed value as one `Param` event, a step; it never ramps. A value that clicks when it
   jumps (a level, a cutoff) is ramped inside the unit (`dsp/smooth.h` `Ramp`; the template's Level does it, and its test proves it).
   `core::kParamSmooth` in a descriptor is advisory and no kit code reads it: set, it tells a UI or host that this unit smooths the
   parameter itself, so it may be turned fast or automated without a click. Set it only when the unit does (`docs/FRAMEWORK.md`, "Units").
7. **Big buffers** (a delay line, a sample) belong to the app, in the SDRAM: `O3SDK_BIG float g_line[N];` at namespace scope
   (`kit/core/include/o3sdk/core/placement.h`) and handed to the unit's constructor, as `apps/hello/src/hello_app.cpp` does for DELAY. They
   start at zero; the cross compiler refuses a non-zero initialiser on them. SRAM is 256 KiB (SRAM_U, 192 KiB, holds data and stacks and keeps
   32 KiB free by a linker assertion; SRAM_L, 64 KiB, holds the hot code); the SDRAM is 16 MiB.
8. **Alignment is declared**: a buffer read with word or multi-word loads gets `alignas(...)` and a `static_assert`.
9. **State the cost** in the header as an estimate (cycles a sample, percent of a block) and measure it on the unit later: the render's
   CPU % is in the header strip and in `o3console.py cpu` (skill `rt-optimize`). A host test is not that measurement.
10. **Porting open-source DSP** (Plaits, Open303, DaisySP, Faust output): skill `dsp-port` (licence gate, float-only, oracle test,
    `THIRD_PARTY.md` row); o3-303's `units/tb303` is a worked port (`units/tb303/tests/tb303_oracle_test.cpp`).

Route it in `setup()`: an instrument is a slot's source (`set_source(slot, id)`, 8 slots), an effect an insert of a slot (`set_insert`, 2 per
slot), a send effect (`set_send_effect(bus, id)`, buses A and B; the slot's `MixField::SendA/SendB` is how much goes to it) or a master
insert (`set_master_insert`). Up to 16 units and 4 clocked hooks. HELLO routes a delay to send A: `apps/hello/src/hello_app.cpp`, `setup()`.
Each of these returns `false` (`attach`: `kNoUnit`) when the engine refuses the call, which the template ignores with `(void)`: the refusal is
recorded as well (`Engine::setup_errors()`, `setup_error(i)`), and `sim::Harness::start()` and `<id>_sim` fail on it and print which call,
so a wrong slot or an effect used as a source is found by the first test run.

## 4. Add a screen

A screen is a class (`app::Screen`, `kit/app/include/o3sdk/app/screen.h`); see `app/src/play_screen.cpp`.

- `title()` is shown in the header strip. `input(ctx, in)` gets every press, release and encoder turn that the app did not take. `update(ctx)` runs
  every main-loop pass. `draw(ctx, renderer, area)` draws into the content area (800 x 426 between the header and the soft keys).
  `soft_keys(ctx, keys)` gives the labels of F1..F8 (a hot key is inverted, a dim one is greyed). `leds(ctx, leds)` writes a whole frame of
  48 pad levels. **All of it runs in the main loop**: never touch DSP state directly; talk to the sound through `ctx.engine()` (notes, parameters,
  transport) and read the render's state through `engine().stats()` or the app's own snapshots.
- Switch screens with `ctx.show(screen)` (from the next pass: `leave()` of the old, `enter()` of the new). Screens are plain members of the app.
- **Drawing** is immediate mode: `r.panel(rect, Color)`, `r.frame`, `r.rule`, `r.text(x, y, "TEXT", Color, Face)`, tiles, icons
  (`kit/ui/include/o3sdk/ui/renderer.h`), colours are tokens of the theme (`style.h`: `Color::Text`, `Accent`, `Play`, ...), faces are roles
  (`Display`, `Data`, `Label`, `Micro`). Components: `ui/components.h` (strip, badge, soft keys, bar, meter), `app/widgets.h` (`knob_strip`,
  `level_meter`, `step_row`), plots (waveform, spectrum, scope, meters, curves) in `kit/ui/plot/include/o3sdk/ui/plot/`. Flat fills, 1 px borders, no gradients.
- **The display list holds 2048 words** and the frame is refused when it overflows (the previous frame stays). A glyph or a tile is one word,
  plus one per colour change: group draws by colour. The simulator prints the peak (`peak list n/2048`) and `app_test` asserts it. HELLO peaks under
  900; o3-303's busiest screens have a plan for it (`../o3-303/docs/M3B-PLAN.md`).
- The header strip (app name, screen title, tempo, transport, the render's load on the unit) and the soft-key row are the kit's and are drawn
  for you. An app with no sequencer still shows `120.0 BPM` and a transport badge: today they are not optional.
- Look at the result: `<id>_sim --scenario ... --out <dir>`, then open the PNGs. A screen change without a scenario is not finished.

## 5. Controls and pads

The panel (`kit/app/include/o3sdk/app/panel.h`): 33 buttons (no LED), 48 pads in 4 rows of 12 with one monochrome LED each (no velocity,
no pressure), one encoder (no push). Pads are numbered 0..47 from the top left, row by row (`pad_at(row, col)`). Button names are the printed legends
(`Button::Play`, `Params`, `Shift`, `Left`, ...): an app gives any of them any meaning and says it on the screen.

- `app::Input` is raw: a press arrives at the press, so pads and keys play at once. `in.press(Button::Play)`, `in.pad_press()` / `in.pad()`,
  `in.turn()` / `in.delta()`, `in.shift`. What is held now is `ctx.panel()`. For holds and chords use `ctx.panel()` or `core/gesture.h`.
- **Global keys** (Play, the screen buttons) go in `App::input()`: it sees every input first, and `true` consumes it. A screen's own keys go in
  `Screen::input()`. **The Config button belongs to the kit** (the system Config screen is planned, `docs/FRAMEWORK.md` section 5): do not give it
  a meaning in a new app, so that nothing breaks when it lands. HELLO and o3-303 use it for a theme toggle today.
- **Editing values**: `app::ParamKeys` (`app/param_keys.h`) is the kit's convention: tap F1..F8 latches the encoder on that key's parameter, hold an F
  key and turn for a moment, hold two to move both, Shift for coarse steps, a page button for the next page of eight. A `ParamPage` is a name,
  a unit and up to eight parameter ids. `knob_strip()` draws the eight cells (name, value, bar) above the soft keys. It is what the template's
  screen does in about ten lines.
- **Pad LEDs**: `leds.set(pad, level)` with the shared levels `kLedDim` (something is here), `kLedLow` (a second kind), `kLedMid` (selected),
  `kLedOn` (now: the playhead, a held pad); `PadLeds::blink()` gives a phase. The runtime sends only what changed. Meaning comes from a few
  distinct levels and from blinking, because the LEDs are monochrome.
- **Keep the pad map in one pure header** (`app/include/<id>/pad_map.h`): the input, the LEDs and the drawing then agree, and a host test
  covers it (`tests/pad_map_test.cpp`; o3-303's `pad_map.h` is the larger example).

## 6. Parameters

Everything the user edits is a parameter of a unit: a row in its table. The engine's main side stores the values, formats them
(`engine.format_param`), steps them with the encoder (`step_param`) and sends each change to the unit as a `Param` event at frame 0 of the next
block. A screen changes one with `engine().set_param(unit, id, value)` (clamped through the descriptor), reads it with `engine().param()`.
Learn the descriptor fields from `kit/core/include/o3sdk/core/param.h`; copy a row of `units/synth/src/synth.cpp` and edit it.

App-level settings that are not a unit's (a swing, a pattern length) are the app's own state, shown on a screen and read by the hook. The
kit stores nothing on the card for an app yet (section 13).

## 7. A sequencer (a clocked hook)

A `dsp::Clocked` hook (`engine.add_hook(h)`, up to 4) runs in the render context before the units, once per block, and may `emit(unit, event)` at any
frame and write a slot's mixer (`set_mix`, ramped). `dsp::for_each_step(transport, period, offset, swing, fn)` walks a grid at 960 PPQN with no
drift; `Transport::started` and `stopped` mark the first block after Play and Stop (end your notes there). The tempo is `engine.set_tempo(mbpm)`
and Play/Stop `engine.play()` / `stop()`, from `App::input`. HELLO's `Riff` (about 60 lines, `apps/hello/src/hello_app.cpp`) is the smallest
sequencer; o3-303's `app/seq/` is a full one with its own host tests. The hook shares state with the main loop through atomics or a documented
single-writer protocol, stated at the declaration (`AGENTS.md`, "Rules").

## 8. A scenario and tests

- **Scenario** (`app/scenarios/*.txt`, commands in `sim/README.md`): `wait`, `press`, `release`, `tap`, `pad`, `paddown`, `padup`, `turn`, `snap`,
  `record`, `wav`. It is the evidence for a screen or a sound change: run it, open the PNGs, listen to or measure the WAV. `tests/app_test.cpp`
  runs every scenario in the directory and fails when a command is wrong or a frame does not fit, so a new scenario is a regression test for free.
- **Tests** are doctest (`tests/*_test.cpp`, globbed: rerun the configure after adding one; one executable, one ctest per file;
  `o3sdk_app_test_suite` in `tests/CMakeLists.txt`). Pure logic gets a test (maths, mappings, sequencer arithmetic); a unit is tested against the engine
  (`tests/voice_test.cpp`: attach, route, start, send notes and parameters, render blocks, look at the samples); the app is tested end to end
  with `sim::Harness` (`h.start()`, which is `false` when the app's `setup()` had a call the engine refused, with the reason on stderr;
  `run_ms`, `pad_down`, `press`, `turn`, `audio()`, `leds()`, `runtime().state_line()`). **`tests/CMakeLists.txt` is one call**,
  `o3sdk_app_test_suite(<id> SOURCES ${test_sources} DEPENDS <your units>)`: it builds the executable `<id>_tests`, links the kit, the
  simulator, the host platform and the app library **whole** (that library defines `o3sdk_app()`, which only the test names), and defines
  `APP_SCENARIO_DIR` as `app/scenarios` for the test that runs every scenario (`SCENARIO_DIR`, `SCENARIO_DEFINE` and `EXE` change those
  defaults). The older explicit form, `o3sdk_test_suite(<exe> SOURCES ... DEPENDS app sim platform_host
  "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<id>>")` plus a `target_compile_definitions`, still works (o3-303 uses it). **`o3sdk_app()` returns one
  object**, so every `Harness` in a test run drives the same app: `setup()` runs again, and whatever your screens and hook keep in members must be reset
  there (the template's `PlayScreen::init` does), or one test case leaks into the next.
- **Sanitizers** for buffer and DSP code, once: `cmake -S . -B build-host-asan -DO3SDK_SANITIZE=ON && cmake --build build-host-asan -j4 && ctest --test-dir build-host-asan`.
- A negative control is worth it: make sure the test fails when the code is wrong (the template's click test was checked with a one-sample ramp).

## 9. Build the image

```sh
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain dir>
cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure
```

Or the whole check in one command, from a fresh directory: `tools/ci.sh mk66` (configure, build, ctest, the size of every image, the software
doubles, and a count of warnings that must be 0; `tools/ci.sh host` is the host's; `tools/remote_ci.sh` runs either on the remote build machine,
`docs/ci.md`).

You get `build-mk66/app/<id>.elf`, `.hex`, `.bin`, `.map`, `.ptf` (the checked HEX under the name the bootloader takes) and the link's
region table (flash, SRAM_L, SRAM_U, SDRAM). The four ctests are static checks of the image: `<id>_image` (record checksums, every address at or below
`0x000FFFFF`, one contiguous run, the initial stack pointer, a Thumb reset vector, every vector in flash or SRAM_L), `<id>_flashconfig` (bytes
`0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff`: a wrong value can lock the chip for good), `<id>_ramfunc` (the SRAM_L hot code calls
nothing in flash), `<id>_usb` (the USB vector and the console in the image: an image without them cannot be installed or read over USB).

By hand, after a change that could matter (`tools/ci.sh mk66` does both):

```sh
arm-none-eabi-nm build-mk66/app/<id>.elf | grep ' __aeabi_d'          # must print nothing: no software double linked in
arm-none-eabi-size -A build-mk66/app/<id>.elf
```

The template's image is about 149 KB of flash (14 % of 1 MiB), 78 KB of SRAM_U (39 % of 192 KiB, the kit's console and audio buffers included)
and 64 KB of SDRAM. The image carries the build identity of both trees (`strings build-mk66/app/<id>.bin | grep o3sdk`): `<app git id> / o3sdk
<kit git id>`, `unknown` without a commit and `-dirty` with changed tracked files. **Commit before an image you will install**, so that it can be traced.

## 10. The gate

`fw-gate-check` (`.claude/skills/fw-gate-check/SKILL.md`) is the static release gate: the identity of both trees, two fresh builds with
byte-identical HEX, the image checks above, link sanity (no region overflow, no undefined reference, no `__aeabi_d*`), the full host
`ctest`, SHA-256 of the `.hex` and `.ptf`, and a manifest `CANDIDATE.md` with PASS/FAIL per check and the line "Not flashed. A unit test needs the
maintainer's go." It never flashes and never writes to a card. **One command does the mechanical part** (`tools/gate.py`, the same file in
every app repository; about 40 s for a small app):

```sh
cmake -S . -B build-host && cmake --build build-host -j4          # the host build of the same code, if there is none yet
python3 -I tools/gate.py <id> --host-build build-host              # builds build-gate-a and build-gate-b, writes build-gate-a/CANDIDATE.md
```

The verdict is `CANDIDATE` (everything passed, both trees clean and committed), `BENCH BUILD` (everything passed but a tree has changed tracked
files or no commit, so the image says `-dirty` or `unknown` and nobody can trace it: **commit first**, then run it again) or `FAIL` (exit 1,
the manifest says which check). The manifest also prints the label the unit's `version` will show, which is what `--expect-version` wants. What
stays with a person or an agent: a second look at the manifest against the raw logs (`build-gate-*/gate-*.log`), and the maintainer's go.

## 11. Install over USB, and the way back

Only with the maintainer's go, only an image that passed the gate, and only by this path (or by hand on the card). The tools are in
`../o3-sdk/platform/mk66/tools/` (Python 3, standard library only; `platform/mk66/tools/README.md` is their manual); in the commands below
`O3=../o3-sdk/platform/mk66/tools`. The commands below, up to the way back, were run on a unit on 2026-10-25 with HELLO and o3-303 (`docs/hw/2026-10-25-usb.md`); `o3hil.py` scenarios
were not. Your user must be allowed to open the unit's serial port (the `dialout` group on most distributions).

**Before the first install** (once, by a person at the unit): the card holds a known-good image in `Firmware/` that the hidden reset button
flashes (track3r-os's `o3os_1.0.0.ptf`, or the official image): that is the way back if your image does not boot. Back up the card.

```sh
python3 -I $O3/o3console.py --list                                  # the unit: /dev/serial/by-id/*Track3r*
python3 -I $O3/o3console.py version "fw"                            # what runs now; the card, and first-ptf (the reset button's target)
python3 -I $O3/o3console.py --put build-mk66/app/<id>.ptf --name <id>-0.1.ptf
#   -> ok <id>-0.1.ptf <bytes> <crc> image <length>   (check_image.py runs first and refuses a bad file)
python3 -I $O3/o3console.py --update <id>-0.1.ptf --crc <crc> --yes-update --expect-version "<app git id>"
#   -> the unit leaves, the bootloader flashes, it comes back on USB (about 15 s) and prints `version`
```

- `--put` of a name that is already on the card is refused (`ERR already exists`, exit 4): use a new `--name` per build (a version or the CRC in
  it; names are `[0-9A-Za-z][0-9A-Za-z._-]{0,47}.ptf`, and not starting with `Polyend`), or `--update` the file that is there with its CRC. The CRC of a
  file: `python3 -I -c 'import sys, zlib; print("%08X" % (zlib.crc32(open(sys.argv[1], "rb").read()) & 0xFFFFFFFF))' <file>`.
- After the `--put`, run `fw` again: `first-ptf` is the file the hidden reset button flashes (the card's directory order decides, and a put can move
  it to your file). A stranded unit is recovered by a person pressing that button, so for an image not yet proven to boot, a `first-ptf` that is your
  own file is no way back: `fw rm <name>` and have a person fix the card first. The session of 2026-10-25 accepted it for two images that
  had been gated, with the console as the way back.
- Exit status of `--put` and `--update`: 0 done, 1 usage, 2 no prompt, 3 wrong card, 4 the unit refused or failed, 5 it did not come back,
  6 `--expect-version` not in the reply, 7 the bootloader did not take over.

**Look at it** (what the unit can tell, with no hands on it):

```sh
python3 -I $O3/o3console.py version state cpu "log 40" usb leds      # your app's state pairs, the render's load, the log, USB audio, the LEDs
python3 -I $O3/o3console.py --shot /tmp/<id>.png                      # the real screen, 800 x 480
python3 -I $O3/o3console.py "key play tap" "pad 12 down" leds "pad 12 up" "enc 3" state   # inject input, read it back
python3 -I $O3/o3audio.py record --seconds 3 --channel 7 --not-silent # the master is on USB channels 7-8 (1-6 are silent, kept for stems)
```

Your own `state` pairs (`App::describe_state`) and commands (`App::diag_commands`, name 2-7 lowercase letters, one short reply) are how you
make the unit's state checkable from the computer: add the ones your engine needs. `python3 -I $O3/o3hil.py` runs JSON scenarios of these steps.

**The way back** (to track3r-os, or to the previous image): the image is already on the card, so only `--update` it, with its CRC:

```sh
python3 -I $O3/o3console.py --update o3os_1.0.0.ptf --crc <crc of that file> --yes-update --expect-version v1.0.0
```

About 45 s. Then `fw rm <id>-0.1.ptf` from track3r-os, if you want the card tidy (a file you did not put there is never deleted without the maintainer's ask).
The image has a console by default, so it can always be replaced this way. An image built with `-DO3SDK_MK66_CONSOLE=OFF` or the skeleton has none:
it strands the unit until someone uses the card and the reset button, so `<id>_usb` must pass before any remote install.

What the console does not do: it does not look at the screen with eyes, press the real keys, hear the real outputs or measure the real CPU
over hours. Those are the checks that stay with a person at the unit (`hw-session`).

## 12. Reference: the console hooks an app can add

`App::describe_state(ctx, out, cap)` writes `key=value` pairs (`StateWriter`: `num`, `sint`, `flag`, `str`; no space in a value) after the kit's
`app=<name> screen=<title> playing=<0|1> tempo=<mbpm> cpu=<pct> frames=<n>`; the template does it for the keyboard octave and the encoder's key.
`App::diag_commands(rows)` returns a constexpr table of `DiagCommand {name, help, run}`; the handler runs in the main loop, says well under 1,600
bytes and reaches the app through `o3sdk_app()`. Details: `kit/app/include/o3sdk/app/diag.h`.

## 13. What a new app cannot use yet

The capability map is `docs/FRAMEWORK.md` section 10 (done and verified on the unit, done on the host only, planned). The gaps that
matter for an instrument today:

| Missing | Means | Workaround |
|---|---|---|
| Saving and loading (the card from an app) | patterns, kits and settings do not survive power-off; `hal::storage` is read-only and the write side is the update path's | keep state in RAM; ship factory content in the image; plan the file format now |
| MIDI in/out | no notes or clock from outside | none; the engine accepts events from any source when the driver lands |
| The system Config screen and the on-screen firmware switcher | the Config button is free but belongs to the kit; switching engine is done over the console | install over USB (section 11) |
| Power switch shutdown | no clean shutdown (silent, dark, card released) | none |
| Audio in (line, microphone), FM radio | units receive `in_l`/`in_r` planes but input select, gain and recording are contract-only | none |
| Header and transport are fixed | an app with no sequencer shows a tempo and a transport badge | none |

## 14. When it goes wrong

| Symptom | Cause |
|---|---|
| `O3SDK_DIR='...' is not an o3-sdk checkout` | o3-sdk is not at `../o3-sdk`: `-DO3SDK_DIR=<path>` or `$O3SDK_DIR`; a cached value stays until you pass `-D` or use a fresh build directory |
| `cmake/O3sdkPlatform.cmake must be included before project()` | the app's `CMakeLists.txt` includes it after `project()`: keep the template's order |
| `undefined reference to o3sdk_app` in a test or an image | the app library is not linked whole: `$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<id>>` (`o3sdk_app()` does it for the simulator and the image; `o3sdk_app_test_suite()` for the tests; a hand-written `o3sdk_test_suite` does it itself) |
| the simulator exits 1 with `setup refused: ...`, or `REQUIRE(h->start())` fails | a call in `setup()` was refused (a slot or index out of range, a unit that was never attached or is the wrong kind, a full engine, a call after `start()`): fix the call; the line names it |
| a unit header "file not found" in the app | the unit is not in the app's `DEPENDS`, or the include path of its `include/` has another prefix than the one you spell |
| `only zero initializers are allowed in section` | an `O3SDK_BIG` object with an initialiser: it starts at zero, give it a value in a constructor |
| a link error "region ... overflowed" | a budget failure: SRAM_U keeps 32 KiB free by assertion; move big buffers to `O3SDK_BIG`; read `<id>.map` |
| `__aeabi_d...` references | a `double` somewhere (an unsuffixed constant in a library call, `std::pow(x, 2.0)`, `printf`): find it with `nm -u` on the archives |
| the screen is blank in the simulator | the frame did not fit (`refused` in the summary line, exit 1): fewer words |
| a new `*_test.cpp` is not run | the glob: rerun the cmake configure |
| every image link prints ``ld: <id>.elf: section `.bss' can't be allocated in segment 3`` and `LOAD: .data .bss` | seen at every image link here (hello, fillrate, the template's); hello's image, which ran on the unit, links the same way. It has no `warning:` in it, so `tools/ci.sh` does not count it. A platform follow-up (the program headers of `mk66.ld.in`), not yours |
| `--put` ends `ERR already exists` | section 11: a new `--name`, or `--update` the file already there |
| the unit does not come back after `--update` | wait 60 s; `--list`; blink code 14 means USB did not start (`platform/mk66/README.md`, "Faults and blink codes"); the way back is the card and the hidden reset button, by a person |
