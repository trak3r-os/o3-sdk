# @APP_NAME@

An instrument for the Tracker hardware (NXP MK66FX1M0, 800x480 display, 48 pads with LEDs, 33 buttons, one encoder), built as an app on
the [o3-sdk](https://github.com/trak3r-os/o3-sdk) framework. **Describe the instrument here**: what it sounds like, what the pads, the
buttons and the encoder do, what is saved, what is planned. An app is a whole firmware image; o3-sdk, checked out next to this repository,
is the library under it and is used as it is (never edited from here).

Unofficial; not affiliated with or endorsed by Polyend. "Tracker" is used only to describe the hardware it runs on.

## What is here

A starter from o3-sdk's `templates/app`: one unit (`VOICE`, a small monophonic instrument), one screen (`PLAY`: the pads are a chromatic
keyboard, F1-F3 and the encoder edit the voice), a scenario, and tests. Replace the voice with your engine and keep the shape.

| Path | What |
|---|---|
| `app/` | the app: `src/app.cpp` (units and screens wired into the engine, the entry point), `src/play_screen.cpp` (the screen), `include/@APP_ID@/pad_map.h` (pure pad logic) |
| `app/scenarios/` | simulator scenarios: screens as PNG, audio as WAV (commands: `../o3-sdk/sim/README.md`) |
| `units/voice/` | the DSP unit: a parameter table and `process()`; one directory per unit, each with a `CMakeLists.txt` calling `o3sdk_unit()` |
| `tests/` | host tests (doctest): the unit against the engine, the pad map, the app in the simulator |
| `tools/` | `ci.sh`, `remote_ci.sh` and `gate.py`, identical to o3-sdk's (`../o3-sdk/docs/ci.md`) |
| `.claude/` | the agent harness (agents, skills, hooks), copied from o3-sdk |

Start with `../o3-sdk/docs/NEW-APP.md` (the tutorial), then `CLAUDE.md` and `AGENTS.md` for the rules and the agents.

## Build

o3-sdk next to this directory (`export O3SDK_DIR=<path>` or `-DO3SDK_DIR=<path>` otherwise). The host build needs CMake 3.24 or newer and a
C++17 compiler; the image needs the Arm GNU Toolchain 15.3.rel1.

```sh
cmake -S . -B build-host
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure

# the app in the simulator: PNG snapshots of the screen and the pads, and the audio as a WAV
mkdir -p /tmp/@APP_ID@
build-host/app/@APP_ID@_sim --scenario app/scenarios/boot.txt --out /tmp/@APP_ID@
```

## Build the image

```sh
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir>
cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure      # @APP_ID@_image, _flashconfig, _ramfunc, _usb
```

`build-mk66/app/@APP_ID@.ptf` is the image the bootloader takes. Or the whole check from a fresh directory: `tools/ci.sh host` and
`tools/ci.sh mk66`. Before an image goes to a unit it passes the gate (`python3 -I tools/gate.py @APP_ID@ --host-build build-host`: two fresh builds,
identical HEX, the image checks, a manifest in `build-gate-a/CANDIDATE.md`; skill `fw-gate-check`, `../o3-sdk/docs/NEW-APP.md` section 10); it is
installed over USB with `o3console.py` (section 11) or by card, with the maintainer's go. Commit first: the image carries the git state of both trees.

## Licence

GPL-3.0-or-later (`LICENSE`): the image links the GPL-3.0-or-later kit statically.
