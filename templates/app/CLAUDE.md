# @APP_NAME@: an app on the o3-sdk framework

An instrument for the Tracker hardware (NXP MK66FX1M0, Cortex-M4F at 180 MHz with a single-precision FPU, 256 KiB SRAM, 16 MiB SDRAM,
FT812 display 800x480, SGTL5000 codec, 48 pads with monochrome LEDs, 33 buttons, one encoder). It is one app, a whole firmware image,
built on the o3-sdk framework. Licence GPL-3.0-or-later. Everything in this repository is written in English. Replace this paragraph
with the description of the instrument (its design document goes in `docs/`).

## The framework is ../o3-sdk

The kit (core, hal, eve, ui, dsp, app), the units it ships (synth, delay), the MK66 platform, the simulator and the CMake helpers
(`o3sdk_app`, `o3sdk_unit`, `o3sdk_test_suite`) live in the sibling repository `../o3-sdk` (`-DO3SDK_DIR=<path>` or `$O3SDK_DIR` if it is
elsewhere). **It is never edited from here**, not even to get a build through: a change the app needs is made in o3-sdk, under its own rules
(`../o3-sdk/CLAUDE.md`, `../o3-sdk/AGENTS.md`), and this repository then builds against it. The tutorial is `../o3-sdk/docs/NEW-APP.md`; the
reference is `../o3-sdk/docs/FRAMEWORK.md` (read it before writing a unit or a screen).

What lives here: `app/` (the app, `o3sdk_app(@APP_ID@ ...)`, its scenarios in `app/scenarios/`), `units/` (this project's own units, each with its own
`CMakeLists.txt` calling `o3sdk_unit()`), `tests/` (doctest, `o3sdk_test_suite()`), `docs/`.

## Hard rules

- Never add code, assets or reverse-engineering material from the official firmware. "Polyend" and "Tracker" are used only to
  describe the hardware.
- Firmware reaches a unit only through a documented update path, after `fw-gate-check` passes: the bootloader's `.ptf` from the
  SD card, put there by hand, on a mounted card, or by the USB console (`o3console.py --put`, then the console's firmware
  commands), as in track3r-os (maintainer, 2026-10-25). Agents may write to an SD card mounted under `/media` or `/run/media`
  (never `/mnt`, never a raw device). No debug-probe flashing, no improvised uploaders. Formatting a card, or deleting files you
  did not put there, needs the maintainer's ask.
- No secrets, personal data or binaries in git. Commit or push only when the maintainer asks.

## Build and test

```sh
cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
mkdir -p <dir> && build-host/app/@APP_ID@_sim --scenario app/scenarios/boot.txt --out <dir>     # screens as PNG, audio as WAV
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir> && cmake --build build-mk66 -j4
```

The image is `build-mk66/app/@APP_ID@.elf`, `.hex`, `.bin` and `.ptf` (the `.ptf` is what the bootloader takes from the card); its ctests
(`@APP_ID@_image`, `@APP_ID@_flashconfig`, `@APP_ID@_ramfunc`, `@APP_ID@_usb`) check it statically. Run builds under `nice -n 10`. A host test is not
hardware verification: name the remaining unit check.

Release gate: `python3 -I tools/gate.py @APP_ID@ --host-build build-host` (two fresh builds, identical HEX, the image checks, a manifest; commit first).
CI: `tools/ci.sh <host|mk66>` runs the whole check from a fresh directory; `tools/remote_ci.sh <host|mk66>` runs it on the remote build
machine against this tree and `../o3-sdk` as they are; exit 3 or 255 means run `tools/ci.sh` locally (`../o3-sdk/docs/ci.md`).

## Engineering standards

The standards of o3-sdk apply unchanged (`../o3-sdk/CLAUDE.md`, `../o3-sdk/AGENTS.md`): layering by link dependency (a unit depends on `dsp` only,
an app on `app` and its units), no allocation or blocking in the real-time paths, no heap, no RTTI, no exceptions, no `double` (check the
cross build for `__aeabi_d*` with `arm-none-eabi-nm -u`), big buffers in the SDRAM with `O3SDK_BIG`, a unit's parameter id is its index in
the table (append, never insert), warning-free under `-Werror`, the CPU measured on the unit before a per-block feature is relied on, pure
logic gets a host test in `tests/`, a screen or a sound change gets a simulator scenario in `app/scenarios/`, the Config button is the kit's.

## Agents, skills and hooks

`.claude/` is the o3-sdk harness, copied (agents, skills, hooks); read its texts with this layout in mind: the kit, platform and simulator
paths they name are in `../o3-sdk`, this repository's code is `app/`, `units/` and `tests/`. `AGENTS.md` says which agent or skill to use when.
The hooks work here as they do there (`guard_flash.py`, `syntax_check.py` needs a `build-host` configured first, `shell_edit_check.py`,
`session_brief.py`, `verify_gate.py`).
