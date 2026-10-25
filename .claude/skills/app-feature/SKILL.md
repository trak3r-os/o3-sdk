---
name: app-feature
description: Orchestrator playbook to build one feature, fix or refactor of o3sdk end-to-end with subagents - scope, questions, spec, implement, verify, review, fold, document - for an app (a screen, a device, a sequencer, a sound) or the kit (a layer, a unit, the engine, the platform), with a simulator scenario for every screen or sound change. Argument - a short description or the milestone item.
---

# Feature pipeline (orchestrator)

The orchestrator owns decisions, sequencing and the maintainer's questions; subagents do the work. Open each progress message with `phase k/8` and keep it to a line or two. It does not review: it never reads diffs, code or build logs itself, it reads verdicts (15 lines at most), exit codes and ctest summary lines; when a verdict is not enough, it asks the reviewer for another pass.

## Risk level first
- **A** (typo, docs, isolated tooling, trivial local fix): a scope note, the fix, the relevant check, self-review. Phases 1-3 and 6 are not needed.
- **B** (a feature, a refactor, an ordinary bug fix, a new unit or screen): written intent and acceptance, implementation, host tests plus the simulator scenario and the cross build when relevant, one independent review.
- **C** (the render path and ISRs, the `Unit`/HAL/engine contracts and anything an app is built against, `platform/mk66` hardware values, the linker and memory map, the image and its flash configuration field, any persistent file format once one exists): every phase, a risk review plus an independent review, the maintainer's decisions, and a named unit check.

| Phase | Who | Output |
|---|---|---|
| 1 Scope | orchestrator (+ `Explore`, `dsp-scout` for ports) | what exists, what is unknown, 1-3 approaches with CPU and memory cost |
| 2 Questions | orchestrator to the maintainer | one table of decisions, recommendation first (`decide` for cross-cutting ones); if he is away, park the item and say which question blocks it |
| 3 Spec | orchestrator, `Plan` for big items | a note in the brief, or the design doc: goal, behaviour, parameters with ranges and defaults, **Done when** (checkable by build, tests or simulator), out of scope, CPU/memory estimate, unit-only checks |
| 4 Implement | `firmware-engineer` | change, tests, scenario, docs, scoped numbers |
| 5 Verify | `build-verifier` | exit codes, ctest summary, simulator summary, sizes |
| 6 Review | `firmware-reviewer` (read-only), in parallel with 5 | ranked findings and a verdict |
| 7 Fold | the same engineer, by SendMessage | every finding fixed or refuted; two rounds at most, then report to the maintainer |
| 8 Integrate | `doc-keeper`; the orchestrator | docs updated; a commit only if the maintainer asked |

Agents take their model from their definition; pass `model` only to the built-in agents.

## Briefing the engineer
A packet, not a spec dump: the task, risk level, the Done-when list, the file scope (the M2 ownership table when a slice is open: other agents edit other files at the same time), the doc sections that apply (path and lines), the build directory to use, the CPU and memory budget, the skill to follow (`dsp-port`, `rt-optimize`), and on a fold the findings. Do not paste CLAUDE.md or the agent's own rules; it has both. Per-task words go in the prompt, not in the agent files.

## What each kind of change must leave behind
- **Pure logic** (DSP maths, sequencer arithmetic, mappings): a doctest in `tests/`.
- **A screen**: a scenario in `apps/<app>/scenarios/` (commands in `sim/README.md`), run with `<app>_sim`, and the PNGs looked at; a scenario that matters also gets a ctest, as `tests/sim_hello_test.cpp` does for HELLO.
- **A sound**: a host test of the maths against the engine (`tests/units_test.cpp`), the WAV from a scenario, and the cost estimate labelled *estimate*.
- **A kit or HAL change**: every implementer of the contract (`platform/host`, `platform/mk66`, the simulator) builds; a public header compiles alone; the layering holds.
- **A platform change**: the host `mk66` ctests, the cross build, and the image gate (`fw-gate-check`) before any image reaches the maintainer.
- A new third-party component or port: a row in `THIRD_PARTY.md` and the licence gate of `dsp-port`.

## Definition of done
Done-when met (Level B/C) - build exit 0 with no warnings - ctest passes (and the cross build when relevant) - the scenario run and looked at - reviewer APPROVE or every finding folded - docs updated - the unit-only checks listed for the next `hw-session`. Never say "done", "fixed" or "passing" without the proving command run in the same turn and its result quoted. No commit, no push, no tag unless the maintainer asks.
