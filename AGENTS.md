# AGENTS.md: o3-sdk for coding agents

For any coding agent, whatever the tool. `CLAUDE.md` is the compact rule sheet that Claude Code loads; this file is the working guide.
Humans read `README.md` and `docs/NEW-APP.md`. Everything in the repository is written in English.

o3-sdk is a C++17 bare-metal framework for instruments on the Tracker hardware (NXP MK66FX1M0, Cortex-M4F at 180 MHz with a single-precision
FPU, 256 KiB SRAM, 16 MiB SDRAM, FT812 display 800x480, SGTL5000 codec, 48 pads with monochrome LEDs, 33 buttons, one encoder). **An app is a
whole firmware image**; the kit is the library under it. The promise: a new instrument needs only its own engine (units, screens, controls);
the machine bring-up, UI kit, engine plumbing, build, simulator, tests, image, gate and the remote install over USB already exist.

## 1. Which job is this?

| Job | Where it happens |
|---|---|
| **Build a new instrument** (the usual one) | In **its own repository**, started from `templates/app` with `python3 -I tools/new_app.py <dir> <app-id>`. The repository sits next to `o3-sdk` and uses it as it is. Never put a new app in `o3-sdk/apps/` (those are the kit's examples: `hello`, `fillrate`). The worked example is `../o3-303` |
| Change an app | In the app's repository. It never edits `../o3-sdk`, not even to get a build through |
| Change the kit, a unit the kit ships, the platform, the simulator, the docs | Here, under this repository's rules, with a host test or a scenario, and the doc that the change makes false updated in the same pass |

If a task is ambiguous in a way that changes the design (which layer, which API, a file format), stop and ask; do not guess.

## 2. Orientation: what to read, in order

1. `README.md`: what is here, what is verified on the unit, the quick start.
2. `docs/NEW-APP.md`: the path from zero to an image on the unit, with the real files. **Start here for a new instrument.**
3. `docs/FRAMEWORK.md`: the reference (units, engine, apps, controls, runtime, simulator, the capability map in section 10: what is done and
   verified on the unit, done on the host only, or planned; section 11, an app in its own repository).
4. The header comments of the kit headers you touch are the API reference: `kit/dsp/include/o3sdk/dsp/{unit,engine,event,transport}.h`,
   `kit/app/include/o3sdk/app/{app,screen,context,input,panel,param_keys,widgets,pad_leds,diag,runtime}.h`, `kit/ui/include/o3sdk/ui/{renderer,components,style,theme}.h`,
   `kit/core/include/o3sdk/core/{param,placement}.h`.
5. `platform/mk66/README.md` (boot, memory map, the image, the checks, blink codes) and `platform/mk66/tools/README.md` (the USB tools and
   the static checks); `sim/README.md` (scenarios); `docs/ci.md`; `docs/hw/` (what the unit has shown, dated).
6. `docs/M2-PLAN.md`, `docs/M2B-PLAN.md`: the plans and decisions of the platform milestones; history and ownership tables, not a guide.

Locate before you read: search for the symbol or file, then read the function you need and its callers. Match the surrounding style.

## 3. Layout and layering

```
kit/{core,hal,eve,ui,dsp,app,usb}   the framework (one library each)    units/{synth,delay}    the kit's example units
platform/host                       HAL fakes for the simulator         apps/{hello,fillrate}  the kit's example apps
platform/mk66                       the Tracker: drivers, image, USB    sim/                   the headless simulator
templates/app, tools/new_app.py     the starter for a new app           tests/                 host tests (doctest)
cmake/                              o3sdk_app, o3sdk_unit, o3sdk_test_suite, the toolchain files
```

Layering is enforced by link dependency, not by convention: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`, `core <- usb` (linked by
the MK66 platform, never by an app). A **unit depends on `dsp` only**; an **app depends on `app` and its units**. `platform/*` and `sim`
implement the HAL contracts (`kit/hal`). A public header lives in `<layer>/include/o3sdk/<layer>/` and compiles alone (the `<layer>_hdrcheck`
targets). A path inside o3-sdk is `O3SDK_ROOT` in CMake, never `PROJECT_SOURCE_DIR`.

## 4. Commands

```sh
# host: build, test, simulate                      (nice -n 10 for builds; -j4 at most)
cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
mkdir -p <dir> && build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out <dir>   # PNG + WAV; exit 1: failed or a frame did not fit
cmake -S . -B build-host-asan -DO3SDK_SANITIZE=ON && cmake --build build-host-asan -j4                   # buffer and DSP code, host only

# cross: the image of every app, with its static checks
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir> && cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure
arm-none-eabi-nm -u <archive> | grep __aeabi_d        # must print nothing

# the whole check in one command, from a fresh directory (an app repository has the same two scripts)
tools/ci.sh host | tools/ci.sh mk66                   # warnings must be 0; mk66 adds the image checks, sizes and the double check
tools/remote_ci.sh host | mk66                        # on the remote build machine; exit 3 or 255: run ci.sh locally (docs/ci.md)
python3 -I tools/gate.py <app> --host-build build-host   # the release gate's mechanical half: two fresh builds, identical HEX, checks, build-gate-a/CANDIDATE.md

# a new app
python3 -I tools/new_app.py <dest-dir> <app-id> [--name NAME] [--git] [--no-harness]
ctest --test-dir build-host -L template               # the template, instantiated and built (host; in the mk66 tree: its image checks)
```

An app repository: `build-host/app/<id>_sim`, `build-mk66/app/<id>.{elf,hex,bin,ptf}`, ctests `<id>_image`, `<id>_flashconfig`, `<id>_ramfunc`,
`<id>_usb`. In o3-sdk's own tree the apps are in `build-*/apps/<name>/`. A new `*_test.cpp` or library source needs a cmake configure rerun.
While other agents share the tree, build only in your own directory (`build-<yourtask>-host`, `build-<yourtask>-mk66`; all `build*/` are untracked).

## 5. Rules

**Hard rules** (they override everything else):
- Never add code, assets, notes or reverse-engineering material derived from the official firmware. "Polyend" and "Tracker" only describe the
  hardware. Ported or vendored code needs a licence gate and a `THIRD_PARTY.md` row (skill `dsp-port`). Comments that cite `OS-...` or `ADR ...`
  refer to track3r-os documents and may stay.
- **Firmware reaches a unit only through a documented update path**, after `fw-gate-check` passes and with the maintainer's go for that image
  (section 7). No debug probe, no improvised uploader.
- No secrets, personal data or binaries in git (no `.ptf .hex .elf .bin .a .o .wav`). Commit, stage, tag or push only when the maintainer asks.
- Never edit anything under `../track3r-os`. **Ported code stays identical**: `kit/core`, `kit/hal`, `kit/eve`, `kit/ui`, `kit/usb`,
  `platform/host`, the MK66 drivers (display, input, audio, boot, usb, storage, update) and the USB tools come from track3r-os, where the
  unit has proven them; they differ only by the rename `o3os` to `o3sdk` and the documented cuts. Do not "fix" them here, and do not work around
  a defect in them: report it to the maintainer as a track3r-os finding with the evidence. New o3-sdk code (`kit/dsp`, `kit/app`, `platform/mk66/src/main.cpp`
  and the console glue, `tools/`, `templates/`, the docs) is edited normally.
- Hardware register values copied from track3r-os (clock tree, SDRAM controller, I2S dividers, SGTL5000 writes, FT812 bring-up, TCA8418 and
  IS31FL3731 setup, the flash configuration bytes `0x400..0x40F` = `ffffffffffffffffffffffffdef9ffff`) are proven on a unit: keep them exactly.
  Cutting code is fine; "improving" a value without a unit to test it on is not. A wrong flash configuration can lock the chip for good.

**Engineering rules** (non-negotiable):
- Real-time paths (`Unit::process`, `Clocked::block`, `Engine::render`, `Runtime::block_callback`, ISRs and what they call): no heap, no SD or file
  I/O, no blocking, bounded loops, no `printf`, no `double`. Precompute outside.
- No heap anywhere in the kit, units or apps; no RTTI, no exceptions. Big buffers (delay lines, samples) are `O3SDK_BIG` (SDRAM,
  `kit/core/include/o3sdk/core/placement.h`), start at zero, and are handed to the unit at construction.
- `float` only in DSP (`double` is emulated in software on the M4F): `f` suffixes, block processing, coefficients per block or per event,
  no `libm` in a sample loop when avoidable. The build treats unsuffixed constants as `float`; the cross build is checked for `__aeabi_d*`.
- Data shared between an ISR or the render context and the main loop: atomics or `volatile`, multi-word updates in a critical section or a
  documented single-writer protocol, stated in a comment at the declaration.
- A unit's parameter id is its index in the table: append, never insert. A unit never owns the values the user sees; `kParamSmooth` is advisory (smooth inside the unit).
- Alignment is declared, never placed: `alignas` plus a `static_assert` on every buffer touched by word or multi-word loads.
- No undefined behaviour: every non-void path returns, no uninitialised reads, no out-of-bounds indexing, no type punning (`memcpy`).
- Warning-free under `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Werror`.
- CPU is the binding constraint: a per-block feature states its estimated cost and is measured on the unit later (skill `rt-optimize`).
  A host test is not that measurement; label estimates as estimates.
- Pure logic gets a doctest. A screen or a sound change gets a simulator scenario, run and looked at. A kit or HAL change builds every
  implementer of the contract (`platform/host`, `platform/mk66`, the simulator).

**Scope**: the smallest change that fully solves the task; no drive-by refactors (list them as follow-ups); other agents may edit other files
at the same time, so stay in your files and report a change you had to make elsewhere.

## 6. Verify, and say what is not verified

Never say "done", "fixed" or "passing" without the proving command run in the same turn and its result quoted.

| Level | Evidence |
|---|---|
| Builds | the build exits 0 with no warning (`-Werror` is on; also grep the output) |
| Host tests | the full `ctest --test-dir build-host` (not a subset) passes; quote its summary line ("100% tests passed ... out of N") |
| Screens and sound | the scenario ran (`<app>_sim --scenario ... --out <dir>`, exit 0), the PNGs were opened and looked at, the WAV was checked |
| Buffers and DSP | once under `-DO3SDK_SANITIZE=ON` |
| Image | cross build warning-free, `ctest -L mk66` (the image's static checks), no `__aeabi_d*`, sizes against the previous figure |
| Release candidate | skill `fw-gate-check` (`tools/gate.py` does its mechanical steps): identity of both trees, two fresh builds with identical HEX, the checks, SHA-256, a manifest; never flashes |
| On the unit | skill `hw-session` with the maintainer; over USB the test session can read `version state cpu log usb leds`, take `--shot`, inject `key pad enc` and record the master |

A host test is **not** hardware verification: always name the check that remains for the unit. What only the unit can show today: the screen by
eye (the first shot's colours differ from the simulator's: the theme or the shot's colour order is open), real presses of pads, buttons and the
encoder, the pad LEDs by eye, the analogue outputs by ear, the render's CPU cost of anything new (header strip, `o3console.py cpu`), a long soak,
the crash record's round trip, the FT812's per-line fill rate (`apps/fillrate`). `docs/FRAMEWORK.md` section 10 and `docs/hw/` say, per capability,
what has been seen on a unit and when.

## 7. Firmware install rules, and the guard hooks

The documented update paths, and nothing else:
1. **By card, by hand**: the `.ptf` alone in `Firmware/` of the SD card, then the hidden reset button (`platform/mk66/README.md`).
2. **By an agent onto a mounted card**: only an SD card mounted under `/media/<user>/<volume>` or `/run/media/<user>/<volume>`; never `/mnt`,
   never a raw device. Formatting a card, or deleting files you did not put there, needs the maintainer's ask.
3. **By the USB console** of an image already on the unit: `o3console.py --put <ptf>`, then `--update` (`docs/NEW-APP.md` section 11). The tools
   are not run against a unit from a development session: the hardware session does it, with the maintainer's go for that image, after the gate,
   and only for an image whose `<app>_usb` check passed (an image with no console strands the unit until a person uses the card and the reset button).
   Keep a known-good image as the card's first `.ptf` (the target of the reset button) before the first install.

Hook-enforced where the tool supports hooks (`.claude/hooks/guard_flash.py`, PreToolUse on shell commands and file edits; check:
`python3 -I .claude/hooks/guard_flash.py --selftest`): it refuses a hardware uploader or debug probe in command position (`openocd`, `pyocd`, J-Link,
`teensy_loader_cli`, `dfu-util`, `st-flash`, `probe-rs`, ...), a raw block-device write or a partition or format tool, a write to `/mnt` or to a media path that is
not a mounted volume, and `git add` of a firmware image or binary. It fails open on a payload it cannot parse: **the hook is a backstop, not
the rule**; an agent whose tool has no hooks follows the same rules by itself.

## 8. The agent harness (`.claude/`)

Claude Code loads it by itself; for any other tool the files are plain Markdown and Python 3 scripts you can read and run. In an app repository
the harness is a copy (`tools/new_app.py` copies it); its texts name o3-sdk's paths, which are `../o3-sdk/...` from the app. Read `apps/<name>/` in them as
`app/`, `build-host/apps/<name>/<name>_sim` as `build-host/app/<id>_sim` and `build-gate-a/apps/<name>/` as `build-gate-a/app/`; the harness
files themselves are not edited in an app repository without need (they drift from o3-sdk's copy: re-copy a newer one).

| Agent | Use it for |
|---|---|
| `firmware-engineer` | one scoped edit (a kit layer, a unit, an app, a screen, the platform, CMake): implements, builds, tests, reports numbers; does not commit |
| `firmware-reviewer` | independent read-only review after each engineer change: correctness, real-time hazards, UB, layering, unproven hardware values, licence |
| `build-verifier` | mechanical numbers only: host build, ctest, simulator runs, cross build sizes, the static image gate |
| `doc-keeper` | the README status table, plan documents, `FRAMEWORK.md` sections and `THIRD_PARTY.md`, from facts it is given |
| `dsp-scout` | licence and cost research for an open-source DSP to port as a unit |

| Skill | When |
|---|---|
| `app-feature` | the pipeline for any feature, fix or refactor: scope, questions, spec, implement, verify, review, fold, document; risk levels A, B, C |
| `fw-build` | build, test, sizes, the `__aeabi_d*` check |
| `fw-gate-check` | before an image goes to the maintainer: reproducible build twice, image checks, manifest; never flashes |
| `dsp-port` | licence gate, float-only port, oracle test and cost estimate for bringing open DSP in as a unit |
| `rt-optimize` | structure of `process()`, memory placement, how to measure; no cycle claim without the unit |
| `hw-session` | one batched session with the unit: gate-checked images, an ordered checklist, capture, ingest of the results |
| `decide` | a cross-cutting or product decision recorded for the maintainer (an ADR, or the design doc's open items) |
| `handoff` | closing a session so the next one resumes cold |

| Hook | Does |
|---|---|
| `guard_flash.py` | the install rules above |
| `syntax_check.py`, `shell_edit_check.py` | `-fsyntax-only` of each C/C++ file you edit (with Edit/Write, or named by a shell command that writes), with the build's flags from `build-host/compile_commands.json`: configure a host build first. By hand: `python3 -I .claude/hooks/syntax_check.py <files>` |
| `session_brief.py` | the git state and the open milestones at the start of a session |
| `verify_gate.py` | an agent that edited code runs the ctest once before it stops |

**The orchestrator pattern** (`app-feature`): one agent decides, briefs and reads verdicts; an engineer implements one scoped change; a reviewer
reads it independently while a verifier gives the numbers; findings go back to the same engineer, each fixed with evidence or refuted with a
concrete reason; two rounds at most. An engineer's final report is 250 words at most: what changed (file:line), why it is correct, build and test
result with exit codes and counts, tests and scenarios added, docs touched, risks, follow-ups, and the check that still needs the unit.

## 9. How an agent builds a new instrument (the path)

1. Confirm the idea's shape with the maintainer: units (what sounds), screens, controls on 48 pads, 33 buttons and one encoder, what is saved (nothing can be saved yet).
2. `python3 -I tools/new_app.py ../<repo> <app-id> --name <NAME> --git`; build, test and run the scenario as the first proof that the toolchain works.
3. One unit at a time (`docs/NEW-APP.md` sections 3 and 6): a parameter table, `prepare`, `process`, a test against the engine, a cost estimate; port open DSP through `dsp-port`.
4. Screens and controls (sections 4 and 5): a scenario per screen, run and looked at; keep the pad map in one pure header with a test.
5. A sequencer as a `dsp::Clocked` hook if the instrument has one (section 7), with its arithmetic tested on the host.
6. Make the unit able to say what it is doing: `App::describe_state` (and `diag_commands` if needed), so the hardware session can check state from the computer.
7. `tools/ci.sh host` and `tools/ci.sh mk66` clean; sizes against the budgets (flash 1 MiB, SRAM_U 192 KiB with 32 KiB free, SDRAM 16 MiB; the template is 149 KB flash, 78 KB SRAM_U).
8. Commit (when asked), `python3 -I tools/gate.py <id> --host-build build-host` and the skill `fw-gate-check`'s second pass, hand the candidate and the unit-only checklist to the maintainer (`hw-session`). Stop there.

## 10. Pitfalls that have cost time

- The app library defines `o3sdk_app()` and is linked **whole**; a test that names it needs `$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<id>>`.
- A cached `O3SDK_DIR` survives in a build directory; pass `-DO3SDK_DIR` again or use a fresh directory.
- `O3SDK_BIG` objects start at zero and refuse initialisers on the cross compiler (the host accepts them: a host-green change can break the cross build).
- The display list is 2048 words; a frame that does not fit is dropped silently on the unit and is `refused` (exit 1) in the simulator.
- The simulator's `--out` directory must exist; its PNGs are 800 x 697 (the screen above the pads), the unit's `--shot` 800 x 480.
- `o3console.py --put` of a name already on the card is refused; use a new name. After a put, `fw` shows `first-ptf`: the reset button's target may have moved.
- The Config button belongs to the kit (the system Config screen is planned): a new app does not give it a meaning.
- An image built without the console (`-DO3SDK_MK66_CONSOLE=OFF`, the skeleton) cannot be installed or read over USB.
- Build identity: an image built without a commit says `unknown`, with changed tracked files `-dirty`; the gate wants both trees clean and committed.
