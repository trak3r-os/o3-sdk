#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""digest_check.py: pin the frames an app draws.

    digest_check.py --sim <app>_sim --pins tests/digests/<app>.txt [--skip KEY]... --run 'NAME|SCENARIO|ARGS' [--run ...]
    digest_check.py --pins FILE --have DIGESTS.TXT [--have ...]            (digests a caller already has)
    ... --update                                                            (rewrite the pin file instead of comparing)
    digest_check.py --self-test

The simulator writes <out>/digests.txt: one line per picture a scenario took, `<snap> <digest>`, where the digest is the FNV-1a (32 bit) of the
display-list words of that frame (sim/include/o3sdk/sim/eve_raster.h). Words, not pixels: they are what a change of the kit must not move. A pin
file holds `<run>/<snap> <digest>` lines (`#` starts a comment). A change that moves a digest fails this check until the pin file is updated
(--update) and the new file is reviewed with the change that caused it.

--run: NAME names the run (it prefixes the keys), SCENARIO is a scenario file and ARGS are more simulator options, split on spaces; {out} is the run's
output directory, {card} a fresh directory for --card and {wav} a file in {out}. A run fails when the simulator exits non-zero or says that the
app's bitmap handle 0 was used (kit apps never use it: ui/plot/canvas.h, EveRaster::handle_app_uses()). --skip leaves a key out of the
comparison and out of the pin file: for a frame that shows a measured number that differs between machines.

Exit status: 0 the digests are the pinned ones (or the file was rewritten), 1 they differ or a run failed, 2 a usage error.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile

LINE = re.compile(r"^(\S+) ([0-9a-f]{8})$")
HEADER = ("# Pinned frame digests (FNV-1a over the display-list words of each picture a scenario takes): tools/digest_check.py compares them.\n"
          "# A change that moves one fails its ctest until this file is regenerated (digest_check.py ... --update) and reviewed.\n")


class Failure(Exception):
    pass


def parse_digests(text, prefix=""):
    """`<name> <digest>` lines (digests.txt) as an ordered list of (prefix + name, digest)."""
    out = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = LINE.match(line)
        if not m:
            raise Failure("line %d is not '<name> <8 hex digits>': %r" % (number, raw))
        out.append((prefix + m.group(1), m.group(2)))
    return out


def compare(pinned, have):
    """The differences between two lists of (key, digest): a list of text lines, empty when they are the same."""
    problems = []
    want = dict(pinned)
    got = dict(have)
    for kind, pairs in (("pinned", pinned), ("drawn", have)):
        keys = [k for k, _ in pairs]
        for k in sorted({k for k in keys if keys.count(k) > 1}):
            problems.append("%s: the key %s occurs more than once" % (kind, k))
    for k, d in have:
        if k not in want:
            problems.append("new frame, not pinned: %s %s" % (k, d))
        elif want[k] != d:
            problems.append("moved: %s was %s, is %s" % (k, want[k], d))
    for k, d in pinned:
        if k not in got:
            problems.append("pinned frame not drawn: %s %s" % (k, d))
    return problems


def render_pins(have):
    return HEADER + "".join("%s %s\n" % (k, d) for k, d in have)


def run_one(sim, spec, workdir):
    name, scenario, args = (spec.split("|", 2) + ["", ""])[:3]
    if not name or not scenario:
        raise Failure("--run wants NAME|SCENARIO|ARGS, got %r" % spec)
    out = os.path.join(workdir, name, "out")
    card = os.path.join(workdir, name, "card")
    os.makedirs(out)
    os.makedirs(card)
    extra = [a.format(out=out, card=card, wav=os.path.join(out, "run.wav")) for a in shlex.split(args)]
    cmd = [sim, "--scenario", scenario, "--out", out] + extra
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        raise Failure("%s: the simulator exited %d:\n%s" % (name, proc.returncode, "\n".join(proc.stdout.splitlines()[-12:])))
    used = re.search(r"handle 0 used (\d+) times", proc.stdout)
    if used is None:
        raise Failure("%s: the simulator did not report the use of handle 0 (an old simulator?)" % name)
    if int(used.group(1)) != 0:
        raise Failure("%s: the app's bitmap handle 0 was used %s times: the kit's apps never touch it" % (name, used.group(1)))
    path = os.path.join(out, "digests.txt")
    if not os.path.isfile(path):
        raise Failure("%s: the simulator wrote no digests.txt (the scenario takes no snap?)" % name)
    with open(path) as f:
        return parse_digests(f.read(), name + "/")


def main(argv):
    parser = argparse.ArgumentParser(prog="digest_check.py")
    parser.add_argument("--sim")
    parser.add_argument("--pins")
    parser.add_argument("--run", action="append", default=[])
    parser.add_argument("--have", action="append", default=[])
    parser.add_argument("--skip", action="append", default=[])
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.pins or (not args.run and not args.have) or (args.run and not args.sim):
        parser.print_usage(sys.stderr)
        return 2
    try:
        have = []
        for path in args.have:
            with open(path) as f:
                have += parse_digests(f.read())
        if args.run:
            with tempfile.TemporaryDirectory(prefix="digest_check_") as workdir:
                for spec in args.run:
                    have += run_one(args.sim, spec, workdir)
        have = [(k, d) for k, d in have if k not in args.skip]
        if args.update:
            with open(args.pins, "w") as f:
                f.write(render_pins(have))
            print("digest_check: %s rewritten: %d frames" % (args.pins, len(have)))
            return 0
        with open(args.pins) as f:
            pinned = [(k, d) for k, d in parse_digests(f.read()) if k not in args.skip]
        problems = compare(pinned, have)
        if problems:
            print("digest_check: %s differs from what the simulator drew (%d problems):" % (args.pins, len(problems)))
            for p in problems:
                print("  " + p)
            print("If the change is meant, regenerate the pins with the same command plus --update, and review the new file with it:")
            print("  " + " ".join(shlex.quote(a) for a in [sys.executable, "-I"] + sys.argv + ["--update"]))
            return 1
        print("digest_check: %d frames match %s" % (len(have), args.pins))
        return 0
    except Failure as e:
        print("digest_check: %s" % e)
        return 1
    except OSError as e:
        print("digest_check: %s" % e)
        return 1


def self_test():
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("self-test FAILED: " + what)
            ok = False

    a = parse_digests("# c\n01_boot 0000abcd  # tail\n\n02_x ffffffff\n", "run/")
    check(a == [("run/01_boot", "0000abcd"), ("run/02_x", "ffffffff")], "parse with prefix, comments and blanks")
    for bad in ("01_boot ABCD", "01_boot 0000abcd extra", "01_boot"):
        try:
            parse_digests(bad)
            check(False, "refuses %r" % bad)
        except Failure:
            pass
    check(compare(a, a) == [], "equal lists compare equal")
    check(compare(a, a[:1]) == ["pinned frame not drawn: run/02_x ffffffff"], "a missing frame is named")
    check(compare(a[:1], a) == ["new frame, not pinned: run/02_x ffffffff"], "an unpinned frame is named")
    moved = [a[0], ("run/02_x", "fffffffe")]
    check(compare(a, moved) == ["moved: run/02_x was ffffffff, is fffffffe"], "a moved digest is named")
    check(len(compare(a + [a[0]], a)) == 1, "a repeated key is reported")
    check(parse_digests(render_pins(a)) == a, "a rewritten pin file reads back")
    print("digest_check self-test: " + ("passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
