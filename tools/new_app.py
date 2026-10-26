#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""new_app.py: instantiate templates/app into a new app repository that builds against this o3-sdk checkout.

    python3 -I tools/new_app.py <dest-dir> <app-id> [--name NAME] [--sdk-dir DIR] [--no-harness] [--git]
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

Python 3 standard library only. Exit status: 0 done, 1 a usage error or a refused id/name/destination, 2 the template is broken.
"""
import argparse
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
    parser.add_argument("--sdk-dir", default=SDK_ROOT, help="the o3-sdk checkout to take LICENSE, tools and the harness from (default: this one)")
    parser.add_argument("--no-harness", action="store_true", help="do not copy .claude/ (agents, skills, hooks)")
    parser.add_argument("--git", action="store_true", help="run git init in the new repository (no commit)")
    parser.add_argument("--self-test", action="store_true", help="run the checks of this tool and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
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
