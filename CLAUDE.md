# o3-sdk — a framework for instruments on the Tracker hardware

C++17, bare metal, for the NXP MK66FX1M0 (Cortex-M4F, 180 MHz, single-precision FPU), 256 KiB SRAM, 16 MiB SDRAM, FT812 display
(800×480), SGTL5000 codec, 48 pads with monochrome LEDs, 33 buttons, one encoder. An app is a whole firmware image; the kit is the
library under it. Licence GPL-3.0-or-later. Everything in this repository is written in English.

**Where to start**: `README.md` (what is here, what is verified on the unit, the quick start), `docs/NEW-APP.md` (a new instrument, zero to an
image on the unit), `docs/FRAMEWORK.md` (the reference; section 10 is the capability map: verified on the unit, host only, planned),
`AGENTS.md` (the working guide: how to build an app, the rules, the install rules, the agents and skills, how to verify). A new app lives in its
**own repository** started with `python3 -I tools/new_app.py <dir> <app-id>`; the first one is `../o3-303`, the worked example.

## Hard rules

- Never add code, assets or reverse-engineering material from the official firmware. "Polyend" and "Tracker" are used only to
  describe the hardware.
- Firmware reaches a unit only through a documented update path, after `fw-gate-check` passes: the bootloader's `.ptf` from the
  SD card, put there by hand, on a mounted card, or by the USB console (`o3console.py --put`, then the console's firmware
  commands), as in track3r-os (maintainer, 2026-10-25). Agents may write to an SD card mounted under `/media` or `/run/media`
  (never `/mnt`, never a raw device). No debug-probe flashing, no improvised uploaders. Formatting a card, or deleting files you
  did not put there, needs the maintainer's ask.
- No secrets, personal data or binaries in git. Commit or push only when the maintainer asks.
- Never edit `../track3r-os`. Code ported from it (`kit/core`, `hal`, `eve`, `ui`, `usb`, `platform/host`, the MK66 drivers, the USB tools) stays identical
  to it apart from the rename and the documented cuts: report a defect in it to the maintainer, do not fix or work around it here.

## Build and test

```sh
cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out <dir>     # screens as PNG, audio as WAV (the dir must exist)
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir> && cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure
```

This is the top-level build: it also builds o3sdk's own apps (HELLO, fillrate), tests and the app template's tests (`ctest -L template`:
`tools/new_app.py` instantiated and built, host and mk66). A project that uses o3sdk from outside (`add_subdirectory`) gets the kit, the units and
the platform only. Run builds under `nice -n 10`, in your own build directory (`build-<task>-host`, `-mk66`). A host test is not hardware
verification: name the remaining unit check.

CI: `tools/ci.sh <host|mk66>` runs the whole check from a fresh directory; `tools/remote_ci.sh <host|mk66>` runs it on the remote build machine;
exit 3 or 255 means run `tools/ci.sh` locally (`docs/ci.md`).

## Engineering standards

- Layering by link dependency: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`, `core <- formats <- app`, `core <- usb` (linked by the MK66 platform, never by an
  app); units depend on `dsp` only; an app on `app` and its units.
- Real-time paths (`Unit::process`, `Clocked::block`, `Engine::render`, ISRs): no allocation, no SD I/O, no blocking, bounded loops.
- No heap, no RTTI, no exceptions, no `double` in the kit, units or apps (`-fsingle-precision-constant`; check the cross build for
  `__aeabi_d*` with `arm-none-eabi-nm -u`). Big buffers go in the SDRAM with `O3SDK_BIG` (`kit/core/include/o3sdk/core/placement.h`).
- A unit's parameter id is its index in the table: append, never insert. Hardware register values copied from track3r-os are proven: keep them exactly.
- Warning-free with `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Werror`. Match the surrounding style.
- The CPU is the binding constraint: measure per-block features on the unit before relying on them.
- Pure logic (DSP maths, sequencer arithmetic, formats) gets host tests; screens and sounds get a simulator scenario, run and looked at.

## Agents, skills and hooks

`.claude/` is the Claude Code harness (agents `firmware-engineer`, `firmware-reviewer`, `build-verifier`, `doc-keeper`, `dsp-scout`; skills `app-feature`,
`fw-build`, `fw-gate-check`, `dsp-port`, `rt-optimize`, `hw-session`, `decide`, `handoff`; hooks `guard_flash.py`, `syntax_check.py`,
`shell_edit_check.py`, `session_brief.py`, `verify_gate.py`). `AGENTS.md` section 8 says when to use each and what the hooks enforce. The
orchestrator decides, briefs and reads verdicts; an engineer implements one scoped change; a reviewer reads it independently; the verifier
gives the numbers. Other agents may share the tree: stay in your files, build in your own directory.
