---
name: handoff
description: Close a working session cleanly so the next chat resumes cold without re-briefing - stop owned background agents, leave no unexplained changes, update the status docs, save durable lessons. Use when the maintainer says they will stop or continue in another chat, at a milestone, or when context is running long.
---

# Session handoff

1. **Agents.** Stop background agents this session owns (`TaskStop`) unless the maintainer asked to let them finish. If other agents share the tree (the M2 slices), tell them what you leave and where; do not touch their files.
2. **Tree.** Nothing unexplained: list `git status --short` in the handoff, grouped by owner or slice. Do not commit, stage, stash or revert unless the maintainer asked; there may be no commit yet. Remove only the scratch directories you made (`build-gate-*`, `/tmp` outputs); leave `build*/` that others use.
3. **Status.** `doc-keeper` (Haiku) with the facts (what was built, test counts, sizes, decisions with the maintainer's words) updates the README status table row, `docs/M2-PLAN.md`'s `Status:` line and a short list of what remains, in an order a fresh session can execute (the SessionStart hook prints the README table and that line). The maintainer alone moves a milestone to done.
4. **Memory.** Only durable maintainer preferences or corrections become memory, updating an existing entry rather than duplicating; project facts belong in the docs, not in memory.
5. **Maintainer message**, 8 lines at most, in the language the maintainer writes in (he chats in Spanish; everything written to the repository stays English): what is done, what is pending, where the next session resumes, the questions waiting for him, and which checks only the unit can give.
