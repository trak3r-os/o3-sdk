#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3tone: play a known tone from the PC into the Tracker's USB AUDIO return (OS-USBRECQA; stdlib only; `pw-play` from PipeWire or `aplay` from alsa-utils does the playback).

A unit in USB AUDIO mode presents a stereo return, the computer's stream into the unit ("2 IN" of profile 8, OS-USBRET), and Sample Rec has the source USB that records it. A scenario that records
from it (hil/qa_usbrec.json) needs the computer to play something known while the runner drives the unit. o3hil's `audio` step only captures, and no step runs a program on the PC, so this tool is
started by hand in a second terminal BEFORE the scenario and left running (it plays for --seconds, 300 by default, and ends by itself or on Ctrl-C). The tone is a sine, the same on both channels,
at --hz (1000) and --db dBFS peak (-12): a whole number of cycles per second at 44.1 kHz, so the loop it plays has no step in it.

The unit's playback device is found BY NAME at run time (`aplay -l`, the card whose description names the unit: "Track3r"; with pw-play, the PipeWire sink of that name from `pw-dump`), never by a
card number the machine may hand out differently next time. --device overrides it (an alsa name such as plughw:3,0 for aplay, a PipeWire node name for pw-play).

    o3tone.py list                                    the playback devices alsa knows (aplay -l), the one that looks like the unit, and the PipeWire sink that looks like it
    o3tone.py play [--player auto|pw-play|aplay] [--device auto|NAME] [--hz 1000] [--db -12] [--seconds 300] [--rate 44100]
    o3tone.py gen FILE.wav [--hz 1000] [--db -12] [--seconds 10] [--rate 44100]     write the tone as a stereo WAV and exit (play it with anything)
    o3tone.py --self-test

`--player auto` takes pw-play when PipeWire has a sink that looks like the unit (a desktop's sound server usually holds the card, so aplay on it would say "busy"), else aplay. Exit status: 0 played
(or written), 2 no player, no device that looks like the unit, or the player failed.
"""
import argparse
import importlib.util
import json
import math
import os
import stat
import struct
import subprocess
import sys
import tempfile
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
SEGMENT_SECONDS = 60  # one WAV of at most this long is played again and again: a gap of the player's start-up every minute, no more


class ToneError(Exception):
    pass


def sibling(name):
    """o3audio as a module (python -I puts no script directory on the path)."""
    spec = importlib.util.spec_from_file_location("o3_" + name, os.path.join(HERE, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# ---- the tone ----------------------------------------------------------------------------------------------------------------------

def tone_frames(hz, db, seconds, rate):
    """The 16-bit samples (one channel) of a sine at `hz`, `db` dBFS peak, as long as a whole number of its cycles nearest to `seconds`: played again from the start it continues without a step."""
    if not (0.0 < hz < rate / 2.0):  # written so that nan fails it too
        raise ToneError("the frequency must be above 0 and below half the rate (%d)" % (rate // 2))
    if not (-90.0 <= db <= 0.0):
        raise ToneError("the level is -90 to 0 dBFS")
    if not math.isfinite(seconds):
        raise ToneError("the length must be a number of seconds")
    cycles = max(1, int(round(hz * seconds)))
    count = int(round(cycles * rate / float(hz)))
    amp = 32767.0 * (10.0 ** (db / 20.0))
    return [int(round(amp * math.sin(2.0 * math.pi * cycles * i / count))) for i in range(count)]


def write_wav(path, samples, rate, channels=2):
    """A WAV of the samples on every one of `channels` channels."""
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for v in samples:
            data += struct.pack("<h", v) * channels
        w.writeframes(bytes(data))


# ---- finding the unit by name ----------------------------------------------------------------------------------------------------------

def alsa_device(aplay="aplay"):
    """`plughw:N,M` of the playback card whose description names the unit (o3audio.find_unit's hints), or None. plughw: lets alsa convert what the device does not take as it is."""
    audio = sibling("o3audio")
    try:
        found = audio.find_unit(audio.list_devices(aplay))
    except audio.AudioError:
        return None
    return None if found is None else "plughw:" + found[len("hw:"):]


def pipewire_sink(pw_dump="pw-dump"):
    """The node name of the PipeWire sink whose name or description names the unit, or None (no pw-dump, no PipeWire, no such sink)."""
    try:
        run = subprocess.run([pw_dump], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10)
        objects = json.loads(run.stdout.decode("utf-8", "replace"))
    except (OSError, subprocess.TimeoutExpired, ValueError):
        return None
    hints = ("track3r", "tracker", "o3sdk")
    for obj in objects if isinstance(objects, list) else []:
        props = ((obj.get("info") or {}).get("props") or {}) if isinstance(obj, dict) else {}
        if props.get("media.class") != "Audio/Sink":
            continue
        text = (str(props.get("node.name", "")) + " " + str(props.get("node.description", "")) + " " + str(props.get("device.description", ""))).lower()
        if any(h in text for h in hints):
            return props.get("node.name")
    return None


def choose_player(player, device, aplay="aplay", pw_play="pw-play", pw_dump="pw-dump"):
    """(command prefix, the name of what it plays to) for `--player` and `--device`; ToneError when nothing looks like the unit."""
    if device != "auto" and player == "auto":
        raise ToneError("--device names what one player plays to: give --player aplay (an ALSA name) or pw-play (a PipeWire node name) with it")
    if player in ("auto", "pw-play"):
        sink = device if (device != "auto" and player == "pw-play") else pipewire_sink(pw_dump)
        if sink is not None:
            return [pw_play, "--target", sink], "PipeWire sink " + sink
        if player == "pw-play":
            raise ToneError("no PipeWire sink looks like the unit (pw-dump): give --device NODE-NAME")
    name = device if device != "auto" else alsa_device(aplay)
    if name is None:
        raise ToneError("no playback device looks like the unit (aplay -l): give --device plughw:CARD,DEV, or is the unit in USB AUDIO mode (`usb mode audio`)?")
    return [aplay, "-q", "-D", name], "ALSA device " + name


# ---- playing ------------------------------------------------------------------------------------------------------------------------

def play(prefix, wav_path, seconds, clock=time.monotonic, popen=subprocess.Popen):
    """Run the player on the WAV again and again for `seconds`; a player that fails to start, or ends at once with an error, is a ToneError. `clock` and `popen` are the self-test's hooks."""
    end = clock() + seconds
    runs = 0
    while clock() < end:
        began = clock()
        try:
            proc = popen(prefix + [wav_path], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        except OSError as error:
            raise ToneError("%s cannot run: %s" % (prefix[0], error))
        try:
            _, err = proc.communicate(timeout=max(1.0, end - clock()) + SEGMENT_SECONDS)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise ToneError("%s did not end" % prefix[0])
        runs += 1
        if proc.returncode != 0:
            raise ToneError("%s exited %d: %s" % (prefix[0], proc.returncode, err.decode("utf-8", "replace").strip()))
        if clock() - began < 0.05:
            raise ToneError("%s returned at once: nothing was played" % prefix[0])
    return runs


def cmd_list(args):
    audio = sibling("o3audio")
    try:
        devices = audio.list_devices(args.aplay)
    except audio.AudioError as error:
        print("aplay: %s" % error)
        devices = []
    for hw, line in devices:
        print("%-10s %s" % (hw, line))
    unit = audio.find_unit(devices) if devices else None
    print("the unit (ALSA): %s" % (("plughw:" + unit[len("hw:"):]) if unit else "not found"))
    print("the unit (PipeWire sink): %s" % (pipewire_sink(args.pw_dump) or "not found"))
    return 0


def cmd_gen(args):
    try:
        write_wav(args.file, tone_frames(args.hz, args.db, args.seconds, args.rate), args.rate)
    except (ToneError, OSError) as error:
        print("o3tone: %s" % error, file=sys.stderr)
        return 2
    print(args.file)
    return 0


def cmd_play(args):
    try:
        prefix, where = choose_player(args.player, args.device, args.aplay, args.pw_play, args.pw_dump)
        samples = tone_frames(args.hz, args.db, min(args.seconds, SEGMENT_SECONDS), args.rate)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "tone.wav")
            write_wav(path, samples, args.rate)
            print("o3tone: %g Hz, %.1f dBFS, both channels, %g s to the %s; start the scenario now (Ctrl-C ends it)" % (args.hz, args.db, args.seconds, where), flush=True)
            try:
                play(prefix, path, args.seconds)
            except KeyboardInterrupt:
                print("o3tone: stopped")
    except (ToneError, OSError) as error:
        print("o3tone: %s" % error, file=sys.stderr)
        return 2
    print("o3tone: done")
    return 0


# ---- self-test -----------------------------------------------------------------------------------------------------------------------

def _script(path, text):
    with open(path, "w") as f:
        f.write("#!/bin/sh\n" + text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


class _FakeTime:
    """A clock that moves only when a player is started: each run lasts `run` seconds of fake time, whatever the machine's load (the checks on play() then depend on no real timing)."""

    def __init__(self):
        self.now = 0.0
        self.run = 0.1
        self.starts = 0

    def clock(self):
        return self.now

    def popen(self, *args, **kwargs):
        self.starts += 1
        if self.starts > 100:
            raise AssertionError("play() did not end in 100 runs of fake time")
        proc = subprocess.Popen(*args, **kwargs)
        self.now += self.run
        return proc


def self_test():
    problems = []
    audio = sibling("o3audio")
    # the tone: both channels the same, the level and the pitch asked for, a whole number of cycles (the loop has no step)
    for hz, db in ((1000.0, -12.0), (440.0, -6.0), (997.0, -20.0)):
        samples = tone_frames(hz, db, 1.0, 44100)
        peak = max(abs(v) for v in samples)
        if abs(20.0 * math.log10(peak / 32768.0) - db) > 0.1:
            problems.append("the peak of %.0f Hz at %.0f dB is %d" % (hz, db, peak))
        step = samples[0] - samples[-1]  # the sample before the loop's start and the first one: one sample step of the sine at most
        if abs(step) > 2.0 * math.pi * hz / 44100.0 * peak * 1.05:
            problems.append("the loop of %.0f Hz has a step of %d" % (hz, step))
    if len(tone_frames(1000.0, -12.0, 1.0, 44100)) != 44100:
        problems.append("1 s of 1 kHz at 44.1 kHz is not 44100 samples")
    nan, inf = float("nan"), float("inf")
    for bad in ((0.0, -12.0), (30000.0, -12.0), (1000.0, 3.0), (1000.0, -120.0), (nan, -12.0), (1000.0, nan), (inf, -12.0), (1000.0, -inf), (1000.0, 1.0, nan), (1000.0, -12.0, inf)):
        try:
            tone_frames(bad[0], bad[1], bad[2] if len(bad) > 2 else 1.0, 44100)
            problems.append("a tone of %r was accepted" % (bad,))
        except ToneError:
            pass
        except Exception as error:  # a bad number must be a ToneError, never a traceback
            problems.append("a tone of %r raised %s: %s" % (bad, type(error).__name__, error))
    with tempfile.TemporaryDirectory() as tmp:
        wav_path = os.path.join(tmp, "t.wav")
        write_wav(wav_path, tone_frames(1000.0, -12.0, 1.0, 44100), 44100)
        with open(wav_path, "rb") as f:
            analysis = audio.analyze_wav(f.read())
        left, right = analysis["results"]
        if analysis["channels"] != 2 or analysis["rate"] != 44100:
            problems.append("the WAV is not stereo 44100: %r" % analysis["channels"])
        for res in (left, right):
            if res["pitch_hz"] is None or abs(audio.cents(res["pitch_hz"], 1000.0)) > 5.0 or abs(res["peak_db"] + 12.0) > 0.3 or res["clipped"] != 0:
                problems.append("the WAV's channel is not 1 kHz at -12 dBFS: %r" % res)

        # a fake `aplay` (the card is found by its name, not by a number) and a fake `pw-play`, `pw-dump`; each logs what it was given
        log = os.path.join(tmp, "log.txt")
        aplay = os.path.join(tmp, "aplay")
        _script(aplay, "if [ \"$1\" = \"-l\" ]; then\n  echo 'card 0: PCH [HDA Intel PCH], device 0: ALC [ALC]'\n  echo 'card 5: Track3r [Track3r], device 0: USB Audio [USB Audio]'\n  exit 0\nfi\n"
                       "echo \"aplay $@\" >> %s\nsleep 0.1\n" % log)
        pw_play = os.path.join(tmp, "pw-play")
        _script(pw_play, "echo \"pw-play $@\" >> %s\nsleep 0.1\n" % log)
        dump = os.path.join(tmp, "pw-dump")
        sinks = [{"type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink", "node.name": "alsa_output.pci-0000_00_1f.3.analog-stereo", "node.description": "Built-in Audio"}}},
                 {"type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Source", "node.name": "alsa_input.usb-track3r-os_Track3r-00.mono", "node.description": "Track3r"}}},
                 {"type": "PipeWire:Interface:Node", "info": {"props": {"media.class": "Audio/Sink", "node.name": "alsa_output.usb-track3r-os_Track3r-00.analog-stereo", "node.description": "Track3r Analog Stereo"}}},
                 {"type": "PipeWire:Interface:Device", "info": {"props": {}}}]
        with open(os.path.join(tmp, "dump.json"), "w") as f:
            json.dump(sinks, f)
        _script(dump, "cat %s/dump.json\n" % tmp)
        nodump = os.path.join(tmp, "no-pw-dump")
        none_dump = os.path.join(tmp, "pw-dump-empty")
        _script(none_dump, "echo '[]'\n")

        if alsa_device(aplay) != "plughw:5,0":
            problems.append("the unit was not found by name among the ALSA cards: %r" % alsa_device(aplay))
        if pipewire_sink(dump) != "alsa_output.usb-track3r-os_Track3r-00.analog-stereo":
            problems.append("the PipeWire sink of the unit was not found by name (a source and another sink must not do): %r" % pipewire_sink(dump))
        if pipewire_sink(nodump) is not None or pipewire_sink(none_dump) is not None:
            problems.append("a missing or empty pw-dump found a sink")
        prefix, where = choose_player("auto", "auto", aplay, pw_play, dump)
        if prefix != [pw_play, "--target", "alsa_output.usb-track3r-os_Track3r-00.analog-stereo"] or "PipeWire" not in where:
            problems.append("auto with PipeWire did not take pw-play: %r" % prefix)
        prefix, where = choose_player("auto", "auto", aplay, pw_play, none_dump)
        if prefix != [aplay, "-q", "-D", "plughw:5,0"] or "ALSA" not in where:
            problems.append("auto without a PipeWire sink did not take aplay: %r" % prefix)
        if choose_player("aplay", "auto", aplay, pw_play, dump)[0] != [aplay, "-q", "-D", "plughw:5,0"]:
            problems.append("--player aplay did not take aplay")
        if choose_player("aplay", "plughw:1,0", aplay, pw_play, dump)[0] != [aplay, "-q", "-D", "plughw:1,0"]:
            problems.append("--device was not used by aplay")
        if choose_player("pw-play", "my_sink", aplay, pw_play, none_dump)[0] != [pw_play, "--target", "my_sink"]:
            problems.append("--device was not used by pw-play")
        # nothing looks like the unit: an error that says so, never a guess
        empty_aplay = os.path.join(tmp, "aplay_none")
        _script(empty_aplay, "echo 'card 0: PCH [HDA Intel PCH], device 0: ALC [ALC]'\n")
        for player, dev in (("aplay", "auto"), ("auto", "auto")):
            try:
                choose_player(player, dev, empty_aplay, pw_play, none_dump)
                problems.append("a machine with no unit gave a player (%s)" % player)
            except ToneError as error:
                if "no playback device looks like the unit" not in str(error):
                    problems.append("wrong error for no unit: %s" % error)
        try:
            choose_player("pw-play", "auto", aplay, pw_play, none_dump)
            problems.append("pw-play with no sink gave a player")
        except ToneError:
            pass
        try:
            choose_player("auto", "plughw:1,0", aplay, pw_play, dump)
            problems.append("--device with --player auto was accepted")
        except ToneError:
            pass

        # playing: the player is run on the WAV again and again for the time asked, with the device found by name
        # (on fake time: runs of 0.1 s in a window of 0.35 s are exactly four, and a player of no time at all is "nothing was played")
        ft = _FakeTime()
        runs = play([aplay, "-q", "-D", "plughw:5,0"], wav_path, 0.35, ft.clock, ft.popen)
        with open(log) as f:
            logged = f.read().splitlines()
        if runs != 4 or len(logged) != runs or any(l != "aplay -q -D plughw:5,0 " + wav_path for l in logged):
            problems.append("the player was not run as asked: %d runs, %r" % (runs, logged[:2]))
        bad = os.path.join(tmp, "aplay_busy")
        _script(bad, "echo 'aplay: main:831: audio open error: Device or resource busy' >&2\nexit 1\n")
        try:
            play([bad], wav_path, 0.2, ft.clock, ft.popen)
            problems.append("a failing player was accepted")
        except ToneError as error:
            if "busy" not in str(error):
                problems.append("the player's own error was lost: %s" % error)
        instant = os.path.join(tmp, "aplay_instant")
        _script(instant, "exit 0\n")
        ft.run = 0.0
        try:
            play([instant], wav_path, 0.2, ft.clock, ft.popen)
            problems.append("a player that plays nothing was accepted")
        except ToneError as error:
            if "nothing was played" not in str(error):
                problems.append("wrong error for an instant player: %s" % error)
        try:
            play([os.path.join(tmp, "no-such-player")], wav_path, 0.2, ft.clock, ft.popen)
            problems.append("a missing player was accepted")
        except ToneError:
            pass

        # the commands, end to end with the stand-ins
        class A:
            pass
        a = A()
        a.player, a.device, a.aplay, a.pw_play, a.pw_dump = "aplay", "auto", aplay, pw_play, dump
        a.hz, a.db, a.seconds, a.rate = 1000.0, -12.0, 0.3, 44100
        open(log, "w").close()
        if cmd_play(a) != 0:
            problems.append("play with the stand-ins failed")
        a.aplay = empty_aplay
        if cmd_play(a) != 2:
            problems.append("play with no unit did not exit 2")
        g = A()
        g.file, g.hz, g.db, g.seconds, g.rate = os.path.join(tmp, "g.wav"), 1000.0, -12.0, 2.0, 44100
        if cmd_gen(g) != 0 or not os.path.isfile(g.file):
            problems.append("gen wrote nothing")
        g.hz = 99999.0
        if cmd_gen(g) != 2:
            problems.append("gen with a bad frequency did not exit 2")
        # nan and inf through the real argument parser: exit 2, never a traceback (gen writes a file under tmp; play runs on the stand-ins only)
        for flag in ("--hz", "--db", "--seconds"):
            for text in ("nan", "inf"):
                try:
                    code = main(["gen", os.path.join(tmp, "n.wav"), flag, text])
                except Exception as error:
                    code = "%s: %s" % (type(error).__name__, error)
                if code != 2:
                    problems.append("gen %s %s exited %r, not 2" % (flag, text, code))
        a.aplay, a.hz = aplay, nan
        try:
            code = cmd_play(a)
        except Exception as error:
            code = "%s: %s" % (type(error).__name__, error)
        if code != 2:
            problems.append("play with a nan frequency exited %r, not 2" % (code,))
    if problems:
        for p in problems:
            print("o3tone self-test: FAIL: %s" % p)
        return 1
    print("o3tone self-test: ok")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="Play a known tone into the Tracker's USB AUDIO return (OS-USBRECQA).")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--aplay", default="aplay", help=argparse.SUPPRESS)
    parser.add_argument("--pw-play", default="pw-play", help=argparse.SUPPRESS)
    parser.add_argument("--pw-dump", default="pw-dump", help=argparse.SUPPRESS)
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list")
    p = sub.add_parser("play")
    p.add_argument("--player", choices=("auto", "pw-play", "aplay"), default="auto")
    p.add_argument("--device", default="auto")
    p.add_argument("--hz", type=float, default=1000.0)
    p.add_argument("--db", type=float, default=-12.0)
    p.add_argument("--seconds", type=float, default=300.0)
    p.add_argument("--rate", type=int, default=44100)
    g = sub.add_parser("gen")
    g.add_argument("file")
    g.add_argument("--hz", type=float, default=1000.0)
    g.add_argument("--db", type=float, default=-12.0)
    g.add_argument("--seconds", type=float, default=10.0)
    g.add_argument("--rate", type=int, default=44100)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "list":
        return cmd_list(args)
    if args.command == "play":
        return cmd_play(args)
    if args.command == "gen":
        return cmd_gen(args)
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
