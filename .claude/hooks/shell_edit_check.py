#!/usr/bin/env python3
"""PostToolUse(Bash) check: syntax-check the C/C++ files a shell command edited.

syntax_check.py covers the Edit and Write tools. Agents also edit C/C++ from the shell (sed -i, a python heredoc with write_text,
`cat > file <<EOF`, cp, mv), which skips that check. This hook closes the gap without refusing anything: after a Bash command that
looks like a write, it takes the C/C++ files of the tree that were modified in the last few minutes and are named by the command
(by path or by file name), and runs the same -fsyntax-only check on them (errors exit 2 and are shown to the model).

Why it checks and does not refuse (track3r-os refuses shell edits): the agents of this repository edit C++ from the shell all the
time (a replay of 646 tool calls of the M1/M2 sessions: 76 would have been refused), and a refusal there stops live work. A command
that edits many files through a glob names none of them: run `python3 -I .claude/hooks/syntax_check.py <files>` on them.

`shell_edit_check.py --selftest` checks the matching on a scratch tree.
"""
import json
import os
import re
import sys
import tempfile
import time

sys.dont_write_bytecode = True  # no __pycache__ next to the hooks
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script directory from sys.path
import syntax_check  # noqa: E402

WINDOW_S = 300
MAX_FILES = 6
SKIP_DIRS = {".git", ".claude", "third_party", "__pycache__", "node_modules"}
CXX_EXT = (".c", ".cc", ".cpp", ".h", ".hh", ".hpp")
# A command that may write a file: sed/perl -i, a redirect (not 2>&1), tee, cp/mv/install/dd/ed/ex/truncate, a script's write calls.
WRITE_MARK = re.compile(
    r"\b(?:sed|perl|ruby)\b[^|;&\n]*\s-\w*i|--in-place|(?<![0-9&>])>>?\s*[^\s&>]|\btee\b|\b(?:cp|mv|install|dd|ed|ex|truncate|sponge)\b"
    r"|write_text\(|write_bytes\(|\.write\(|\.writelines\(|shutil\.|os\.(?:replace|rename)"
    r"""|open\([^)]*['"][rwabx]*[wax+][rwabx+]*['"]""")


def is_write_command(command):
    return bool(WRITE_MARK.search(command))


def tree_files(root, exts):
    """(relative path, absolute path) of the files of the tree with one of the extensions; build*/ and vendored code skipped."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith("build")]
        for name in filenames:
            if name.endswith(exts):
                full = os.path.join(dirpath, name)
                yield os.path.relpath(full, root), full


def named_files(text, root, exts, newer_than=None):
    """Files of the tree named in text (by relative path or by file name), modified at or after newer_than if given."""
    found = []
    for rel, full in tree_files(root, exts):
        base = os.path.basename(rel)
        if rel not in text and not re.search(r"(?<![\w.-])" + re.escape(base) + r"(?![\w])", text):
            continue
        try:
            mtime = os.path.getmtime(full)
        except OSError:
            continue
        if newer_than is None or mtime >= newer_than:
            found.append((rel, mtime))
    return found


def check_command(command, root, now=None):
    """(exit code, stderr text, context text) for a Bash command."""
    if not is_write_command(command):
        return 0, "", ""
    now = time.time() if now is None else now
    hits = named_files(command, root, CXX_EXT, newer_than=now - WINDOW_S)[:MAX_FILES]
    errors, notes = [], []
    for rel, _ in hits:
        rc, err, ctx = syntax_check.check_file(os.path.join(root, rel))
        if rc:
            errors.append(err)
        elif ctx:
            notes.append(ctx)
    if errors:
        return 2, "\n\n".join(errors), ""
    return 0, "", "\n".join(notes)


def main():
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    if not isinstance(payload, dict):
        return 0
    command = (payload.get("tool_input") or {}).get("command") or ""
    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd()
    try:
        rc, err, ctx = check_command(command, os.path.realpath(root))
    except Exception:  # fail open: a hook bug must never break a tool call
        return 0
    if err:
        print(err, file=sys.stderr)
    if ctx:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": ctx}}))
    return rc


def selftest():
    bad = []
    with tempfile.TemporaryDirectory(dir="/tmp") as root:
        root = os.path.realpath(root)
        os.makedirs(os.path.join(root, ".git"))
        os.makedirs(os.path.join(root, "kit", "src"))
        os.makedirs(os.path.join(root, "build-host"))
        sources = {"broken.cpp": "int f() { return nope; }\n", "fine.cpp": "int g() { return 1; }\n",
                   "other.cpp": "int h() { return nope2; }\n"}
        entries = []
        for name, text in sources.items():
            full = os.path.join(root, "kit", "src", name)
            with open(full, "w") as fh:
                fh.write(text)
            entries.append({"directory": os.path.join(root, "build-host"), "file": full,
                            "command": f"/usr/bin/c++ -Wall -o x.o -c {full}"})
        with open(os.path.join(root, "build-host", "compile_commands.json"), "w") as fh:
            json.dump(entries, fh)
        syntax_check.PROJECT = type(syntax_check.PROJECT)(root)
        old = time.time() - 3600
        os.utime(os.path.join(root, "kit", "src", "other.cpp"), (old, old))

        cases = [
            ("python heredoc writing a broken file",
             "cd kit/src && python3 - <<'EOF'\nfrom pathlib import Path\nPath('broken.cpp').write_text('x')\nEOF", 2),
            ("sed -i on a broken file", "sed -i 's/a/b/' kit/src/broken.cpp", 2),
            ("cat > redirect on a good file", "cat > kit/src/fine.cpp <<'EOF'\nint g() { return 1; }\nEOF", 0),
            ("a read of the broken file is not a write", "grep -n nope kit/src/broken.cpp", 0),
            ("2>&1 is not a redirect into a file", "cmake --build build-host 2>&1 | tail -5 # broken.cpp", 0),
            ("a broken file that was not modified recently is left alone", "sed -i 's/a/b/' kit/src/other.cpp", 0),
            ("a file the command does not name is left alone", "sed -i 's/a/b/' kit/src/unknown.cpp", 0),
            ("no command", "", 0),
        ]
        for label, command, want in cases:
            rc, err, _ = check_command(command, root)
            if rc != want:
                bad.append(f"{label}: rc {rc}, wanted {want}")
            if label.startswith("python heredoc") and "nope" not in err:
                bad.append("the compiler error is not reported")
    for label in bad:
        print("FAIL " + label)
    print(f"{len(cases) - len(bad)}/{len(cases)} ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
