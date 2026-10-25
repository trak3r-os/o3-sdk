#!/usr/bin/env python3
"""SessionStart hook: inject a short state brief so no session starts cold.

Prints (stdout becomes session context) the git state, the milestones of the README "Status" table that are not done yet, the status
line of docs/M2-PLAN.md, the build directories present and the rules that bite most. Kept under ~25 lines on purpose. Fails open:
it prints what it can, and works in a repository with no commit yet.
"""
import json
import os
import re
import subprocess
import sys

MAX_ROWS = 5
MAX_CHARS = 170


def git(root, *args):
    try:
        done = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout.rstrip("\n") if done.returncode == 0 else None


def clip(text):
    text = " ".join(text.split())
    return text if len(text) <= MAX_CHARS else text[:MAX_CHARS - 3] + "..."


def read_lines(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def open_milestones(root):
    """Rows of the README's '## Status' table whose State column does not start with 'done'."""
    rows, inside = [], False
    for line in read_lines(root, "README.md"):
        if line.startswith("## "):
            inside = line.strip() == "## Status"
            continue
        if inside and line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if len(cells) >= 3 and re.match(r"^M\d", cells[0]) and not cells[2].lower().startswith("done"):
                rows.append(f"{cells[0]} {cells[1]}: {cells[2]}")
    return rows


def plan_status(root):
    for line in read_lines(root, os.path.join("docs", "M2-PLAN.md"))[:12]:
        if line.startswith("Status:"):
            return line
    return ""


def main():
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        payload = {}
    if not isinstance(payload, dict):
        payload = {}
    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or "."

    branch = git(root, "branch", "--show-current") or "(detached)"
    head = git(root, "log", "-1", "--format=%h %s")
    dirty = [l for l in (git(root, "status", "--porcelain", "--untracked-files=normal") or "").splitlines() if l]
    print("## Session brief (auto, SessionStart hook)")
    if git(root, "rev-parse", "--is-inside-work-tree") is None:
        print("- Not a git repository.")
    else:
        where = f"at {clip(head)}" if head else "with no commit yet"
        names = ""
        if head and dirty:  # with no commit everything is untracked: the list would only be noise
            names = ": " + ", ".join(l[3:] for l in dirty[:4]) + (", ..." if len(dirty) > 4 else "")
        print(f"- Git: branch `{branch}` {where}; {len(dirty)} uncommitted path(s){names or '.'}")

    rows = open_milestones(root)
    if rows:
        print("- README Status, not done yet:")
        for row in rows[:MAX_ROWS]:
            print("  - " + clip(row))
    status = plan_status(root)
    if status:
        print("- docs/M2-PLAN.md " + clip(status))
        if "in progress" in status.lower():
            print("  M2 slices edit this tree at the same time: stay in your files (Ownership table), build only in your own "
                  "build-m2-<slice>-host / -mk66 directories, do not fix another slice's half-done work.")

    builds = sorted(d for d in os.listdir(root) if d.startswith("build") and os.path.isdir(os.path.join(root, d))) \
        if os.path.isdir(root) else []
    if builds:
        print("- Build dirs: " + ", ".join(builds[:8]) + (", ..." if len(builds) > 8 else "")
              + " (untracked; build-host feeds the syntax-check hook).")
    print("- Rules: CLAUDE.md. No probe or uploader, firmware only through a documented update path after fw-gate-check, never commit unless asked; "
          "builds under nice -n 10; verify before claiming done (name the unit check a host test cannot give); "
          "everything written in the repository is English.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
