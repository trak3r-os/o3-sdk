# o3-sdk — a framework for instruments on the Tracker hardware

C++17, bare metal, for the NXP MK66FX1M0 (Cortex-M4F, 180 MHz, single-precision FPU), 256 KiB SRAM, 16 MiB SDRAM, FT812 display
(800×480), SGTL5000 codec, 48 pads with monochrome LEDs, 33 buttons, one encoder. An app is a whole firmware image; the kit is the
library under it. Licence GPL-3.0-or-later. Read `docs/FRAMEWORK.md` first. The first real app, o3-303 (the TB-303), is its own repository, the sibling `../o3-303`, which
uses this one as it is (`docs/FRAMEWORK.md`, "Using o3sdk from another project"); its design is `../o3-303/docs/DESIGN-303.md`.
Everything in this repository is written in English.

## Hard rules

- Never add code, assets or reverse-engineering material from the official firmware. "Polyend" and "Tracker" are used only to
  describe the hardware.
- Firmware reaches a unit only through the documented update path (the bootloader's `.ptf` from the SD card). No debug-probe
  flashing, no improvised uploaders. Formatting a card, or deleting files you did not put there, needs the maintainer's ask.
- No secrets, personal data or binaries in git. Commit or push only when the maintainer asks.

## Build and test

```sh
cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out <dir>     # screens as PNG, audio as WAV
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir> && cmake --build build-mk66 -j4
```

This is the top-level build: it also builds o3sdk's own apps (HELLO) and tests (`O3SDK_BUILD_APPS`, `O3SDK_BUILD_TESTS`). A project that
uses o3sdk from outside (`add_subdirectory`) gets the kit, the units and the platform only.

Run builds under `nice -n 10`. A host test is not hardware verification: name the remaining unit check.

## Engineering standards

- Layering by link dependency: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`; units depend on `dsp` only.
- Real-time paths (`Unit::process`, `Clocked::block`, `Engine::render`, ISRs): no allocation, no SD I/O, no blocking, bounded loops.
- No heap, no RTTI, no exceptions, no `double` in the kit, units or apps (`-fsingle-precision-constant`; check the cross build for
  `__aeabi_d*` with `arm-none-eabi-nm -u`). Big buffers go in the SDRAM with `O3SDK_BIG` (`kit/core/include/o3sdk/core/placement.h`).
- A unit's parameter id is its index in the table: append, never insert.
- Warning-free with `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Werror`. Match the surrounding style.
- The CPU is the binding constraint: measure per-block features on the unit before relying on them.
- Pure logic (DSP maths, sequencer arithmetic, formats) gets host tests; screens get a simulator scenario.

## Origin

`kit/core`, `kit/hal`, `kit/eve`, `kit/ui`, `sim/eve_raster`, `sim/raster_display` and `platform/host` come from track3r-os (same
author), renamed `o3os` -> `o3sdk`. Comments citing `OS-…` or `ADR …` refer to track3r-os documents. The MK66 platform (milestone M2)
will come from track3r-os `platform/mk66`; see `docs/FRAMEWORK.md` section 9 for the cut points.

## Agents, skills and hooks

`.claude/` holds the Claude Code harness (adapted from track3r-os). **Orchestrator pattern:** the orchestrator decides, briefs and reads
verdicts; `firmware-engineer` implements one scoped change; `firmware-reviewer` reviews it read-only, in parallel with `build-verifier`,
which gives the numbers (build, ctest, simulator, sizes, image checks); `doc-keeper` updates the status docs from facts. The orchestrator
does not read diffs or logs itself. Other agents may share the tree (the M2 slices): stay in your files, build in your own directory.

| Agent | Use it for |
|---|---|
| `firmware-engineer` (sonnet) | any edit to kit, units, apps, platform or CMake; reports numbers, does not commit |
| `firmware-reviewer` (sonnet, read-only) | one review per Level A/B change, a risk review plus one for Level C |
| `build-verifier` (haiku) | host build and ctest, simulator runs, cross build sizes, the static image gate |
| `doc-keeper` (haiku) | README status table, `docs/M2-PLAN.md`, `docs/FRAMEWORK.md`, `THIRD_PARTY.md`, from given facts |
| `dsp-scout` (sonnet) | licence and cost research for open-source DSP to port as a unit (M3 voice, M5 port) |

| Skill | When |
|---|---|
| `app-feature` | the orchestrator's pipeline for any feature, fix or refactor, with risk levels A/B/C |
| `fw-build` | build, test, sizes, the `__aeabi_d*` check |
| `fw-gate-check` | before an app's `.ptf` goes to the maintainer: reproducible build, image checks, manifest; never flashes |
| `dsp-port` | licence gate, oracle test and rules for porting DSP into an `o3sdk_unit` |
| `rt-optimize` | structure of `process()`, memory placement, how to measure; no cycle claim without the unit |
| `hw-session` | one batched session with the unit: checklist, capture, ingest |
| `decide` | cross-cutting decisions (ADR) and app decisions (the design doc's open items) |
| `handoff` | closing a session so the next one resumes cold |

Hooks (`.claude/hooks/`, Python 3 standard library, wired in `.claude/settings.json`):
- `guard_flash.py` refuses uploaders, debug probes, raw block-device writes, partition tools and any write to removable media; the
  maintainer opens one card with `O3_ALLOW_CARD_WRITE=1` in `.claude/settings.local.json` (see the `.example`). Check: `--selftest`.
- `syntax_check.py` compiles each C/C++ file edited with Edit/Write with `-fsyntax-only` and the build's flags
  (`build-host/compile_commands.json`, then any other `build*/`). `shell_edit_check.py` does the same for the files a Bash command
  that writes (sed -i, a python heredoc, `cat >`) names; nothing is refused. By hand: `python3 -I .claude/hooks/syntax_check.py <files>`.
- `session_brief.py` prints the git state, the README milestones not done and the M2 status at session start.
- `verify_gate.py` makes an agent that edited code since the last ctest run run it (once) before stopping. Check: `--selftest`.
