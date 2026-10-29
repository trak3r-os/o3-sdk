#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""midi_clock_tool: MIDI clock out / in checks of the Tracker over a raw ALSA MIDI device node (stdlib only; Linux). Helper of MANUAL.md, not a scenario.

    midi_clock_tool.py list                                   the raw MIDI nodes (/dev/snd/midiC*D*) and what `amidi -l` calls them
    midi_clock_tool.py measure DEV [--seconds 20]             count the 0xF8 clocks the unit sends (and FA / FB / FC / FE bytes) and say the BPM, in 1 s windows too
    midi_clock_tool.py send DEV --profile 120:10,140:10 [--start] [--stop]
                                                              be the MIDI clock master: 24 clocks a beat at the BPM of each segment ("bpm:seconds,..."),
                                                              FA (Start) first with --start (FB Continue with --continue), FC (Stop) at the end with --stop
    midi_clock_tool.py --self-test                            send and measure against a FIFO: no MIDI device

DEV is /dev/snd/midiC<card>D<device> (the card of `amidi -l`: hw:1,0,0 is /dev/snd/midiC1D0). The node must be readable / writable by you (group audio).
Timestamps are the host's monotonic clock when read() returned: a USB MIDI packet is 1 ms, so single clocks are only known to about +-1-2 ms; the mean over 20 s is good to 0.01 %.
"""
import argparse
import glob
import os
import select
import subprocess
import sys
import tempfile
import threading
import time


def measure(dev, seconds, quiet=False, stop_event=None):
    fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
    stamps = []
    events = []
    start = time.monotonic()
    try:
        while time.monotonic() - start < seconds and not (stop_event and stop_event.is_set()):
            ready, _, _ = select.select([fd], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            now = time.monotonic()
            if not data:  # a FIFO with no writer (yet, or any more) reads as end of file; a real MIDI node never does
                if stamps or events:
                    break
                time.sleep(0.01)
                continue
            for b in data:
                if b == 0xF8:
                    stamps.append(now)
                elif b in (0xFA, 0xFB, 0xFC, 0xFE, 0xFF):
                    events.append((now - start, b))
    finally:
        os.close(fd)
    result = {"clocks": len(stamps), "events": events}
    if len(stamps) >= 25:
        span = stamps[-1] - stamps[0]
        result["bpm"] = (len(stamps) - 1) / 24.0 / span * 60.0
        windows = []
        i = 0
        for w in range(int(span)):
            n = sum(1 for s in stamps if stamps[0] + w <= s < stamps[0] + w + 1)
            windows.append(n / 24.0 * 60.0)
        result["windows"] = windows
    if not quiet:
        names = {0xFA: "Start", 0xFB: "Continue", 0xFC: "Stop", 0xFE: "ActiveSense", 0xFF: "Reset"}
        print("clocks %d in %.1f s" % (len(stamps), seconds))
        for t, b in events[:20]:
            print("  %7.3f s  %s" % (t, names[b]))
        if "bpm" in result:
            print("mean BPM %.3f (first to last clock)" % result["bpm"])
            print("BPM per second window: %s" % " ".join("%.1f" % w for w in result["windows"]))
        else:
            print("fewer than 25 clocks: no BPM")
    return result


def send(dev, profile, start=False, stop=False, cont=False):
    fd = os.open(dev, os.O_WRONLY)
    try:
        if start:
            os.write(fd, b"\xFA")
        elif cont:
            os.write(fd, b"\xFB")
        t0 = time.perf_counter()
        t = t0
        total = 0
        for bpm, seconds in profile:
            interval = 60.0 / (24.0 * bpm)
            n = int(seconds / interval)
            for _ in range(n):
                while True:
                    left = t - time.perf_counter()
                    if left <= 0:
                        break
                    time.sleep(left - 0.0015 if left > 0.003 else 0)
                os.write(fd, b"\xF8")
                total += 1
                t += interval
        if stop:
            os.write(fd, b"\xFC")
    finally:
        os.close(fd)
    return total


def parse_profile(text):
    out = []
    for part in text.split(","):
        bpm, _, seconds = part.partition(":")
        out.append((float(bpm), float(seconds)))
    return out


def list_nodes():
    for node in sorted(glob.glob("/dev/snd/midiC*D*")):
        print(node)
    try:
        print(subprocess.run(["amidi", "-l"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10).stdout)
    except (OSError, subprocess.TimeoutExpired) as error:
        print("amidi: %s" % error)


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        fifo = os.path.join(tmp, "midi")
        os.mkfifo(fifo)
        out = {}

        def reader():
            out["r"] = measure(fifo, 6.0, quiet=True)

        th = threading.Thread(target=reader)
        th.start()
        time.sleep(0.2)
        n = send(fifo, parse_profile("120:2,150:2"), start=True, stop=True)
        th.join()
        r = out["r"]
        problems = []
        if r["clocks"] != n:
            problems.append("sent %d clocks, read %d" % (n, r["clocks"]))
        if not any(b == 0xFA for _, b in r["events"]) or not any(b == 0xFC for _, b in r["events"]):
            problems.append("Start / Stop not seen: %r" % (r["events"],))
        w = r.get("windows", [])
        if len(w) < 3 or not (115 <= w[0] <= 125) or not (145 <= w[-1] <= 155):
            problems.append("windows %r are not 120 then 150 BPM" % (w,))
        if problems:
            print("midi_clock_tool self-test FAILED: " + "; ".join(problems))
            return 1
        print("midi_clock_tool self-test passed (%d clocks, windows %s)" % (n, " ".join("%.1f" % x for x in w)))
        return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list")
    m = sub.add_parser("measure")
    m.add_argument("dev")
    m.add_argument("--seconds", type=float, default=20.0)
    s = sub.add_parser("send")
    s.add_argument("dev")
    s.add_argument("--profile", required=True)
    s.add_argument("--start", action="store_true")
    s.add_argument("--stop", action="store_true")
    s.add_argument("--continue", dest="cont", action="store_true", help="FB (Continue) first instead of FA (OS-HIL2)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "list":
        list_nodes()
        return 0
    if args.command == "measure":
        measure(args.dev, args.seconds)
        return 0
    if args.command == "send":
        n = send(args.dev, parse_profile(args.profile), args.start, args.stop, args.cont)
        print("sent %d clocks%s%s" % (n, " after Start" if args.start else "", ", then Stop" if args.stop else ""))
        return 0
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
