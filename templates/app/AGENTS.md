# AGENTS.md: @APP_NAME@ for coding agents

For any coding agent, whatever the tool; `CLAUDE.md` is the compact rule sheet that Claude Code loads. This repository is one app on the
o3-sdk framework, which lives next to it in `../o3-sdk` and is used as it is. The framework's own guide for agents is `../o3-sdk/AGENTS.md`
(rules, the harness, the verification ladder, the install rules): read it, this file only adds what is specific to an app repository.
Everything here is written in English.

## Read in this order

1. `README.md`: what this instrument is and how to build it. Replace its description with the real one; a design document goes in `docs/`.
2. `../o3-sdk/docs/NEW-APP.md`: how to add a unit, a screen, controls, a scenario and a test, then build, gate and install the image.
3. `../o3-sdk/docs/FRAMEWORK.md`: the reference; section 10 says which hardware capabilities exist and which are verified on the unit.
4. The kit headers you use: their comments are the API reference (`../o3-sdk/kit/{dsp,app,ui,core}/include/o3sdk/...`).

## Where things go

| What | Where |
|---|---|
| A DSP unit | `units/<name>/{CMakeLists.txt, include/@APP_ID@/units/<name>/<name>.h, src/<name>.cpp}`; one line `o3sdk_unit(<name> SOURCES src/<name>.cpp)`; add `unit_<name>` to `DEPENDS` in `app/CMakeLists.txt` and `tests/CMakeLists.txt` |
| A screen | a class in `app/src/` (like `play_screen.cpp`), a member of the app in `app/src/app.cpp`, `ctx.show(screen)`; add the source to `app/CMakeLists.txt` |
| Pure logic (pad maps, sequencer arithmetic, formats) | headers in `app/include/@APP_ID@/`, a doctest in `tests/` |
| A scenario | `app/scenarios/*.txt` (commands: `../o3-sdk/sim/README.md`); `tests/app_test.cpp` runs every one |
| A sequencer or arpeggiator | a `dsp::Clocked` hook added with `engine.add_hook()` (an example: `../o3-sdk/apps/hello/src/hello_app.cpp`, `Riff`) |
| Console state and commands | `App::describe_state`, `App::diag_commands` in `app/src/app.cpp` |
| Something the kit lacks (a driver, an API, a widget) | **not here**: a change in `../o3-sdk`, under its rules, then this repository builds against it |

## Commands

```sh
cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure     # a new *_test.cpp: rerun the configure
mkdir -p <dir> && build-host/app/@APP_ID@_sim --scenario app/scenarios/boot.txt --out <dir>                     # then open the PNGs
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain dir> && cmake --build build-mk66 -j4
ctest --test-dir build-mk66 -L mk66 --output-on-failure                  # @APP_ID@_image, _flashconfig, _ramfunc, _usb
tools/ci.sh host | tools/ci.sh mk66                                      # fresh directory: build, warnings (0), ctest, sizes, no __aeabi_d
python3 -I tools/gate.py @APP_ID@ --host-build build-host                   # the release gate's mechanical half: build-gate-a/CANDIDATE.md (commit first)
```

o3-sdk is `../o3-sdk` (`-DO3SDK_DIR=<path>` or `$O3SDK_DIR` otherwise; a cached value stays until `-D` is passed again). Run builds under
`nice -n 10`, with `-j4` at most, in your own build directory.

## Rules (the short list; `../o3-sdk/AGENTS.md` section 5 is the full one)

- Layering is by link dependency: a unit depends on `dsp` only (`o3sdk_unit` links it); the app on `app` and its units.
- Real-time (`Unit::process`, `Clocked::block`, ISRs): no heap, no blocking, no SD or file I/O, no `printf`, no `double`, bounded loops. No heap, RTTI or
  exceptions anywhere. Big buffers are `O3SDK_BIG` (SDRAM, zero at start) and are handed to the unit at construction.
- A unit's parameter id is its index in the table: append, never insert. Smooth parameters inside the unit (`kParamSmooth` is advisory).
- Warning-free under `-Werror` (`-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion`). Shared ISR/main data: atomics or a documented protocol.
- Pure logic gets a doctest; a screen or a sound change gets a scenario you ran and looked at. Estimates of CPU are labelled estimates.
- **Never edit `../o3-sdk` or `../track3r-os`.** Never add anything derived from the official firmware.
- **Firmware install**: only through a documented path after the gate, with the maintainer's go: a `.ptf` on the card by hand or onto a mounted card
  (`/media`, `/run/media` only), or the USB console (`o3console.py --put`, `--update`: `../o3-sdk/docs/NEW-APP.md` section 11). No probe, no
  uploader, no raw device, no `/mnt`. The hook `.claude/hooks/guard_flash.py` backs this up; it is not the rule. No binaries in git; commit or push only when asked.

## Verify

Quote the proving command and its result in the same turn: warning-free build, the full host `ctest`, the scenario run and its PNGs looked at,
the image's four ctests, `tools/ci.sh host` and `tools/ci.sh mk66`. A host test is not hardware verification: name the check that stays for the
unit (the screen by eye, real presses, the LEDs by eye, sound by ear, the render's CPU % in the header and `o3console.py cpu`). Then `tools/gate.py` and the skill
`fw-gate-check` for a release candidate, and `hw-session` for the batch of unit checks, with the maintainer.

## Harness

`.claude/` is a copy of o3-sdk's: agents (`firmware-engineer` for one scoped change, `firmware-reviewer` read-only, `build-verifier` for numbers,
`doc-keeper`, `dsp-scout`), skills (`app-feature`, `fw-build`, `fw-gate-check`, `dsp-port`, `rt-optimize`, `hw-session`, `decide`, `handoff`) and hooks.
Their texts name o3-sdk's paths: from here the kit, the platform and the simulator are in `../o3-sdk`, and this repository's code is `app/`, `units/`
and `tests/`. `../o3-sdk/AGENTS.md` section 8 says when to use each. After pulling a newer o3-sdk, `python3 -I ../o3-sdk/tools/new_app.py
--sync-harness .` reports how this copy differs (`--write` copies o3-sdk's versions over and keeps what this repository added).
