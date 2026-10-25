# o3-sdk

A framework for instruments and grooveboxes on the Tracker hardware: NXP MK66FX1M0 (Cortex-M4F, 180 MHz, single-precision FPU),
256 KiB SRAM, 16 MiB SDRAM, FT812 display (800×480), SGTL5000 codec, 48 pads with LEDs, 33 buttons, one encoder.

An app built with o3sdk is a **whole firmware image**: it owns the screen, the pads, the buttons, the CPU and the memory. The kit is a
library underneath it, not a host around it: drivers, the audio engine, the UI toolkit, the input conventions and a simulator, already
solved. The first app is **[o3-303](https://github.com/trak3r-os/o3-303)**, an improved TB-303, in its own repository (checked out next to
this one) that uses o3sdk as it is; it is also the example of how an app repository builds on the kit.

Unofficial; not affiliated with or endorsed by Polyend. "Tracker" is used only to describe the hardware it runs on.

## Status

| Milestone | What | State |
|---|---|---|
| M1 | The kit (core, hal, eve, ui, dsp, app), the simulator, two example units, the HELLO app | done, host-tested; the libraries cross-build for the Cortex-M4F |
| M2 | The MK66 platform: boot, display, input, LEDs, audio; a bootable `.ptf` image of an app | image built and statically gated; waiting for the unit test (`docs/M2-PLAN.md`) |
| M2b | USB console and firmware update, the firmware switcher (Config, FIRMWARE: change engine by flashing another `.ptf` from the card), USB audio out, SD card, MIDI (DIN and USB), Power switch | planned (`docs/FRAMEWORK.md` section 10) |
| M2c | Audio in (line, microphone), FM radio, recording into SDRAM | planned |
| M3 | o3-303 on the kit | in progress; its own repository `../o3-303` (`../o3-303/docs/DESIGN-303.md`) |
| M4 | The HOOKS device (sample launcher, synced sampling) | planned |
| M5 | A port (Plaits is the obvious candidate) to prove the unit API | planned |

Nothing here has run on a Tracker yet.

## Build

CMake 3.24 or newer and a C++17 compiler for the host; the ARM GNU toolchain 15.3.rel1 for the image.

```sh
cmake -S . -B build-host
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure

# the HELLO app in the simulator: PNG snapshots of the screen and the pads, and the audio as a WAV
mkdir -p /tmp/hello
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello

# the cross build (the kit, units and apps as Cortex-M4F libraries, and each app's firmware image)
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir>
cmake --build build-mk66 -j4
```

An app of its own repository uses o3sdk through `add_subdirectory` (`docs/FRAMEWORK.md`, "Using o3sdk from another project").

## Layout

`kit/` the framework (`core` `hal` `eve` `ui` `dsp` `app`) · `units/` DSP units · `apps/` apps (one firmware image each) ·
`platform/` the HAL implementations (`host` today, `mk66` in M2) · `sim/` the simulator · `tests/` host tests · `docs/` the
framework guide and the designs · `tools/` the font, icon and logo converters · `third_party/` (see `THIRD_PARTY.md`).

Start with `docs/FRAMEWORK.md`.

## Licence and origin

GPL-3.0-or-later (`LICENSE`). The layers `kit/core`, `kit/hal`, `kit/eve`, `kit/ui`, the simulator's display interpreter and the
host fakes come from track3r-os, by the same author, renamed from `o3os` to `o3sdk`; comments that cite `OS-…` or `ADR …` refer to
that repository's documents.
