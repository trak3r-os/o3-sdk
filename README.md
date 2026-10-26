# o3-sdk

A framework for instruments and grooveboxes on the Tracker hardware. **An app built with it is a whole firmware image**: it owns the screen,
the pads, the buttons, the CPU and the memory. The kit is the library underneath: the machine bring-up, the UI toolkit, the audio engine, the
input conventions, a simulator, the build, the tests, the image, the release gate and the install over USB. A new instrument needs only its own
**engine**: its DSP units, its screens, its controls.

- **You write**: units (a parameter table and `process()`), screens (what 48 pads, 33 buttons, one encoder and an 800×480 display mean), a
  sequencer if there is one, and their tests.
- **You get**: clocks, SDRAM, display, input, LEDs, audio, USB console and USB audio, the engine (slots, inserts, sends, master, transport,
  clocked hooks), the header strip, the soft keys, the controls convention, widgets, a headless simulator, `o3sdk_app()` (one CMake call makes the
  simulator and the `.ptf` image with its checks), the CI scripts, the gate, and `o3console.py` to install and inspect an image over USB.

The first app is **[o3-303](https://github.com/trak3r-os/o3-303)**, an improved TB-303, in its own repository next to this one: the worked example of
an app that uses o3-sdk as it is. `docs/NEW-APP.md` takes you from an empty directory to your own image on the unit.

Unofficial; not affiliated with or endorsed by Polyend. "Tracker" is used only to describe the hardware it runs on.

## The hardware

| | |
|---|---|
| CPU | NXP MK66FX1M0, Cortex-M4F at 180 MHz, single-precision FPU only (`double` is emulated in software: never use it) |
| Memory | 1 MiB flash, 256 KiB SRAM (192 KiB SRAM_U + 64 KiB SRAM_L), 16 MiB SDRAM |
| Display | FT812, 800×480, no frame buffer: a display list of at most 2048 words per frame |
| Audio | SGTL5000 codec, I2S + eDMA, 44.1 kHz, 64-frame blocks (261,224 cycles each); line in and microphone on the codec; USB audio out |
| Controls | 48 pads in 4 rows of 12, one monochrome LED each, no velocity; 33 buttons without LEDs; one encoder without a push |
| Other | SD card (FAT), USB device (console and audio on the same port), DIN MIDI (not yet driven), FM receiver (not yet driven) |

## What is verified on the unit

On 2026-10-25 the test session installed HELLO and o3-303 on a unit **over USB** with `o3console.py` and read them back (`docs/hw/2026-10-25-usb.md`):
both boot and run with no blink code; `version`, `state` (the app's own pairs), `cpu`, `log`, `usb` and `leds` answer; `--shot` returns a clean
800×480 screen; injected `key`, `pad` and `enc` drive the app and the pad LEDs follow; the master was recorded over USB audio (HELLO a pad at 523 Hz,
o3-303 playing, channels 7-8, 1-6 silent); 0 underruns and 0 faults over a 10-minute soak; the render took 19.6 % of a block at idle with HELLO
and at most 39.8 % with o3-303 playing; the way back to track3r-os v1.0.0 took about 45 s.

**Not verified yet**: anything that needs eyes, hands or ears (the screen itself, real presses of pads and buttons, the LEDs by eye, sound on the analogue
outputs), the FT812's per-line fill rate, a provoked crash and its record, and everything planned (the system Config screen and the on-screen
firmware switcher, MIDI, a file API for apps, the power switch, audio in beyond the engine's input planes, the FM radio). The per-capability table
is `docs/FRAMEWORK.md` section 10: each row says verified on the unit, host only or planned.

## Quick start (10 minutes)

You need Linux, CMake 3.24 or newer, a C++17 compiler, Python 3, and for the image the Arm GNU Toolchain 15.3.rel1 (`arm-none-eabi`, from Arm's
downloads page). Install on a unit also needs the unit (running track3r-os v1.0.0 or an o3-sdk image) and a USB cable.

```sh
# 1. clone
git clone https://github.com/trak3r-os/o3-sdk.git
cd o3-sdk

# 2. build for the host and run the tests (about 110 seconds with 4 jobs on a recent machine: 70 s of build, 40 s of tests)
cmake -S . -B build-host
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure

# 3. run the HELLO app in the simulator: PNG snapshots of the screen and the pads, the audio as a WAV
mkdir -p /tmp/hello
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello

# 4. build the firmware image (HELLO's: build-mk66/apps/hello/hello.ptf) and run its static checks
export ARM_GNU_ROOT=<the arm-gnu-toolchain-15.3.rel1 directory that holds bin/>
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66
cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure

# 5. install it over USB (only with the maintainer's go and a known-good image on the card: docs/NEW-APP.md section 11)
O3=platform/mk66/tools
python3 -I $O3/o3console.py --put build-mk66/apps/hello/hello.ptf --name hello-0.1.ptf       # prints the CRC
python3 -I $O3/o3console.py --update hello-0.1.ptf --crc <that crc> --yes-update
python3 -I $O3/o3console.py version state cpu --shot /tmp/hello-unit.png                      # read it back
```

Then **your own instrument**, in its own repository next to this one:

```sh
python3 -I tools/new_app.py ../myapp myapp --name MYAPP --git     # CMake, one app, one screen, one unit, tests, a scenario, agent docs
cd ../myapp && cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
```

Every step has a command: `tools/ci.sh host` and `tools/ci.sh mk66` check a tree from a fresh directory (warnings, tests, images, sizes, no
software doubles), `tools/remote_ci.sh` does it on a build machine, `python3 -I tools/gate.py <app> --host-build build-host` is the release gate
(two fresh builds, identical HEX, the image checks, a manifest; skill `fw-gate-check`), and `platform/mk66/tools/o3console.py` installs and
inspects. `docs/NEW-APP.md` explains each one.

## Documentation

| Read | For |
|---|---|
| `docs/NEW-APP.md` | **the tutorial**: a new instrument from zero to an image on the unit (unit, screen, controls, parameters, scenario, test, image, gate, install, the way back) |
| `docs/FRAMEWORK.md` | the reference: layers, units, the engine, apps and the controls convention, the runtime, the simulator, **the capability map (what is verified, host only, planned)**, using o3-sdk from another repository |
| `AGENTS.md`, `CLAUDE.md` | for coding agents: how to build on the kit, the rules, the install rules and guard hooks, the agents and skills in `.claude/`, how to verify |
| `platform/mk66/README.md`, `platform/mk66/tools/README.md` | the unit side: boot, memory map, the image and its checks, blink codes; the USB tools and static checks |
| `sim/README.md` | the simulator and its scenario language |
| `docs/ci.md` | `tools/ci.sh` and the remote CI |
| `docs/hw/` | what the unit has shown, dated (`2026-10-25-usb.md`: installed and run over USB; `2026-10-25.md`: the card session, prepared) |
| `docs/M2-PLAN.md`, `docs/M2B-PLAN.md` | the plans and decisions of the platform milestones (history) |
| `templates/app/` | the starter for a new app (instantiated by `tools/new_app.py`) |
| `../o3-303/docs/DESIGN-303.md` | a complete app design, as an example of what to write before the code |

## Status

| Milestone | What | State |
|---|---|---|
| M1 | The kit (core, hal, eve, ui, dsp, app), the simulator, two example units, the HELLO app | done, host-tested; the libraries cross-build for the Cortex-M4F |
| M2 | The MK66 platform: boot, display, input, LEDs, audio; a bootable `.ptf` image of an app | image built and statically gated; HELLO and o3-303 run on the unit over USB (2026-10-25, `docs/hw/2026-10-25-usb.md`); the eyes-and-hands card session is still pending (`docs/hw/2026-10-25.md`), so not closed |
| M2b | USB console and install over USB, USB audio out, SD card (M2b-R); the on-screen firmware switcher (Config, FIRMWARE), MIDI (DIN and USB), Power switch | M2b-R built and verified on the unit over USB; the rest planned (`docs/FRAMEWORK.md` section 10) |
| M2c | Audio in (line, microphone), FM radio, recording into SDRAM | planned |
| M3 | o3-303 on the kit | in progress; its own repository `../o3-303` (`../o3-303/docs/DESIGN-303.md`) |
| M4 | The HOOKS device (sample launcher, synced sampling) | planned |
| M5 | A port (Plaits is the obvious candidate) to prove the unit API | planned |
| SDK | The app template and `tools/new_app.py`, the tutorial, agent docs | done on the host: the template is built and tested by `ctest -L template` (host and mk66); not tried by anyone but its author |

## Layout

`kit/` the framework (`core` `hal` `eve` `ui` `dsp` `app` `usb`) · `units/` the kit's example units (`synth`, `delay`) · `apps/` the kit's example apps
(`hello`, `fillrate`; a new instrument lives in its own repository) · `platform/` the HAL implementations (`host` for the simulator, `mk66` for the
unit) · `sim/` the simulator · `templates/app/` the starter for a new app · `tests/` host tests · `docs/` · `tools/` (`ci.sh`, `remote_ci.sh`,
`gate.py`, `new_app.py`, the font, icon and logo converters) · `cmake/` (`o3sdk_app`, `o3sdk_unit`, `o3sdk_test_suite`, `o3sdk_app_test_suite`, the toolchain) · `.claude/` the agent
harness · `third_party/` (see `THIRD_PARTY.md`).

## Licence and origin

GPL-3.0-or-later (`LICENSE`). An image links the kit statically, so an app built on it is GPL-compatible. The layers `kit/core`, `kit/hal`,
`kit/eve`, `kit/ui`, `kit/usb`, the MK66 drivers, the simulator's display interpreter, the host fakes and the USB tools come from track3r-os, by the
same author, renamed from `o3os` to `o3sdk`; comments that cite `OS-…` or `ADR …` refer to that repository's documents.
