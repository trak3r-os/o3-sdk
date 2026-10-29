#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""check_template.py: instantiate templates/app and build it the way a new app author does (docs/NEW-APP.md).

    check_template.py host --sdk <o3-sdk dir> --work <dir> [--cmake CMAKE] [--ctest CTEST] [--jobs N] [--ccache]
    check_template.py mk66 --sdk <o3-sdk dir> --work <dir> --arm <toolchain dir> --nm <arm-none-eabi-nm> [--jobs N] [--ccache] ...

host: tools/new_app.py into <work>/app/demo_app, configure and build it against --sdk (warnings are failures), run its four ctests
(app_test, pad_map_test, pattern_test, voice_test) and its two scenarios in the simulator (exit 0, pictures and the WAV written, no frame
refused): boot.txt, and files.txt on the simulator's card (a fresh directory), which saves, reboots and loads, and leaves its files.
mk66: the same instance, cross-built with -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<--arm>, then its image ctests (demo_app_image,
_flashconfig, _ramfunc and _usb), the image files, and no software double (__aeabi_d) in the ELF.

<work> is this check's own: the instance (<work>/app) is created fresh on every run, and only a <work> that carries the marker file this
script writes is ever cleaned; the build directories (<work>/build-host, <work>/build-mk66) stay between runs, so a second run only rebuilds
what changed. It never touches a unit and writes nothing outside <work>.
Exit status: 0 passed, 1 a check failed, 2 a usage error.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

MARKER = ".o3sdk-template-check"
APP = "demo_app"


class Failure(Exception):
    pass


def run(cmd, cwd=None, env=None, what=None, tail=25):
    """Runs a command, returns its output text; raises Failure with the tail of the output when it fails."""
    started = time.monotonic()
    proc = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    label = what or " ".join(cmd[:3])
    print("  %-46s %5.1f s  rc=%d" % (label, time.monotonic() - started, proc.returncode), flush=True)
    if proc.returncode != 0:
        lines = proc.stdout.splitlines()
        raise Failure("%s failed (rc=%d):\n%s" % (label, proc.returncode, "\n".join(lines[-tail:])))
    return proc.stdout


def no_warnings(text, what):
    bad = [l for l in text.splitlines() if re.search(r"\bwarning\b", l, re.IGNORECASE) and "-Werror" not in l]
    if bad:
        raise Failure("%s printed warnings:\n%s" % (what, "\n".join(bad[:10])))


def passed(ctest_output, names):
    """The ctest summary says all passed and every test in `names` ran."""
    if "100% tests passed" not in ctest_output:
        raise Failure("ctest did not pass everything:\n" + "\n".join(ctest_output.splitlines()[-25:]))
    for name in names:
        if not re.search(r"Test\s+#\d+: %s \.+\s+Passed" % re.escape(name), ctest_output):
            raise Failure("the ctest '%s' did not run and pass" % name)


def prepare(args):
    work = os.path.abspath(args.work)
    marker = os.path.join(work, MARKER)
    os.makedirs(work, exist_ok=True)
    if not os.path.exists(marker):
        if os.listdir(work):
            raise Failure("%s is not empty and is not this check's directory (no %s): refusing to clean it" % (work, MARKER))
        with open(marker, "w") as f:
            f.write("created by templates/check_template.py; the instance in app/ is recreated on every run\n")
    app_parent = os.path.join(work, "app")
    if os.path.isdir(app_parent):
        shutil.rmtree(app_parent)
    sdk = os.path.abspath(args.sdk)
    run([sys.executable, "-I", os.path.join(sdk, "tools", "new_app.py"), os.path.join(app_parent, APP), APP, "--name", "DEMO-APP"],
        what="new_app.py")
    # Refuses to overwrite: the same command again must fail.
    again = subprocess.run([sys.executable, "-I", os.path.join(sdk, "tools", "new_app.py"), os.path.join(app_parent, APP), APP],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if again.returncode != 1:
        raise Failure("new_app.py overwrote or mishandled an existing directory (rc=%d)" % again.returncode)
    return work, sdk, os.path.join(app_parent, APP)


def configure_and_build(args, src, build, sdk, extra):
    env = dict(os.environ)
    env.pop("O3SDK_DIR", None)  # the check passes it explicitly
    cfg = [args.cmake, "-S", src, "-B", build, "-DO3SDK_DIR=" + sdk] + extra
    if args.ccache:
        cfg += ["-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"]
    out = run(cfg, env=env, what="cmake configure")
    out += run([args.cmake, "--build", build, "-j", str(args.jobs)], env=env, what="cmake --build", tail=40)
    no_warnings(out, "the configure and build")


def check_host(args):
    work, sdk, src = prepare(args)
    build = os.path.join(work, "build-host")
    configure_and_build(args, src, build, sdk, [])
    out = run([args.ctest, "--test-dir", build, "--output-on-failure"], what="ctest (the instance's host tests)")
    passed(out, ["app_test", "pad_map_test", "pattern_test", "voice_test"])
    sim_out = os.path.join(work, "sim-out")
    shutil.rmtree(sim_out, ignore_errors=True)
    os.makedirs(sim_out)
    text = run([os.path.join(build, "app", APP + "_sim"), "--scenario", os.path.join(src, "app", "scenarios", "boot.txt"), "--out", sim_out],
               what="the simulator, boot.txt")
    if "0 refused" not in text or "0 events dropped" not in text:
        raise Failure("the simulator summary is not clean:\n" + text[-400:])
    for name in ("01_boot.png", "03_decay.png", "boot.wav"):
        path = os.path.join(sim_out, name)
        if not os.path.isfile(path) or os.path.getsize(path) < 1000:
            raise Failure("the scenario did not write %s" % name)
    # The file API: the scenario saves the pattern, reboots and loads it back on the simulator's card (a directory, fresh for this run).
    card = os.path.join(work, "sim-card")
    shutil.rmtree(card, ignore_errors=True)
    os.makedirs(card)
    text = run([os.path.join(build, "app", APP + "_sim"), "--scenario", os.path.join(src, "app", "scenarios", "files.txt"), "--card", card,
                "--out", sim_out], what="the simulator, files.txt on a card")
    if "0 refused" not in text or "0 events dropped" not in text:
        raise Failure("the simulator summary is not clean:\n" + text[-400:])
    for name in ("01_saved.png", "04_after_reboot.png"):
        if not os.path.isfile(os.path.join(sim_out, name)):
            raise Failure("the files scenario did not write %s" % name)
    for name in ("pattern.o3a", "pattern.o3a.bak", "config.o3a"):
        if not os.path.isfile(os.path.join(card, "Apps", APP, name)):
            raise Failure("the files scenario left no %s on the card" % name)


def check_mk66(args):
    work, sdk, src = prepare(args)
    build = os.path.join(work, "build-mk66")
    configure_and_build(args, src, build, sdk, ["-DO3SDK_PLATFORM=mk66", "-DARM_GNU_ROOT=" + os.path.abspath(args.arm)])
    out = run([args.ctest, "--test-dir", build, "-L", "mk66", "--output-on-failure"], what="ctest -L mk66 (the image checks)")
    passed(out, [APP + "_image", APP + "_flashconfig", APP + "_ramfunc", APP + "_usb"])
    for ext in ("elf", "hex", "bin", "ptf"):
        path = os.path.join(build, "app", "%s.%s" % (APP, ext))
        if not os.path.isfile(path) or os.path.getsize(path) < 1000:
            raise Failure("the image file %s is missing or empty" % path)
    syms = run([args.nm, os.path.join(build, "app", APP + ".elf")], what="nm of the image")
    doubles = [l for l in syms.splitlines() if re.search(r"\b__aeabi_d", l)]
    if doubles:
        raise Failure("software double helpers in the image:\n" + "\n".join(doubles[:5]))


def main(argv):
    parser = argparse.ArgumentParser(prog="check_template.py")
    parser.add_argument("mode", choices=("host", "mk66"))
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--arm", help="mk66: the toolchain directory (the one that holds bin/)")
    parser.add_argument("--nm", help="mk66: arm-none-eabi-nm")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--ccache", action="store_true")
    args = parser.parse_args(argv)
    if args.mode == "mk66" and not (args.arm and args.nm):
        parser.error("mk66 needs --arm and --nm")
    try:
        os.nice(10)
    except OSError:
        pass
    print("check_template.py %s" % args.mode, flush=True)
    try:
        (check_host if args.mode == "host" else check_mk66)(args)
    except Failure as err:
        print("check_template.py FAILED: %s" % err, file=sys.stderr)
        return 1
    print("check_template.py %s: passed" % args.mode)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
