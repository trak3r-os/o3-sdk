---
name: decide
description: Settle an architecture or product decision for o3sdk (the unit API, a HAL contract, the engine topology, the memory map, the image layout, an app's controls or file formats) with a small parallel design panel and record it for the maintainer to accept. Use for any cross-cutting decision that several work items depend on.
---

# Decision to record

Decisions are the bottleneck: make them explicit, cheap to review, and batch them for the maintainer.

## Where it is recorded
- An **app's product decision** (the ACC/SLIDE side, pattern length, one voice or two) is an item of that app's "Open decisions" section (`docs/DESIGN-303.md`, section 8): the maintainer answers, you record the answer in place with the date and his words.
- A **cross-cutting kit decision** (a change to `Unit`, a HAL contract, the engine topology, memory placement, what an image contains) is an ADR: `docs/adr/NNNN-<slug>.md` (next free number; create the directory on first use), status *Proposed*.

## Steps
1. **Context packet** (orchestrator, 300 words at most): the question, the constraints (MK66 budgets, real-time rules, the layering, what track3r-os already proved on the unit), links to the docs. No file dumps.
2. **Panel.** A small decision: one `Plan` agent. A cross-cutting one: three `Plan` agents in parallel, one lens each, 400 words back at most:
   - *Real-time and resources*: CPU, memory, ISR and failure modes on the MK66.
   - *Musician and hardware*: the workflow on 48 pads, 33 buttons and one encoder; what the player gains or loses.
   - *Kit and verification*: layering, the simulator, testability, what the next app inherits, how hard it is to undo.
   Each returns 2-4 options, a recommendation, costs, risks and "what would change my mind".
3. **ADR body**: Context, Options (table: option, cost, risk, reversibility), Panel views (one line each), Recommendation, Consequences (what it unlocks or blocks), Validation (what to measure, which `hw-session` check), Links.
4. **Maintainer.** Batch open decisions into one round of questions (4 per call at most, recommendation first, the path in the description). Record the answer: *Accepted* / *Rejected* / *Superseded by NNNN*, date, his words; `doc-keeper` mirrors it where the docs quote the old state. If he is away, leave it *Proposed* and list it under the questions waiting for him in the handoff.
5. Accepted ADRs bind the code and specs; changing one needs a new ADR that supersedes it, or a dated amendment inside it.
