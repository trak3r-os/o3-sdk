#!/usr/bin/env python3
"""PostToolUse(Edit|Write) check: compile the edited translation unit.

Runs the edited .c/.cc/.cpp file through its exact compile_commands.json invocation with -fsyntax-only (no objects, no dependency
files), so it sees the build's warning flags (-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Werror). Errors exit 2 and are
shown to the model; warnings in the edited file are fed back as context. A header is checked through, in order:
  1. its own self-containment stub when it is a public header (kit/<layer>/include/o3sdk/<layer>/x.h is compiled alone by the
     `<layer>_hdrcheck` target: o3sdk_<layer>_x_h.cpp), which also proves the header includes what it uses;
  2. the source of the same name in the same library;
  3. the first unit in the database that includes it (the same library first).
`syntax_check.py FILE...` checks files by hand (after a shell edit through a glob; shell_edit_check.py covers the files a
shell command names).

The compile databases come from build-host/ first, then every other build*/ directory, newest first (build-m2-<slice>-host,
build-mk66, ...), so files that only a platform or cross build compiles (platform/mk66) are found too. A file that is in no database
(not configured yet: rerun cmake after adding a source file) is skipped silently.

Several agents may share the tree (docs/M2-PLAN.md): an error that comes from a file you did not edit is another agent's half-done
work; the message says so.
"""
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

PROJECT = Path(os.environ.get("CLAUDE_PROJECT_DIR") or Path(__file__).resolve().parents[2])
SOURCE_EXTS = {".c", ".cpp", ".cc"}
HEADER_EXTS = {".h", ".hh", ".hpp"}
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ"}
DROP_FLAGS = {"-MD", "-MMD", "-c"}
PREFERRED_DIRS = ("build-host",)
MAX_LINES = 40


def checkout_root(source: Path) -> Path:
    """The checkout (main tree or a git worktree) that contains the file."""
    for parent in source.parents:
        if (parent / ".git").exists():
            return parent
    return PROJECT


def compile_databases(root: Path):
    first = [root / d / "compile_commands.json" for d in PREFERRED_DIRS]
    first = [p for p in first if p.is_file()]
    others = sorted((p for p in root.glob("build*/compile_commands.json") if p not in first),
                    key=lambda p: p.stat().st_mtime, reverse=True)
    return first + others


def load_entries(db: Path):
    """[(resolved source path, entry)] of a database whose build directory still exists; [] when unreadable."""
    try:
        entries = json.loads(db.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    out = []
    for entry in entries:
        if not Path(entry.get("directory", "")).is_dir():
            continue
        entry_file = Path(entry["file"])
        if not entry_file.is_absolute():
            entry_file = Path(entry["directory"]) / entry_file
        out.append((entry_file.resolve(), entry))
    return out


def find_entry(root: Path, source: Path):
    for db in compile_databases(root):
        for entry_file, entry in load_entries(db):
            if entry_file == source:
                return entry
    return None


def syntax_only_argv(entry):
    argv = entry.get("arguments") or shlex.split(entry["command"])
    out, skip = [], False
    for arg in argv:
        if skip:
            skip = False
        elif arg in DROP_WITH_ARG:
            skip = True
        elif arg not in DROP_FLAGS:
            out.append(arg)
    return out + ["-fsyntax-only"]


def library_root(path: Path):
    """kit/core for kit/core/include/o3sdk/core/x.h and for kit/core/src/x.cpp, else the file's directory."""
    parts = path.parts
    for marker in ("include", "src"):
        if marker in parts:
            return Path(*parts[:parts.index(marker)])
    return path.parent


def including_unit(root: Path, header: Path):
    """A compile entry that checks the header: its hdrcheck stub, the source of the same stem, then any includer."""
    ident = None
    if "include" in header.parts:
        rel = "/".join(header.parts[header.parts.index("include") + 1:])
        ident = re.sub(r"\W", "_", rel)  # CMake's MAKE_C_IDENTIFIER, as o3sdk_library() names the stubs
    lib = library_root(header)
    needles = (f'/{header.name}"', f'"{header.name}"', f'/{header.name}>', f'<{header.name}>')
    for db in compile_databases(root):
        units = load_entries(db)
        if ident:
            for entry_file, entry in units:
                if entry_file.stem == ident:
                    return entry_file, entry
        for entry_file, entry in units:
            if entry_file.stem == header.stem and library_root(entry_file) == lib:
                return entry_file, entry
        for same_lib in (True, False):
            for entry_file, entry in units:
                if (library_root(entry_file) == lib) != same_lib:
                    continue
                try:
                    text = entry_file.read_text(errors="replace")
                except OSError:
                    continue
                if any(n in text for n in needles):
                    return entry_file, entry
    return None, None


def check_file(raw: str):
    """(exit code, stderr text, context text) for one edited file."""
    source = Path(raw).resolve()
    if source.suffix not in SOURCE_EXTS | HEADER_EXTS:
        return 0, "", ""
    root = checkout_root(source)  # worktree edits use that worktree's build
    if not source.is_relative_to(root) or source.relative_to(root).parts[0].startswith("build"):
        return 0, "", ""
    unit = source
    if source.suffix in HEADER_EXTS:
        unit, entry = including_unit(root, source)
    else:
        entry = find_entry(root, source)
    if entry is None:
        return 0, "", ""  # not in the build (excluded file), no includer, or no compile database yet

    try:
        result = subprocess.run(["nice", "-n", "10"] + syntax_only_argv(entry),
                                cwd=entry["directory"], capture_output=True,
                                text=True, timeout=90)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 0, f"syntax check skipped: {exc}", ""

    rel = source.relative_to(root)
    via = "" if unit == source else f" (checked through {unit.relative_to(root)})"
    lines = result.stderr.splitlines()
    if result.returncode != 0:
        return 2, (f"Compile errors in {rel}{via} (-fsyntax-only, same flags as the build):\n"
                   + "\n".join(lines[:MAX_LINES])
                   + f"\nFix the errors that are in {rel}. An error reported in another file is that file's owner's half-done "
                   "work (docs/M2-PLAN.md, Ownership): do not fix it, mention it in your report."), ""

    own_warnings = [l for l in lines if "warning:" in l and str(source) in l]
    if own_warnings:
        return 0, "", f"Compiler warnings in {rel}{via} (the build treats warnings as errors):\n" + "\n".join(own_warnings[:15])
    return 0, "", ""


def main() -> int:
    if len(sys.argv) > 1:  # syntax_check.py FILE...: by hand
        worst = 0
        for raw in sys.argv[1:]:
            rc, err, ctx = check_file(raw)
            worst = max(worst, rc)
            print(err or ctx or f"ok {raw}")
        return worst
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    if not isinstance(payload, dict):
        return 0
    tool_input = payload.get("tool_input") or {}
    raw = tool_input.get("file_path") or (payload.get("tool_response") or {}).get("filePath")
    if not raw:
        return 0
    rc, err, ctx = check_file(raw)
    if err:
        print(err, file=sys.stderr)
    if ctx:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": ctx}}))
    return rc


if __name__ == "__main__":
    sys.exit(main())
