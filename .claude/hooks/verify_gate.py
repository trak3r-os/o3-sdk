#!/usr/bin/env python3
"""Stop/SubagentStop hook: verification before completion.

If a code file that THIS agent edited is newer than the last recorded test run, the agent may not stop yet: it is told once which
command proves its work. Only the agent's own edits count, so with several agents sharing the tree (docs/M2-PLAN.md) nobody is
held up for another agent's files. They are read from the agent's transcript: the Edit, Write and NotebookEdit calls, and the Bash
commands that look like a write and name a code file (sed -i, a python heredoc, cat > file; see shell_edit_check.py). A Stop hook
gets `transcript_path`, a SubagentStop hook `agent_transcript_path`. Without a transcript the hook does nothing.

The evidence is the file time of ctest's own log: the newest build*/Testing/Temporary/LastTest.log of any build directory
(build-host, build-m2-<slice>-host, build-mk66, ...), or of <git common dir>/ci-remote-last-run, which tools/remote_ci.sh touches whenever the
remote machine ran its tests to a verdict (green or red; not on exit 3 or 255, nor when the run died before ctest). A run of any ctest after
the edit satisfies it; the hook cannot tell which tests ran, so the final report must still quote the command and its result.
A Bash command counts only if it ran in this tree (`elsewhere`): a session opened here that works in a sibling repository names files
that are not these.
`stop_hook_active` guarantees a single nudge, never a loop. Fails open on anything unexpected.
`verify_gate.py --selftest` checks the logic on a scratch tree.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # no __pycache__ next to the hooks
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script directory from sys.path
import shell_edit_check  # noqa: E402

EDIT_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}
CODE_DIRS = ("kit/", "units/", "apps/", "app/", "platform/", "sim/", "tests/", "tools/", "cmake/")  # app/: an app repository's own code
CODE_FILES = ("CMakeLists.txt",)
CODE_EXT = (".c", ".cc", ".cpp", ".h", ".hh", ".hpp", ".py", ".txt", ".cmake", ".ld", ".in", ".s", ".S", ".sh")
LOG = os.path.join("Testing", "Temporary", "LastTest.log")
REMOTE_MARKER = "ci-remote-last-run"  # in the git common dir; tools/remote_ci.sh touches it after a remote run that reached its tests' verdict
MAX_LINES_SCANNED = 400000
CHDIR = re.compile(r"(?:^|[;&|(\n])\s*cd\s+(\S+)|\bgit\s+-C\s+(\S+)")
ASSIGN = re.compile(r"(?:^|[;&|(\s])([A-Za-z_]\w*)=(/[^\s;&|]*)")


def newest(paths):
    times = []
    for p in paths:
        try:
            times.append(os.path.getmtime(p))
        except OSError:
            pass
    return max(times) if times else 0.0


def remote_evidence(root):
    """File time of <git common dir>/ci-remote-last-run, 0.0 when there is none, no git or anything fails (fail open)."""
    try:
        out = subprocess.run(["git", "-C", root, "rev-parse", "--git-common-dir"], capture_output=True, text=True, timeout=5,
                             check=False)
        common = out.stdout.strip()
        if out.returncode != 0 or not common:
            return 0.0
        return newest([os.path.join(root, common, REMOTE_MARKER)])  # an absolute common dir stays as it is
    except (OSError, subprocess.SubprocessError, ValueError):
        return 0.0


def transcript_edits(transcript):
    """(paths of the Edit/Write calls, (command, cwd) of the Bash calls that look like a write) of a JSON-lines transcript; empty when
    unreadable."""
    paths, commands = [], []
    try:
        with open(transcript, encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh):
                if n > MAX_LINES_SCANNED:
                    break
                if '"tool_use"' not in line:
                    continue
                try:
                    entry = json.loads(line)
                except json.JSONDecodeError:
                    continue
                content = (entry.get("message") or {}).get("content")
                if not isinstance(content, list):
                    continue
                for item in content:
                    if not (isinstance(item, dict) and item.get("type") == "tool_use"):
                        continue
                    args = item.get("input") or {}
                    if item.get("name") in EDIT_TOOLS:
                        path = args.get("file_path") or args.get("notebook_path")
                        if isinstance(path, str):
                            paths.append(path)
                    elif item.get("name") == "Bash":
                        command = args.get("command")
                        if isinstance(command, str) and shell_edit_check.is_write_command(command):
                            commands.append((command, entry.get("cwd")))
    except OSError:
        return [], []
    return paths, commands


def inside(path, root):
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def elsewhere(command, cwd, root):
    """True when a Bash command ran in another tree: its `cd` or `git -C` targets are absolute paths outside root or, with no such target,
    the transcript row's working directory is. Anything unknown (no cwd, a relative target, a variable the command does not set) counts
    as here, so a real edit is not missed."""
    assigned = dict(ASSIGN.findall(command))
    targets = []
    for cd, git_c in CHDIR.findall(command):
        target = (cd or git_c).strip("'\"")
        var = re.fullmatch(r"\$\{?(\w+)\}?(/\S*)?", target)
        if var and var.group(1) in assigned:
            target = assigned[var.group(1)] + (var.group(2) or "")
        if target.startswith("/"):
            targets.append(target)
    if targets:
        return not any(inside(t, root) for t in targets)
    return bool(cwd) and not inside(cwd, root)


def is_code(rel):
    return (rel.startswith(CODE_DIRS) or rel in CODE_FILES) and rel.endswith(CODE_EXT) and not rel.startswith("build")


def verdict(root, transcript):
    """None, or the reason text that blocks the stop."""
    root = os.path.realpath(root)
    paths, commands = transcript_edits(transcript)
    commands = [command for command, cwd in commands if not elsewhere(command, cwd, root)]
    mine = {}
    for raw in paths:
        full = os.path.realpath(raw if os.path.isabs(raw) else os.path.join(root, raw))
        if full.startswith(root + os.sep):
            rel = os.path.relpath(full, root)
            if is_code(rel) and os.path.exists(full):
                mine[rel] = os.path.getmtime(full)
    if commands:
        for rel, mtime in shell_edit_check.named_files("\n".join(commands), root, CODE_EXT):
            if is_code(rel):
                mine.setdefault(rel, mtime)
    if not mine:
        return None
    evidence = max(newest(glob.glob(os.path.join(root, "build*", LOG))), remote_evidence(root))
    stale = sorted(f for f, mtime in mine.items() if mtime > evidence)
    if not stale:
        return None
    build_system = [f for f in stale if f.endswith(("CMakeLists.txt", ".cmake")) or f.startswith("cmake/")]
    cross = [f for f in stale if f.startswith(("platform/mk66", "cmake/")) or f.endswith(".ld")]
    steps = ["while RAM is short, the preferred check is the remote machine: O3SDK_CI_SCOPE=changed tools/remote_ci.sh host "
             "(or O3SDK_CI_TESTS=<regex> tools/remote_ci.sh host); its run counts as evidence once it reaches its tests' verdict",
             "or locally: nice -n 10 cmake --build build-host -j4 && nice -n 10 ctest --test-dir build-host --output-on-failure -L <area> | -R <test>"
             "   (while iterating: only the area or the test you touched, or O3SDK_CI_SCOPE=changed tools/ci.sh host; the full ctest, "
             "no filter, is the batch run before a release or a push; an M2 slice: its own build-m2-<slice>-host directory, "
             "ctest -L mk66 -R <pattern>)"]
    if build_system:
        steps[1] += "; a build-system change needs a fresh configure first (cmake -S . -B <dir>)"
    if cross:
        steps.append("nice -n 10 cmake --build <your mk66 build dir> -j4   (platform/mk66, linker or cmake change: the host "
                     "tests do not compile the hardware halves)")
    return ("Verification before completion: you edited code after the last recorded ctest run ("
            + ", ".join(stale[:4]) + (", ..." if len(stale) > 4 else "")
            + "). Run and quote the result (exit code, ctest pass count, warnings; sizes for a cross build) before claiming "
            "done:\n  - " + "\n  - ".join(steps)
            + "\nIf the change is intentionally unverified (WIP, a design note, an unbuildable slice), say so explicitly in your "
            "final message instead, and name the check that is still owed.")


def decide(payload):
    """The blocking reason for a Stop/SubagentStop payload, else None."""
    if not isinstance(payload, dict) or payload.get("stop_hook_active"):
        return None
    transcript = payload.get("agent_transcript_path")
    if not transcript and payload.get("hook_event_name") != "SubagentStop":
        transcript = payload.get("transcript_path")  # a SubagentStop's transcript_path may be the parent's: not used
    if not transcript:
        return None
    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or os.getcwd()
    try:
        return verdict(root, transcript)
    except Exception:  # fail open: a hook bug must never keep an agent from stopping
        return None


def main():
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    reason = decide(payload)
    if reason:
        print(json.dumps({"decision": "block", "reason": reason}))
    return 0


def selftest():
    def tool_line(name, **args):
        return json.dumps({"type": "assistant", "message": {"role": "assistant", "content": [
            {"type": "tool_use", "id": "t", "name": name, "input": args}]}})

    bad, total = [], 0
    with tempfile.TemporaryDirectory(dir="/tmp") as root:
        root = os.path.realpath(root)
        for rel in ("kit/core/src/a.cpp", "docs/note.md", "platform/mk66/src/b.cpp", "kit/core/src/c.cpp"):
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            with open(os.path.join(root, rel), "w") as fh:
                fh.write("x\n")
        log = os.path.join(root, "build-host", LOG)
        os.makedirs(os.path.dirname(log))
        open(log, "w").close()
        os.environ["CLAUDE_PROJECT_DIR"] = root

        def transcript(*lines):
            path = os.path.join(root, "t.jsonl")
            with open(path, "w") as fh:
                fh.write("\n".join(lines) + "\n")
            return path

        def at(rel, delta):  # file time relative to the test log
            t = os.path.getmtime(log) + delta
            os.utime(os.path.join(root, rel), (t, t))

        def case(label, want, payload):
            nonlocal total
            total += 1
            got = decide(payload)
            if (got is not None) != want:
                bad.append(label)
            return got

        a = os.path.join(root, "kit/core/src/a.cpp")
        b = os.path.join(root, "platform/mk66/src/b.cpp")
        at("kit/core/src/a.cpp", +10)
        case("edited after the tests: nudge", True, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        case("only read, not edited (someone else's file): no nudge", False,
             {"transcript_path": transcript(tool_line("Read", file_path=a))})
        case("nudge only once (stop_hook_active)", False,
             {"stop_hook_active": True, "transcript_path": transcript(tool_line("Edit", file_path=a))})
        case("a subagent uses its own transcript", True,
             {"hook_event_name": "SubagentStop", "agent_transcript_path": transcript(tool_line("Write", file_path=a))})
        case("a subagent without its own transcript: no nudge", False,
             {"hook_event_name": "SubagentStop", "transcript_path": transcript(tool_line("Edit", file_path=a))})
        case("a shell edit that names the file: nudge", True,
             {"transcript_path": transcript(tool_line("Bash", command="cd kit/core/src && sed -i 's/a/b/' a.cpp"))})
        case("a shell command that only reads it: no nudge", False,
             {"transcript_path": transcript(tool_line("Bash", command="grep -n foo kit/core/src/a.cpp"))})
        def ran_in(cwd, line):  # a transcript row with its working directory
            row = json.loads(line)
            row["cwd"] = cwd
            return json.dumps(row)

        edit = tool_line("Bash", command="sed -i 's/a/b/' kit/core/src/a.cpp")
        case("a shell edit run in this tree: nudge", True, {"transcript_path": transcript(ran_in(root, edit))})
        case("the same command run in a sibling tree: no nudge", False, {"transcript_path": transcript(ran_in("/elsewhere", edit))})
        case("a cd out of the tree: no nudge", False, {"transcript_path": transcript(ran_in(root, tool_line(
            "Bash", command="cd /elsewhere && sed -i 's/a/b/' kit/core/src/a.cpp")))})
        case("a cd into the tree from a sibling: nudge", True, {"transcript_path": transcript(ran_in("/elsewhere", tool_line(
            "Bash", command="cd " + root + "/kit/core/src && sed -i 's/a/b/' a.cpp")))})
        case("git -C through a variable set in the command, outside: no nudge", False, {"transcript_path": transcript(ran_in(root, tool_line(
            "Bash", command="R=/elsewhere; git -C $R log > /tmp/x kit/core/src/a.cpp")))})
        case("a variable the command does not set counts as here: nudge", True, {"transcript_path": transcript(ran_in(root, tool_line(
            "Bash", command="cd $SOMEWHERE && sed -i 's/a/b/' kit/core/src/a.cpp")))})
        case("an Edit call is judged by its path, not its cwd: nudge", True, {"transcript_path": transcript(ran_in(
            "/elsewhere", tool_line("Edit", file_path=a)))})
        at("kit/core/src/a.cpp", -10)
        case("tests ran after the edit: no nudge", False, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        at("docs/note.md", +10)
        case("docs are not code: no nudge", False,
             {"transcript_path": transcript(tool_line("Edit", file_path=os.path.join(root, "docs/note.md")))})
        at("platform/mk66/src/b.cpp", +10)
        why = case("platform edit: nudge", True, {"transcript_path": transcript(tool_line("Edit", file_path=b))})
        total += 1
        if not (why and "mk66 build dir" in why):
            bad.append("a platform/mk66 edit should name the cross build")
        case("file outside the tree: no nudge", False,
             {"transcript_path": transcript(tool_line("Edit", file_path="/tmp/elsewhere.cpp"))})
        # the remote marker: git common dir of the tree, touched by tools/remote_ci.sh
        subprocess.run(["git", "init", "-q", root], check=True)
        marker = os.path.join(root, ".git", REMOTE_MARKER)
        at("kit/core/src/a.cpp", +10)
        case("a git tree without a marker: nudge", True, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        open(marker, "w").close()
        t = os.path.getmtime(log) + 20
        os.utime(marker, (t, t))
        case("a remote run after the edit: no nudge", False, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        t = os.path.getmtime(log) + 5
        os.utime(marker, (t, t))
        case("a remote run before the edit: nudge", True, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        why = case("the nudge names the remote run", True, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        total += 1
        if not (why and "tools/remote_ci.sh host" in why and "O3SDK_CI_SCOPE=changed" in why and "O3SDK_CI_TESTS" in why):
            bad.append("the nudge should name tools/remote_ci.sh host with the scope and the tests")
        at("kit/core/src/a.cpp", -10)
        t = os.path.getmtime(log) - 100
        os.utime(marker, (t, t))
        case("an old marker does not hide a newer log: no nudge", False, {"transcript_path": transcript(tool_line("Edit", file_path=a))})
        os.remove(marker)
        case("empty transcript: no nudge", False, {"transcript_path": transcript()})
        case("missing transcript: no nudge", False, {"transcript_path": os.path.join(root, "missing.jsonl")})
        case("no transcript at all: no nudge", False, {})
    for label in bad:
        print("FAIL " + label)
    print(f"{total - len(bad)}/{total} ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
