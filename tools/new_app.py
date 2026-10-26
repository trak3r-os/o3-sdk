#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""new_app.py: instantiate templates/app into a new app repository that builds against this o3-sdk checkout.

    python3 -I tools/new_app.py <dest-dir> <app-id> [--name NAME] [--sdk-dir DIR] [--no-harness] [--git]
    python3 -I tools/new_app.py --sync-harness <app-dir> [--write] [--with-settings] [--diff] [--sdk-dir DIR]
    python3 -I tools/new_app.py --self-test

<dest-dir> is the new repository (it must not exist: nothing is ever overwritten; its parent is created if missing). <app-id> is the
app's identifier: lower case letters, digits and underscores, starting with a letter, 2 to 24 characters. It becomes the CMake project
and the image (`<id>.ptf`), the simulator (`<id>_sim`), the C++ namespace and an include directory, so it must also be a valid C++
identifier that does not collide with o3-sdk's own names (a refused id says why). --name is the name the screen shows (upper case
letters, digits, space and '-', at most 16 characters; default: the id in upper case, '_' as '-').

What it writes (templates/app/, then the files that must stay identical to o3-sdk's):
  - the template: CMakeLists.txt, app/ (one app, one screen, a scenario), units/voice/ (one unit), tests/, README.md, CLAUDE.md,
    AGENTS.md, .gitignore, with @APP_ID@ and @APP_NAME@ replaced and __APP_ID__ in paths replaced;
  - LICENSE (GPL-3.0-or-later: the image links the GPL kit), tools/ci.sh, tools/remote_ci.sh and tools/gate.py, copied from this checkout
    (they are the same files in o3-sdk and in an app: docs/ci.md, the release gate);
  - .claude/ (agents, skills, hooks, settings.json), copied from this checkout, unless --no-harness. The skills and agents are written
    for o3-sdk's layout: in an app the kit lives in ../o3-sdk (AGENTS.md of the new repository says so).
--git runs `git init` in the new repository (nothing is staged or committed). A traceable image needs a commit: the image carries the
git state of the app and of the kit (docs/FRAMEWORK.md section 11), "unknown" without one.

--sync-harness <app-dir> compares the harness of an existing app repository (<app-dir>/.claude: agents/, skills/, hooks/, and
settings.json with settings.local.json.example) with this checkout's (the one a new app is created with) and reports every file that
differs, that the app lacks ("missing") or that only the app has ("extra"). Nothing is written without --write; then o3-sdk's version of
every differing or missing file is copied over the app's, and a file the app added is never deleted or touched. The two settings files
hold the hook wiring and the app's own permissions, so they are reported but copied only with --with-settings. --diff prints the
unified diff of each differing file (the app's text to o3-sdk's). The harness drifts as o3-sdk's moves on (agents, skills and hooks are
copies): run it after pulling a newer o3-sdk, read the report, then --write and review the diff in the app. The tools (ci.sh,
remote_ci.sh, gate.py) are not part of it.

Python 3 standard library only. Exit status: 0 done (a new app created; or a harness in sync, or synchronised with --write), 1 a usage
error or a refused id/name/destination/directory, 2 the template is broken, 3 --sync-harness found differences and wrote nothing.
"""
import argparse
import contextlib
import difflib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

SDK_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(SDK_ROOT, "templates", "app")

ID_RE = re.compile(r"^[a-z][a-z0-9_]{1,23}$")
NAME_RE = re.compile(r"^[A-Z0-9][A-Z0-9 \-]{0,15}$")

# Words an id may not be: C++ keywords (the id is a namespace), the names of o3-sdk's namespaces and CMake targets (the id is a
# target and `using namespace o3sdk` in the template's sources would find two of them), CMake's reserved target names.
CXX_KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class
compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double
dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not
not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed sizeof static
static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using
virtual void volatile wchar_t while xor xor_eq""".split())
SDK_NAMES = set("""o3sdk core hal eve ui dsp app usb sim units platform std hello fillrate synth delay main tests test
platform_host""".split())
CMAKE_RESERVED = set("all clean help install test package package_source edit_cache rebuild_cache list_install_components".split())


def check_id(app_id):
    """None when `app_id` is acceptable, else the reason it is not."""
    if not ID_RE.match(app_id):
        return "must be 2 to 24 characters: a lower case letter, then lower case letters, digits or '_'"
    if "__" in app_id or app_id.endswith("_"):
        return "must not contain '__' or end with '_' (a C++ identifier rule, and the file names built from it)"
    if app_id in CXX_KEYWORDS:
        return "is a C++ keyword (the id is the app's namespace)"
    if app_id in SDK_NAMES:
        return "is a name o3-sdk uses (a namespace or a CMake target of the kit)"
    if app_id in CMAKE_RESERVED:
        return "is a name CMake reserves for a target"
    return None


def check_name(name):
    if not NAME_RE.match(name):
        return "must be 1 to 16 characters of upper case letters, digits, space and '-', starting with a letter or a digit"
    return None


def default_name(app_id):
    return app_id.upper().replace("_", "-")


def substitute(text, values):
    for key, value in values.items():
        text = text.replace(key, value)
    return text


def is_text(path):
    with open(path, "rb") as f:
        return b"\0" not in f.read(4096)


def copy_tree_substituted(src, dst, values):
    """Copies the template tree: path segments __APP_ID__ and file contents @APP_ID@ / @APP_NAME@ are replaced; modes are kept."""
    written = []
    for base, dirs, files in os.walk(src):
        dirs.sort()
        rel = os.path.relpath(base, src)
        parts = [] if rel == "." else [substitute(p, {"__APP_ID__": values["@APP_ID@"]}) for p in rel.split(os.sep)]
        target_dir = os.path.join(dst, *parts)
        os.makedirs(target_dir, exist_ok=True)
        for name in sorted(files):
            source = os.path.join(base, name)
            target = os.path.join(target_dir, substitute(name, {"__APP_ID__": values["@APP_ID@"]}))
            if is_text(source):
                with open(source, encoding="utf-8") as f:
                    text = f.read()
                with open(target, "w", encoding="utf-8", newline="\n") as f:
                    f.write(substitute(text, values))
            else:
                shutil.copyfile(source, target)
            shutil.copymode(source, target)
            written.append(target)
    return written


def leftover_tokens(paths):
    """Template tokens (@WORD@ or __APP_ID__) that survived the substitution: a bug in the template."""
    bad = []
    pattern = re.compile(r"@[A-Z][A-Z0-9_]*@|__APP_ID__")
    for path in paths:
        if pattern.search(path):
            bad.append((path, "path"))
            continue
        if is_text(path):
            with open(path, encoding="utf-8") as f:
                for number, line in enumerate(f, 1):
                    if pattern.search(line):
                        bad.append((path + ":%d" % number, line.strip()))
    return bad


def copy_harness(sdk, dst):
    """The Claude Code harness (agents, skills, hooks, settings), as o3-303 has it. Local settings and runtime locks are not copied."""
    src = os.path.join(sdk, ".claude")
    if not os.path.isdir(src):
        return False
    out = os.path.join(dst, ".claude")
    for sub in ("agents", "skills", "hooks"):
        if os.path.isdir(os.path.join(src, sub)):
            shutil.copytree(os.path.join(src, sub), os.path.join(out, sub), ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    os.makedirs(out, exist_ok=True)
    for name in ("settings.json", "settings.local.json.example"):
        if os.path.isfile(os.path.join(src, name)):
            shutil.copy2(os.path.join(src, name), os.path.join(out, name))
    return True


# The harness files an app repository carries (copy_harness above): three directories, and the settings files that wire the hooks.
HARNESS_DIRS = ("agents", "skills", "hooks")
SETTINGS_FILES = ("settings.json", "settings.local.json.example")


def harness_files(claude_dir):
    """{path relative to claude_dir, with '/': absolute path} of the harness files under claude_dir. Caches, local settings and locks are
    not part of the harness."""
    found = {}
    for sub in HARNESS_DIRS:
        for base, dirs, files in os.walk(os.path.join(claude_dir, sub)):
            dirs[:] = sorted(d for d in dirs if d != "__pycache__")
            for name in sorted(files):
                if name.endswith(".pyc"):
                    continue
                path = os.path.join(base, name)
                found[os.path.relpath(path, claude_dir).replace(os.sep, "/")] = path
    for name in SETTINGS_FILES:
        path = os.path.join(claude_dir, name)
        if os.path.isfile(path):
            found[name] = path
    return found


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def compare_harness(sdk, app_dir):
    """[(status, rel)] for every harness file of either side, sorted by rel. status: 'same'; 'differs' (both have it, the bytes differ);
    'missing' (o3-sdk has it, the app does not); 'extra' (only the app has it)."""
    mine = harness_files(os.path.join(sdk, ".claude"))
    theirs = harness_files(os.path.join(app_dir, ".claude"))
    entries = []
    for rel in sorted(set(mine) | set(theirs)):
        if rel not in theirs:
            entries.append(("missing", rel))
        elif rel not in mine:
            entries.append(("extra", rel))
        elif read_bytes(mine[rel]) == read_bytes(theirs[rel]):
            entries.append(("same", rel))
        else:
            entries.append(("differs", rel))
    return entries


def text_diff(sdk, app_dir, rel):
    """The unified diff from the app's file to o3-sdk's, as a list of lines ([] for binary content)."""
    try:
        old = read_bytes(os.path.join(app_dir, ".claude", rel)).decode("utf-8").splitlines(keepends=True)
        new = read_bytes(os.path.join(sdk, ".claude", rel)).decode("utf-8").splitlines(keepends=True)
    except UnicodeDecodeError:
        return []
    return list(difflib.unified_diff(old, new, "app/.claude/" + rel, "o3-sdk/.claude/" + rel))


def count_changes(diff):
    """(lines o3-sdk's version adds, lines it removes) in a unified diff."""
    added = sum(1 for l in diff if l.startswith("+") and not l.startswith("+++"))
    removed = sum(1 for l in diff if l.startswith("-") and not l.startswith("---"))
    return added, removed


def inside(path, root):
    """`path` (symlinks resolved) is `root` or below it; `root` is already resolved."""
    real = os.path.realpath(path)
    return real == root or real.startswith(root + os.sep)


def copy_into(src, dst, claude_real):
    """Copies src over dst through a temporary file, so that a reader never sees half a file. False, nothing written, when dst would land
    outside the app's .claude (a symlinked directory or file in the app's harness)."""
    if os.path.islink(dst) or not inside(dst, claude_real):
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not inside(os.path.dirname(dst), claude_real):
        return False
    tmp = dst + ".o3sdk-new"
    shutil.copyfile(src, tmp)
    shutil.copymode(src, tmp)
    os.replace(tmp, dst)
    return True


def sync_harness(app_dir, sdk, write=False, with_settings=False, show_diff=False, out=None):
    """Reports (and with `write` repairs) the drift of <app_dir>/.claude from sdk's. Returns the exit status: 0 in sync or synchronised,
    1 a refusal or a file that could not be written, 3 differences found and nothing written."""
    out = out or sys.stdout
    app_dir = os.path.abspath(app_dir)
    sdk = os.path.abspath(sdk)
    if not os.path.isdir(app_dir):
        print("new_app.py: %s is not a directory" % app_dir, file=sys.stderr)
        return 1
    if os.path.realpath(app_dir) == os.path.realpath(sdk):
        print("new_app.py: %s is the o3-sdk checkout itself, not an app repository" % app_dir, file=sys.stderr)
        return 1
    if not os.path.isdir(os.path.join(sdk, ".claude")):
        print("new_app.py: no .claude/ in %s: nothing to compare with" % sdk, file=sys.stderr)
        return 1
    has_harness = os.path.isdir(os.path.join(app_dir, ".claude"))
    if not has_harness and not os.path.isfile(os.path.join(app_dir, "CMakeLists.txt")):
        print("new_app.py: %s has no .claude/ and no CMakeLists.txt: not an app repository (a new one is created with "
              "new_app.py <dest-dir> <app-id>)" % app_dir, file=sys.stderr)
        return 1
    entries = compare_harness(sdk, app_dir)
    print("harness of %s compared with %s" % (os.path.join(app_dir, ".claude"), os.path.join(sdk, ".claude")), file=out)
    if not has_harness:
        print("note: the app has no .claude/ at all", file=out)
    tally = {"same": 0, "differs": 0, "missing": 0, "extra": 0}
    pending = []  # what --write copies
    for status, rel in entries:
        tally[status] += 1
        if status == "same":
            continue
        diff = []
        if status == "differs":
            diff = text_diff(sdk, app_dir, rel)
            added, removed = count_changes(diff)
            detail = "o3-sdk's version adds %d and removes %d lines" % (added, removed) if diff else "binary content"
        elif status == "extra":
            detail = "the app's own: never touched"
        else:
            detail = "o3-sdk's file, the app lacks it"
        if status != "extra":
            if rel in SETTINGS_FILES and not with_settings:
                detail += "; settings are copied only with --with-settings"
            else:
                pending.append(rel)
        print("  %-8s %-44s %s" % (status, rel, detail), file=out)
        if show_diff and diff:
            out.writelines(diff if diff[-1].endswith("\n") else diff + ["\n"])
    print("%d files: %d same, %d differ, %d missing, %d extra (the app's own)" %
          (len(entries), tally["same"], tally["differs"], tally["missing"], tally["extra"]), file=out)
    if not tally["differs"] and not tally["missing"]:
        print("in sync", file=out)
        return 0
    if not write:
        print("nothing written: --write copies o3-sdk's version of %d file(s) over the app's and keeps its extras" % len(pending), file=out)
        return 3
    claude_real = os.path.realpath(os.path.join(app_dir, ".claude"))
    status = 0
    for rel in pending:
        if copy_into(os.path.join(sdk, ".claude", rel), os.path.join(app_dir, ".claude", rel), claude_real):
            print("  wrote    %s" % rel, file=out)
        else:
            print("  skipped  %s (it would land outside the app's .claude: a symlink)" % rel, file=out)
            status = 1
    held = [rel for status_, rel in entries if status_ in ("differs", "missing") and rel not in pending]
    if held:
        print("not copied (--with-settings): %s" % ", ".join(held), file=out)
    return status


def instantiate(dest, app_id, name, sdk, harness, git):
    """Creates `dest` (which must not exist). Returns the list of notes to print. Raises ValueError for a refusal."""
    reason = check_id(app_id)
    if reason:
        raise ValueError("app id '%s' %s" % (app_id, reason))
    reason = check_name(name)
    if reason:
        raise ValueError("name '%s' %s" % (name, reason))
    if not os.path.isdir(TEMPLATE):
        raise ValueError("the template is missing: %s" % TEMPLATE)
    if not os.path.isfile(os.path.join(sdk, "cmake", "O3sdkPlatform.cmake")):
        raise ValueError("%s is not an o3-sdk checkout (cmake/O3sdkPlatform.cmake not found)" % sdk)
    dest = os.path.abspath(dest)
    if os.path.lexists(dest):
        raise ValueError("%s exists: new_app.py never overwrites anything" % dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    os.mkdir(dest)  # claims the name; fails if someone created it meanwhile
    try:
        values = {"@APP_ID@": app_id, "@APP_NAME@": name}
        written = copy_tree_substituted(TEMPLATE, dest, values)
        bad = leftover_tokens(written)
        if bad:
            raise RuntimeError("template tokens left after the substitution: %s" % ", ".join("%s (%s)" % b for b in bad[:5]))
        os.makedirs(os.path.join(dest, "tools"), exist_ok=True)
        for tool in ("ci.sh", "remote_ci.sh", "gate.py"):
            shutil.copy2(os.path.join(sdk, "tools", tool), os.path.join(dest, "tools", tool))
        shutil.copy2(os.path.join(sdk, "LICENSE"), os.path.join(dest, "LICENSE"))
        notes = []
        if harness:
            if not copy_harness(sdk, dest):
                notes.append("no .claude/ in %s: the agent harness was not copied" % sdk)
        if git:
            subprocess.run(["git", "init", "-q"], cwd=dest, check=True)
            subprocess.run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], cwd=dest, check=False)
        sibling = os.path.join(os.path.dirname(dest), "o3-sdk")
        if not (os.path.isdir(sibling) and os.path.samefile(sibling, sdk)):
            notes.append("%s is not next to %s: configure with -DO3SDK_DIR=%s (or export O3SDK_DIR)" % (dest, sdk, sdk))
        return notes
    except BaseException:
        shutil.rmtree(dest, ignore_errors=True)  # the directory is ours: this call created it
        raise


def run_sync(app_dir, **kwargs):
    """sync_harness against this checkout with its report captured: (exit status, report text)."""
    sink = io.StringIO()
    with contextlib.redirect_stderr(io.StringIO()):
        status = sync_harness(app_dir, SDK_ROOT, out=sink, **kwargs)
    return status, sink.getvalue()


def run_main(*argv):
    """main() with the output captured: (exit status, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        status = main(list(argv))
    return status, out.getvalue(), err.getvalue()


def self_test_sync(tmp, plain, expect):
    """--sync-harness: a copy in sync, drift reported, --write repairs it and keeps what the app added. `plain` is an app without .claude."""
    app = os.path.join(tmp, "synced")
    instantiate(app, "synced", "SYNCED", SDK_ROOT, True, False)
    status, text = run_sync(app)
    expect(status == 0 and "in sync" in text, "a fresh instance is not in sync: " + text)

    claude = os.path.join(app, ".claude")
    skill = os.path.join(claude, "skills", "fw-build", "SKILL.md")
    agent = os.path.join(claude, "agents", "doc-keeper.md")
    settings = os.path.join(claude, "settings.json")
    own_skill = os.path.join(claude, "skills", "mine", "SKILL.md")
    own_hook = os.path.join(claude, "hooks", "mine.py")
    cache = os.path.join(claude, "hooks", "__pycache__", "mine.cpython-312.pyc")
    with open(skill, "a", encoding="utf-8") as f:
        f.write("a local edit\n")
    with open(settings, "a", encoding="utf-8") as f:
        f.write("\n")
    os.remove(agent)
    os.makedirs(os.path.dirname(own_skill))
    os.makedirs(os.path.dirname(cache))
    for path in (own_skill, own_hook, cache):
        with open(path, "w", encoding="utf-8") as f:
            f.write("the app's own\n")

    status, text = run_sync(app)
    expect(status == 3, "drift: exit %d, expected 3" % status)
    expect(re.search(r"^\s+differs\s+skills/fw-build/SKILL\.md\s+o3-sdk's version adds 0 and removes 1 lines", text, re.M), "differs line: " + text)
    expect(re.search(r"^\s+missing\s+agents/doc-keeper\.md\b", text, re.M), "missing line: " + text)
    expect(re.search(r"^\s+extra\s+skills/mine/SKILL\.md\b", text, re.M), "extra skill: " + text)
    expect(re.search(r"^\s+extra\s+hooks/mine\.py\b", text, re.M), "extra hook: " + text)
    expect(re.search(r"^\s+differs\s+settings\.json\b.*--with-settings", text, re.M), "settings line: " + text)
    expect("__pycache__" not in text and ".pyc" not in text, "a cache file was reported")
    expect("nothing written" in text, "report mode did not say it wrote nothing")
    with open(skill, encoding="utf-8") as f:
        expect(f.read().endswith("a local edit\n"), "report mode changed a file")
    expect(not os.path.exists(agent), "report mode restored a file")

    status, text = run_sync(app, show_diff=True)
    expect(status == 3 and "-a local edit" in text and "--- app/.claude/skills/fw-build/SKILL.md" in text, "--diff: " + text)

    # --write: o3-sdk's versions over the app's, the app's own files untouched, settings held back, no temporary file left.
    status, text = run_sync(app, write=True)
    expect(status == 0, "--write: exit %d: %s" % (status, text))
    expect("wrote    skills/fw-build/SKILL.md" in text and "wrote    agents/doc-keeper.md" in text, "--write report: " + text)
    expect(read_bytes(skill) == read_bytes(os.path.join(SDK_ROOT, ".claude", "skills", "fw-build", "SKILL.md")), "skill not restored")
    expect(read_bytes(agent) == read_bytes(os.path.join(SDK_ROOT, ".claude", "agents", "doc-keeper.md")), "agent not restored")
    for path in (own_skill, own_hook, cache):
        expect(os.path.isfile(path) and read_bytes(path) == b"the app's own\n", "the app's own file was touched: " + path)
    expect(read_bytes(settings) != read_bytes(os.path.join(SDK_ROOT, ".claude", "settings.json")), "settings.json copied without --with-settings")
    expect("not copied (--with-settings): settings.json" in text, "held-back settings not announced")
    expect(not [n for _, _, names in os.walk(claude) for n in names if n.endswith(".o3sdk-new")], "a temporary file was left")
    status, text = run_sync(app)
    drift = re.findall(r"^\s+(differs|missing)\s+(\S+)", text, re.M)
    expect(status == 3 and drift == [("differs", "settings.json")], "after --write only settings.json should differ: " + text)
    status, text = run_sync(app, write=True, with_settings=True)
    expect(status == 0, "--with-settings: exit %d" % status)
    status, text = run_sync(app)
    expect(status == 0 and "in sync" in text and "2 extra (the app's own)" in text, "not in sync after --with-settings: " + text)
    expect(os.path.isfile(own_skill) and os.path.isfile(own_hook), "the app's own files are gone")

    # An app without .claude gets the whole harness with --write; a directory that is no app is refused and left alone.
    status, text = run_sync(plain)
    expect(status == 3 and "the app has no .claude/" in text and "missing" in text, "no harness: " + text)
    status, text = run_sync(plain, write=True)
    expect(status == 0 and os.path.isfile(os.path.join(plain, ".claude", "hooks", "guard_flash.py")), "harness not created: " + text)
    expect(run_sync(plain)[0] == 3, "settings were not held back for a new harness")  # settings.json is still missing
    expect(run_sync(plain, write=True, with_settings=True)[0] == 0 and run_sync(plain)[0] == 0, "a new harness is not complete")
    stranger = os.path.join(tmp, "stranger")
    os.mkdir(stranger)
    expect(run_sync(stranger, write=True)[0] == 1 and os.listdir(stranger) == [], "a directory that is no app was written to")
    expect(run_sync(os.path.join(tmp, "nowhere"))[0] == 1, "a missing directory was accepted")
    expect(run_sync(SDK_ROOT, write=True)[0] == 1, "o3-sdk itself was accepted as an app")

    # A harness directory that is a symlink out of the app is not written through.
    linked = os.path.join(tmp, "linked")
    instantiate(linked, "linked", "LINKED", SDK_ROOT, True, False)
    outside = os.path.join(tmp, "outside_hooks")
    shutil.copytree(os.path.join(linked, ".claude", "hooks"), outside)
    shutil.rmtree(os.path.join(linked, ".claude", "hooks"))
    os.symlink(outside, os.path.join(linked, ".claude", "hooks"))
    shared = os.path.join(outside, "verify_gate.py")
    with open(shared, "a", encoding="utf-8") as f:
        f.write("# shared with another repository\n")
    before = read_bytes(shared)
    status, text = run_sync(linked, write=True)
    expect(status == 1 and "skipped  hooks/verify_gate.py" in text and read_bytes(shared) == before, "wrote through a symlink: " + text)

    # The command line: exit codes and the combinations it refuses.
    cli = os.path.join(tmp, "cli")
    instantiate(cli, "cli", "CLI", SDK_ROOT, True, False)
    os.remove(os.path.join(cli, ".claude", "agents", "dsp-scout.md"))
    expect(run_main("--sync-harness", cli)[0] == 3, "cli: report exit")
    expect(run_main("--sync-harness", cli, "--diff")[0] == 3, "cli: --diff exit")
    expect(run_main("--sync-harness", cli, "--write")[0] == 0, "cli: --write exit")
    status, out, _ = run_main("--sync-harness", cli)
    expect(status == 0 and "in sync" in out, "cli: in sync exit")
    for bad in (("--sync-harness", cli, "dest", "id"), ("--sync-harness", cli, "--with-settings"), ("--sync-harness", cli, "--no-harness"),
                ("--write",), ("--diff", "somewhere", "id")):
        status, _, err = run_main(*bad)
        expect(status == 1 and err != "", "cli: %s not refused (exit %d)" % (" ".join(bad), status))


def self_test():
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    for good in ("tracker", "my_synth", "acid2", "ab", "a" * 24):
        expect(check_id(good) is None, "good id refused: " + good)
    for bad in ("", "a", "A", "1abc", "my-synth", "my synth", "a__b", "ab_", "a" * 25, "class", "int", "core", "app", "o3sdk", "test", "all",
                "hello", "ünï", "../x", "x/y"):
        expect(check_id(bad) is not None, "bad id accepted: " + repr(bad))
    for good in ("TRACKER", "MY-SYNTH", "O3 303", "A"):
        expect(check_name(good) is None, "good name refused: " + good)
    for bad in ("", "lower", "-X", "A" * 17, 'A"B', "A\\B", "A\nB", "A;B"):
        expect(check_name(bad) is not None, "bad name accepted: " + repr(bad))
    expect(default_name("my_synth") == "MY-SYNTH", "default name")

    with tempfile.TemporaryDirectory(prefix="new_app_selftest_") as tmp:
        dest = os.path.join(tmp, "work", "demo_app")  # the parent does not exist yet
        notes = instantiate(dest, "demo_app", "DEMO-APP", SDK_ROOT, True, False)
        expect(isinstance(notes, list), "notes")
        for rel in ("CMakeLists.txt", "README.md", "CLAUDE.md", "AGENTS.md", ".gitignore", "LICENSE", "app/CMakeLists.txt",
                    "app/src/app.cpp", "app/src/play_screen.cpp", "app/include/demo_app/pad_map.h", "app/scenarios/boot.txt",
                    "units/voice/include/demo_app/units/voice/voice.h", "units/voice/src/voice.cpp", "tests/app_test.cpp",
                    "tools/ci.sh", "tools/remote_ci.sh", "tools/gate.py", ".claude/settings.json", ".claude/hooks/guard_flash.py"):
            expect(os.path.exists(os.path.join(dest, rel)), "missing: " + rel)
        expect(os.access(os.path.join(dest, "tools", "ci.sh"), os.X_OK), "ci.sh lost its mode")
        with open(os.path.join(dest, "app", "src", "app.cpp"), encoding="utf-8") as f:
            text = f.read()
        expect('"DEMO-APP"' in text and "demo_app::Voice" in text, "substitution did not reach app.cpp")
        expect(not leftover_tokens([os.path.join(base, n) for base, _, names in os.walk(dest) for n in names
                                    if ".claude" not in base]), "tokens left")
        # Never overwrite: not a directory with files, not an empty one, not a file, not a dangling symlink.
        marker = os.path.join(dest, "keep.txt")
        with open(marker, "w") as f:
            f.write("mine")
        for target in (dest, os.path.join(tmp, "empty"), os.path.join(tmp, "file"), os.path.join(tmp, "dangling")):
            if target.endswith("empty"):
                os.mkdir(target)
            elif target.endswith("file"):
                open(target, "w").close()
            elif target.endswith("dangling"):
                os.symlink(os.path.join(tmp, "nowhere"), target)
            try:
                instantiate(target, "demo_app", "DEMO-APP", SDK_ROOT, False, False)
                failures.append("overwrote " + target)
            except ValueError:
                pass
        expect(os.path.isfile(marker), "the existing directory was touched")
        # A refused id creates nothing.
        refused = os.path.join(tmp, "refused")
        try:
            instantiate(refused, "class", "X", SDK_ROOT, False, False)
            failures.append("keyword id accepted")
        except ValueError:
            pass
        expect(not os.path.exists(refused), "a refused id left a directory")
        # --no-harness leaves out .claude.
        plain = os.path.join(tmp, "plain")
        instantiate(plain, "plain", "PLAIN", SDK_ROOT, False, False)
        expect(not os.path.exists(os.path.join(plain, ".claude")), "--no-harness copied .claude")
        self_test_sync(tmp, plain, expect)
    for f in failures:
        print("new_app.py self-test FAILED: " + f, file=sys.stderr)
    if not failures:
        print("new_app.py self-test ok")
    return 0 if not failures else 1


def main(argv):
    parser = argparse.ArgumentParser(prog="new_app.py", description="Instantiate templates/app into a new app repository.")
    parser.add_argument("dest", nargs="?", help="the new repository's directory (must not exist)")
    parser.add_argument("app_id", nargs="?", help="the app's identifier, e.g. mysynth")
    parser.add_argument("--name", help="the name the screen shows (default: the id in upper case)")
    parser.add_argument("--sdk-dir", default=SDK_ROOT, help="the o3-sdk checkout to take LICENSE, tools and the harness from, or to compare with (default: this one)")
    parser.add_argument("--no-harness", action="store_true", help="do not copy .claude/ (agents, skills, hooks)")
    parser.add_argument("--git", action="store_true", help="run git init in the new repository (no commit)")
    parser.add_argument("--sync-harness", metavar="APP_DIR",
                        help="compare the .claude harness of an existing app repository with this checkout's and report the differences")
    parser.add_argument("--write", action="store_true", help="--sync-harness: copy o3-sdk's version of every differing or missing file (never deletes)")
    parser.add_argument("--with-settings", action="store_true",
                        help="--sync-harness --write: also copy settings.json and settings.local.json.example (hook wiring, permissions)")
    parser.add_argument("--diff", action="store_true", help="--sync-harness: print the unified diff of each differing file")
    parser.add_argument("--self-test", action="store_true", help="run the checks of this tool and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.sync_harness is not None:
        if args.dest or args.app_id or args.name or args.no_harness or args.git:
            print("new_app.py: --sync-harness takes the app directory only (no <dest-dir> <app-id>, --name, --no-harness or --git)", file=sys.stderr)
            return 1
        if args.with_settings and not args.write:
            print("new_app.py: --with-settings needs --write (without it nothing is copied)", file=sys.stderr)
            return 1
        return sync_harness(args.sync_harness, os.path.abspath(args.sdk_dir), args.write, args.with_settings, args.diff)
    if args.write or args.with_settings or args.diff:
        print("new_app.py: --write, --with-settings and --diff belong to --sync-harness", file=sys.stderr)
        return 1
    if not args.dest or not args.app_id:
        parser.print_usage(sys.stderr)
        return 1
    name = args.name if args.name is not None else default_name(args.app_id)
    try:
        notes = instantiate(args.dest, args.app_id, name, os.path.abspath(args.sdk_dir), not args.no_harness, args.git)
    except ValueError as err:
        print("new_app.py: " + str(err), file=sys.stderr)
        return 1
    except RuntimeError as err:
        print("new_app.py: " + str(err), file=sys.stderr)
        return 2
    dest = os.path.abspath(args.dest)
    print("created %s (app %s, shown as %s)" % (dest, args.app_id, name))
    for note in notes:
        print("note: " + note)
    print("next:")
    print("  cd %s" % dest)
    if any("O3SDK_DIR" in note for note in notes):
        print("  export O3SDK_DIR=%s" % os.path.abspath(args.sdk_dir))
    print("  cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure")
    print("  mkdir -p /tmp/%s && build-host/app/%s_sim --scenario app/scenarios/boot.txt --out /tmp/%s" % (args.app_id, args.app_id, args.app_id))
    print("  cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir> && cmake --build build-mk66 -j4")
    print("  then %s/docs/NEW-APP.md: the units, the screens, the gate, the install" % os.path.abspath(args.sdk_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
