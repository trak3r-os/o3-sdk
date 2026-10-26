#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gate.py: the mechanical half of the release gate (skill fw-gate-check) for one app's firmware image.

    python3 -I tools/gate.py <app> [--arm DIR] [--sdk DIR] [--prefix build-gate] [--jobs N] [--host-build DIR | --skip-host]
    python3 -I tools/gate.py --self-test

Run it from the repository that holds the app: o3-sdk itself (the app is apps/<app>) or an app repository (its app/ directory; the kit is
--sdk, $O3SDK_DIR or ../o3-sdk, as for tools/ci.sh). The same file is in o3-sdk and in an app repository. It:

  1. records the identity of both trees (git: the commit, tracked changes, untracked files) and the toolchain;
  2. builds the image twice from scratch, into <prefix>-a and <prefix>-b (only a directory of that name is ever deleted), with identical options;
  3. checks: no warning in either build (as tools/ci.sh counts them), no linker overflow or undefined reference, the two HEX files identical,
     every image ctest of the app (`ctest -L mk66`: <app>_image, _flashconfig, _ramfunc, _usb), no software double (__aeabi_d) in the image
     or the archives, and the full host ctest when given the host build directory of the same code (--host-build) or --skip-host;
  4. writes <prefix>-a/CANDIDATE.md: the identity, every check with PASS or FAIL, the region usage of the link, the sizes and SHA-256 of
     the .hex, .ptf, .bin and .elf, what the host cannot prove, and the line "Not flashed. A unit test needs the maintainer's go."

Verdicts: CANDIDATE (everything passed and both trees are clean and committed: the image can be traced to a source), BENCH BUILD
(everything passed but a tree has changed tracked files or no commit: the image says "-dirty" or "unknown"; ask the maintainer to commit
first for a real candidate), FAIL. The second pass of the skill (a review of the manifest against the raw output) and the maintainer's
decision are not this script's. It never flashes, never writes to a card, never touches a unit and writes nothing outside the two build
directories. Exit status: 0 CANDIDATE or BENCH BUILD, 1 FAIL, 2 usage.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WARNING_RE = re.compile(r"warning[:(]|CMake (Deprecation )?Warning", re.IGNORECASE)  # tools/ci.sh counts the same
LINK_RE = re.compile(r"region `?\S+'? overflowed|undefined reference|cannot find -l|multiple definition", re.IGNORECASE)
REGION_RE = re.compile(r"^\s*(FLASH|SRAM_L|SRAM_U|SDRAM):\s+(\d+ [KM]?B)\s+(\d+ [KM]?B)\s+([\d.]+%)\s*$")
DEFAULT_ARM = os.path.join(ROOT, "..", "track3r-os", ".toolchains", "arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi")
CANNOT_PROVE = [
    "that the image boots at all on this unit, the clock tree and the SDRAM bring-up (it is track3r-os's, proven there)",
    "the screen by eye, real presses of pads, buttons and the encoder, the pad LEDs by eye, the sound on the analogue outputs by ear",
    "the render's CPU cost of anything new (the header strip and `o3console.py cpu` on the unit), a long soak, a provoked crash and its record",
    "anything the capability map (docs/FRAMEWORK.md section 10) lists as host only or planned",
]


class Gate(Exception):
    pass


def run(cmd, cwd=None, log=None):
    """Runs a command; returns (rc, output). The output is also appended to `log` when given."""
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if log:
        with open(log, "a", encoding="utf-8") as f:
            f.write("$ %s\n%s\n[rc=%d]\n" % (" ".join(cmd), proc.stdout, proc.returncode))
    return proc.returncode, proc.stdout


def git_state(tree):
    """{'commit': 12 hex digits or None, 'tracked': changed tracked files, 'untracked': files git does not track}."""
    rc, out = run(["git", "-C", tree, "rev-parse", "--short=12", "HEAD"])
    commit = out.strip() if rc == 0 else None
    rc, out = run(["git", "-C", tree, "status", "--porcelain", "--untracked-files=no"])
    tracked = [l for l in out.splitlines() if l.strip()] if rc == 0 else ["(not a git checkout)"]
    rc, out = run(["git", "-C", tree, "status", "--porcelain", "--untracked-files=normal"])
    untracked = [l[3:] for l in out.splitlines() if l.startswith("?? ")] if rc == 0 else []
    return {"commit": commit, "tracked": tracked, "untracked": untracked}


def traceable(state):
    return state["commit"] is not None and not state["tracked"]


def image_label(bin_path):
    """The build identity string in the image ("<app label> / o3sdk <kit label>"), from the console's banner; None when not found."""
    try:
        with open(bin_path, "rb") as f:
            m = re.search(rb"Track3r diag - (.{1,120}?) - 'help'", f.read())
    except OSError:
        return None
    return m.group(1).decode("ascii", "replace") if m else None


def verdict_of(checks, trees):
    """CANDIDATE: nothing failed and every tree is clean and committed. BENCH BUILD: nothing failed but an image would say -dirty or unknown."""
    if any(c[1] == "FAIL" for c in checks):
        return "FAIL"
    return "CANDIDATE" if all(traceable(st) for _, st in trees) else "BENCH BUILD"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def regions_of(build_log, app):
    """The region table the linker printed for <app>.elf: [(region, used, size, percent)], or [] when it cannot be told.

    ld prints the table without naming the image, right after the `Linking CXX executable <app>.elf` line. With two links running at once
    (a tree with several apps built with -j) their lines interleave and the table cannot be attributed: then [] (the manifest still has `size`)."""
    chunk, starts = [], False
    chunks = []
    for line in build_log.splitlines():
        if re.search(r"Linking CXX executable (\S*/)?(\S+)\.elf", line):
            if starts:
                chunks.append(chunk)
            name = re.search(r"Linking CXX executable (?:\S*/)?(\S+)\.elf", line).group(1)
            chunk, starts = [name], True
        elif starts:
            chunk.append(line)
    if starts:
        chunks.append(chunk)
    for c in chunks:
        if c[0] != app:
            continue
        rows = [m.groups() for m in (REGION_RE.match(l) for l in c[1:]) if m]
        if len(rows) == 4:
            return rows
    return []


def format_manifest(info):
    """CANDIDATE.md as text from the facts of a run (a dict built by main), so that a self-test can check it."""
    lines = ["# Candidate image: %s" % info["app"], "",
             "Verdict: **%s**" % info["verdict"], "",
             "Not flashed. A unit test needs the maintainer's go.", "",
             "## Identity", ""]
    for name, state in info["trees"]:
        lines.append("- %s: commit `%s`; %s; %d untracked file(s)" % (
            name, state["commit"] or "none (unknown)",
            "clean" if not state["tracked"] else "changed tracked files (the image says -dirty): " + ", ".join(
                l.strip() for l in state["tracked"][:6]),
            len(state["untracked"])))
    lines += ["- image label (what `version` prints after `build` on the unit; `--expect-version` takes a part of it): `%s`" % (info.get("label") or "(not found in the .bin)"),
              "- toolchain: %s" % info["toolchain"], "- options: `-DO3SDK_PLATFORM=mk66` (the defaults of the tree otherwise: O3SDK_RATE_44100, O3SDK_MK66_CONSOLE)",
              "- built: %s" % info["when"], "", "## Checks", "", "| Check | Result | Detail |", "|---|---|---|"]
    for name, ok, detail in info["checks"]:
        lines.append("| %s | %s | %s |" % (name.replace("|", "/"), ok, detail.replace("|", "/").replace("\n", " ")))
    lines += ["", "## The image", ""]
    for name, digest, size in info["files"]:
        lines.append("- `%s`: %d bytes, SHA-256 `%s`" % (name, size, digest))
    if info.get("size"):
        lines += ["", "`arm-none-eabi-size` of the ELF: %s (flash is text + data)" % info["size"]]
    if info["regions"]:
        lines += ["", "Link region usage:", "", "| Region | Used | Size | Used % |", "|---|---|---|---|"]
        lines += ["| %s | %s | %s | %s |" % r for r in info["regions"]]
    else:
        lines += ["", "Link region usage: not attributable from the build log (two images linked at once); `arm-none-eabi-size -A %s.elf` and the `.map` have the sections." % info["app"]]
    lines += ["", "## What the host cannot prove (the unit must show it)", ""] + ["- " + t for t in CANNOT_PROVE]
    lines += ["", "## Hand-over", "",
              "The deliverable is `%s.ptf` (the checked HEX under that name). The maintainer installs it: over the USB console (`o3console.py --put`, then `--update`, "
              "docs/NEW-APP.md section 11) or by card (the `.ptf` alone in `Firmware/`, then the hidden reset button). "
              "A second pass over this manifest against the raw logs (`gate-*.log` in the build directories) is the skill's last step." % info["app"],
              "", "Not flashed. A unit test needs the maintainer's go.", ""]
    return "\n".join(lines)


def build(root, kind, sdk, arm, jobs, app, tag, prefix):
    """One fresh build. Returns (dir, hex path or None, configure+build log text, [problems])."""
    d = os.path.join(root, "%s-%s" % (prefix, tag))
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    log = os.path.join(d, "gate-%s.log" % tag)
    problems = []
    cfg = ["cmake", "-S", root, "-B", d, "-DO3SDK_PLATFORM=mk66", "-DARM_GNU_ROOT=" + arm]
    if kind == "app":
        cfg.append("-DO3SDK_DIR=" + sdk)
    text = ""
    rc, out = run(cfg, log=log)
    text += out
    if rc != 0:
        problems.append("configure failed (rc=%d), see %s" % (rc, log))
        return d, None, text, problems
    rc, out = run(["cmake", "--build", d, "-j", str(jobs)], log=log)
    text += out
    if rc != 0:
        problems.append("build failed (rc=%d), see %s" % (rc, log))
        return d, None, text, problems
    hexes = [p for p in glob.glob(os.path.join(d, "**", app + ".hex"), recursive=True) if "CMakeFiles" not in p]
    if len(hexes) != 1:
        problems.append("expected one %s.hex in %s, found %d" % (app, d, len(hexes)))
        return d, None, text, problems
    return d, hexes[0], text, problems


def main(argv):
    parser = argparse.ArgumentParser(prog="gate.py", description="The mechanical half of the release gate for one app's image.")
    parser.add_argument("app", nargs="?", help="the app's name (hello, o3_303, myapp): the target <app>, its image <app>.ptf")
    parser.add_argument("--arm", default=os.environ.get("ARM_GNU_ROOT") or DEFAULT_ARM, help="the Arm GNU Toolchain directory (holds bin/)")
    parser.add_argument("--sdk", default=os.environ.get("O3SDK_DIR") or os.path.join(ROOT, "..", "o3-sdk"), help="an app repository: the o3-sdk checkout")
    parser.add_argument("--prefix", default="build-gate", help="build directory prefix: <prefix>-a and <prefix>-b (must start with build-gate)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--host-build", help="a host build directory of the same code: its full ctest is part of the gate")
    parser.add_argument("--skip-host", action="store_true", help="do not require the host ctest (say so in the manifest)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.app or not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", args.app):
        parser.print_usage(sys.stderr)
        return 2
    if not re.match(r"^build-gate[A-Za-z0-9_.-]*$", args.prefix):
        print("gate.py: --prefix must start with build-gate (only such directories are deleted)", file=sys.stderr)
        return 2
    if bool(args.host_build) == bool(args.skip_host):
        print("gate.py: give --host-build <dir> (the host build of this code, its ctest runs) or --skip-host", file=sys.stderr)
        return 2
    try:
        os.nice(10)
    except OSError:
        pass

    is_sdk = os.path.isfile(os.path.join(ROOT, "cmake", "O3sdkPlatform.cmake"))
    kind = "sdk" if is_sdk else "app"
    sdk = ROOT if is_sdk else os.path.abspath(args.sdk)
    if not os.path.isfile(os.path.join(sdk, "cmake", "O3sdkPlatform.cmake")):
        print("gate.py: %s is not an o3-sdk checkout (set --sdk or O3SDK_DIR)" % sdk, file=sys.stderr)
        return 2
    arm = os.path.abspath(args.arm)
    gcc = os.path.join(arm, "bin", "arm-none-eabi-gcc")
    if not os.access(gcc, os.X_OK):
        print("gate.py: no arm-none-eabi-gcc in %s/bin (--arm or ARM_GNU_ROOT: the Arm GNU Toolchain 15.3.Rel1 directory)" % arm, file=sys.stderr)
        return 2
    toolchain = run([gcc, "--version"])[1].splitlines()[0]
    nm = os.path.join(arm, "bin", "arm-none-eabi-nm")
    size_tool = os.path.join(arm, "bin", "arm-none-eabi-size")

    checks = []  # (name, "PASS"|"FAIL"|"NOT RUN", detail)

    def record(name, ok, detail=""):
        checks.append((name, "PASS" if ok else "FAIL", detail))
        print("  %-4s %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail and not ok else ""), flush=True)

    trees = [(os.path.basename(ROOT) if is_sdk else "app (%s)" % os.path.basename(ROOT), git_state(ROOT))]
    if not is_sdk:
        trees.append(("kit (o3-sdk)", git_state(sdk)))
    print("gate.py: %s, %s" % (args.app, toolchain), flush=True)
    for name, st in trees:
        print("  %s: %s, %s, %d untracked" % (name, st["commit"] or "no commit", "clean" if not st["tracked"] else "CHANGED tracked files",
                                              len(st["untracked"])), flush=True)

    started = time.time()
    dirs = {}
    hexes = {}
    logs = {}
    for tag in ("a", "b"):
        print("  building %s-%s from scratch ..." % (args.prefix, tag), flush=True)
        d, hx, text, problems = build(ROOT, kind, sdk, arm, args.jobs, args.app, tag, args.prefix)
        dirs[tag], hexes[tag], logs[tag] = d, hx, text
        warnings = [l for l in text.splitlines() if WARNING_RE.search(l)]
        record("build %s: configure and build exit 0, image found" % tag, not problems, "; ".join(problems))
        record("build %s: no warnings" % tag, not warnings, "%d, the first: %s" % (len(warnings), warnings[0].strip()) if warnings else "")
        link = [l for l in text.splitlines() if LINK_RE.search(l)]
        record("build %s: no region overflow, undefined reference or duplicate symbol" % tag, not link, link[0].strip() if link else "")

    if hexes["a"] and hexes["b"]:
        same = open(hexes["a"], "rb").read() == open(hexes["b"], "rb").read()
        record("the two HEX files are identical (reproducible build)", same, "")
    else:
        record("the two HEX files are identical (reproducible build)", False, "a build did not produce its HEX")

    files, regions, label, size_line = [], [], None, ""
    if hexes["a"]:
        da = dirs["a"]
        rc, out = run(["ctest", "--test-dir", da, "-L", "mk66", "--output-on-failure"], log=os.path.join(da, "gate-a.log"))
        tests = re.findall(r"Test\s+#\d+: (%s_\w+) \.+\s+(Passed|\*\*\*Failed|\*\*\*Exception)" % re.escape(args.app), out)
        names = {n for n, _ in tests}
        want = {"%s_image" % args.app, "%s_flashconfig" % args.app, "%s_ramfunc" % args.app, "%s_usb" % args.app}
        ok = rc == 0 and "100% tests passed" in out and want <= names and all(r == "Passed" for _, r in tests)
        record("ctest -L mk66: %s's image checks and the rest of the label pass" % args.app, ok,
               "missing: %s" % ", ".join(sorted(want - names)) if want - names else "rc=%d" % rc)

        elf = os.path.join(os.path.dirname(hexes["a"]), args.app + ".elf")
        doubles = [l for l in run([nm, elf])[1].splitlines() if " __aeabi_d" in l]
        archives = [p for p in glob.glob(os.path.join(da, "**", "*.a"), recursive=True) if "CMakeFiles" not in p]
        refs = []
        for a in archives:
            refs += [l for l in run([nm, "-A", "-u", a])[1].splitlines() if "__aeabi_d" in l]
        record("no software double (__aeabi_d) in the image or the archives", not doubles and not refs,
               "%d linked, %d undefined references" % (len(doubles), len(refs)))

        regions = regions_of(logs["a"], args.app)
        size_rows = run([size_tool, elf])[1].splitlines()
        size_line = " ".join(size_rows[1].split()[:3]) if len(size_rows) > 1 else ""
        size_line = "text, data, bss = %s" % size_line.replace(" ", ", ") if size_line else ""
        print("  link: " + (", ".join("%s %s (%s)" % (r[0], r[1], r[3]) for r in regions) if regions else "region table not attributable (see the size line)")
              + ("; " + size_line if size_line else ""), flush=True)
        for ext in ("hex", "ptf", "bin", "elf"):
            path = os.path.join(os.path.dirname(hexes["a"]), "%s.%s" % (args.app, ext))
            if os.path.isfile(path):
                files.append((os.path.relpath(path, ROOT), sha256(path), os.path.getsize(path)))
        record("the image files exist (.hex .ptf .bin .elf)", len(files) == 4, "found %d" % len(files))
        label = image_label(os.path.join(os.path.dirname(hexes["a"]), args.app + ".bin"))
    else:
        for name in ("ctest -L mk66", "no software double", "the image files exist"):
            record(name, False, "no image was built")

    if args.host_build:
        rc, out = run(["ctest", "--test-dir", os.path.abspath(args.host_build), "--output-on-failure", "-j", str(args.jobs)])
        summary = re.search(r"100% tests passed(, 0 tests failed)? out of (\d+)", out)  # the wording differs between CMake versions
        record("the full host ctest (%s) passes" % args.host_build, rc == 0 and bool(summary),
               summary.group(0) if summary else "no '100%% tests passed' line (rc=%d)" % rc)
    else:
        checks.append(("the full host ctest", "NOT RUN", "--skip-host: run `tools/ci.sh host` on this code before the maintainer sees the candidate"))
        print("  SKIP the full host ctest (--skip-host)", flush=True)

    failed = [c for c in checks if c[1] == "FAIL"]
    verdict = verdict_of(checks, trees)
    manifest = os.path.join(dirs["a"], "CANDIDATE.md")
    with open(manifest, "w", encoding="utf-8") as f:
        f.write(format_manifest({"app": args.app, "verdict": verdict, "trees": trees, "toolchain": toolchain, "checks": checks,
                                 "files": files, "regions": regions, "label": label, "size": size_line, "when": time.strftime("%Y-%m-%d %H:%M", time.gmtime(started)) + " UTC"}))
    print("gate.py: %s (%d check(s), %d failed); manifest %s" % (verdict, len(checks), len(failed), os.path.relpath(manifest, ROOT)))
    if verdict == "BENCH BUILD":
        print("gate.py: a tree has changed tracked files or no commit: the image says -dirty or unknown, so it cannot be traced. "
              "Ask the maintainer to commit first for a real candidate (this tool does not commit).")
    print("gate.py: not flashed. A unit test needs the maintainer's go.")
    return 1 if failed else 0


def self_test():
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    log = """[ 99%] Linking CXX executable other.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:       111111 B         1 MB     10.00%
          SRAM_L:        1476 B        64 KB      2.25%
          SRAM_U:       77616 B       192 KB     39.48%
           SDRAM:         64 KB        16 MB      0.39%
[ 99%] Built target other
[ 99%] Linking CXX executable build/app/demo.elf
/x/ld: demo.elf: section `.bss' can't be allocated in segment 3
LOAD: .data .bss
Memory region         Used Size  Region Size  %age Used
           FLASH:      148776 B         1 MB     14.19%
          SRAM_L:        1476 B        64 KB      2.25%
          SRAM_U:       77616 B       192 KB     39.48%
           SDRAM:         64 KB        16 MB      0.39%
[ 99%] Built target demo
"""
    rows = regions_of(log, "demo")
    expect(len(rows) == 4 and rows[0] == ("FLASH", "148776 B", "1 MB", "14.19%"), "region table of demo: %r" % (rows,))
    other = regions_of(log, "other")
    expect(len(other) == 4 and other[0][1] == "111111 B", "region table of another image")
    expect(regions_of(log, "absent") == [], "no table for an absent image")
    interleaved = """[ 98%] Linking CXX executable a.elf
[ 98%] Linking CXX executable b.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:       111111 B         1 MB     10.00%
          SRAM_L:        1476 B        64 KB      2.25%
          SRAM_U:       77616 B       192 KB     39.48%
           SDRAM:         64 KB        16 MB      0.39%
Memory region         Used Size  Region Size  %age Used
           FLASH:       222222 B         1 MB     20.00%
          SRAM_L:        1476 B        64 KB      2.25%
          SRAM_U:       77616 B       192 KB     39.48%
           SDRAM:         64 KB        16 MB      0.39%
"""
    expect(regions_of(interleaved, "a") == [] and regions_of(interleaved, "b") == [], "interleaved links are not attributed")
    expect(WARNING_RE.search("foo.cpp:3:4: warning: unused") and WARNING_RE.search("CMake Warning at x") and
           not WARNING_RE.search("section `.bss' can't be allocated in segment 3"), "warning pattern")
    expect(LINK_RE.search("ld: region `SRAM_U' overflowed by 12 bytes") and LINK_RE.search("undefined reference to `x'") and
           not LINK_RE.search("Memory region Used Size"), "link pattern")
    clean = {"commit": "abc123abc123", "tracked": [], "untracked": []}
    dirty = {"commit": "abc123abc123", "tracked": [" M a.cpp"], "untracked": ["x"]}
    nocommit = {"commit": None, "tracked": [], "untracked": []}
    expect(traceable(clean) and not traceable(dirty) and not traceable(nocommit), "traceable")
    ok_checks = [("a", "PASS", ""), ("host", "NOT RUN", "")]
    expect(verdict_of(ok_checks, [("app", clean), ("kit", clean)]) == "CANDIDATE", "verdict: candidate")
    expect(verdict_of(ok_checks, [("app", clean), ("kit", dirty)]) == "BENCH BUILD", "verdict: a dirty kit is a bench build")
    expect(verdict_of(ok_checks, [("app", nocommit)]) == "BENCH BUILD", "verdict: no commit is a bench build")
    expect(verdict_of(ok_checks + [("b", "FAIL", "")], [("app", clean)]) == "FAIL", "verdict: a failed check fails")
    text = format_manifest({"app": "demo", "verdict": "BENCH BUILD", "trees": [("app", dirty), ("kit", nocommit)], "toolchain": "arm-none-eabi-gcc 15.3.1",
                            "checks": [("a", "PASS", ""), ("b | c", "FAIL", "x\ny")], "files": [("build-gate-a/app/demo.ptf", "0" * 64, 10)],
                            "regions": rows, "when": "2026-01-01 00:00 UTC"})
    expect("Verdict: **BENCH BUILD**" in text and "Not flashed. A unit test needs the maintainer's go." in text and "| b / c | FAIL | x y |" in text
           and "unknown" in text and "-dirty" in text and "SHA-256 `%s`" % ("0" * 64) in text, "manifest content")
    expect(text.count("Not flashed. A unit test needs the maintainer's go.") == 2, "the line twice (top and end)")
    with tempfile.TemporaryDirectory(prefix="gate_selftest_") as tmp:
        p = os.path.join(tmp, "f")
        with open(p, "wb") as f:
            f.write(b"abc")
        expect(sha256(p) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256")
        with open(p, "wb") as f:
            f.write(b"\0\0Track3r diag - d12250e84e65 / o3sdk 48d87ef98e43-dirty - 'help' lists the commands\0")
        expect(image_label(p) == "d12250e84e65 / o3sdk 48d87ef98e43-dirty", "image label")
        expect(image_label(os.path.join(tmp, "absent")) is None, "no image label without a file")
        subprocess.run(["git", "init", "-q"], cwd=tmp, check=True)
        st = git_state(tmp)
        expect(st["commit"] is None and st["tracked"] == [] and st["untracked"] == ["f"], "git state of a fresh repository: %r" % (st,))
    for f in failures:
        print("gate.py self-test FAILED: " + f, file=sys.stderr)
    if not failures:
        print("gate.py self-test ok")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
