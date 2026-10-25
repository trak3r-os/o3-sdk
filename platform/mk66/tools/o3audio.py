#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3audio: record the unit's USB AUDIO device on the PC and say what is in it (OS-HIL; stdlib only, `arecord` from alsa-utils does the capture).

The unit presents USB AUDIO with a profile of 2, 8, 10 or 11 channels (`usb mode audio`, `usb profile`, `usb map`: a slot is a stem or the master; a boot presents
profile 8, "8 OUT / 2 IN", OS-USBRET). Without --channels the capture asks the device how many it has (/proc/asound/cardN/stream*, snd-usb-audio's own report), 8 when
that cannot be read. This tool records a few seconds of it (a subprocess of `arecord`, 16-bit little endian), splits the channels and measures each one:

  * level     peak and RMS in dBFS (32768 = 0 dBFS), and the DC offset
  * silence   RMS below a floor (default -60 dBFS)
  * clipping  samples at the rails (|x| >= 32766), and runs of three or more of them
  * pitch     the strongest spectral line of the Hann-windowed signal (a radix-2 FFT of 8192 points, parabolic interpolation on the log magnitude), when it stands out of
              the noise floor by 12 dB or more; None for noise and for silence

    o3audio.py list                                  the capture devices alsa knows (arecord -l) and the one that looks like the unit
    o3audio.py record [--device hw:2,0|auto] [--seconds 3] [--channels N|device's own] [--rate 48000] [--save out.wav] [CHECKS]
    o3audio.py analyze FILE.wav [CHECKS]             the same on a file
    o3audio.py --self-test

CHECKS apply to channel --channel (1-based; 0 = every channel): --expect-pitch HZ [--cents 40], --expect-silence, --not-silent, --min-rms DB, --max-peak DB, --no-clipping.
Exit status: 0 every check passed (or none given), 1 a check failed, 2 no capture (no device, arecord missing or failed, not a WAV).
"""
import argparse
import array
import cmath
import math
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile

FFT_POINTS = 8192
PROMINENCE_DB = 12.0
SILENCE_DB = -60.0
RAIL = 32766


class AudioError(Exception):
    pass


# ---- capture ---------------------------------------------------------------------------------------------------------------------

def list_devices(arecord="arecord"):
    """[(hw id, description line)] from `arecord -l`."""
    try:
        run = subprocess.run([arecord, "-l"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AudioError("cannot run %s: %s" % (arecord, error))
    found = []
    for line in run.stdout.splitlines():
        m = re.match(r"card (\d+): (\S+) \[(.*?)\], device (\d+): (.*)", line)
        if m:
            found.append(("hw:%s,%s" % (m.group(1), m.group(4)), line.strip()))
    return found


def find_unit(devices, hints=("track3r", "tracker", "o3sdk")):
    """The device whose description names the unit (the enumerated product is "Track3r"), or None."""
    for hw, line in devices:
        lowered = line.lower()
        if any(h in lowered for h in hints):
            return hw
    return None


BOOT_CHANNELS = 8  # the capture channels of the layout every boot presents ("8 OUT / 2 IN", profile 8), used when the device cannot be asked


def parse_capture_channels(text):
    """The most channels of any capture altset in the text of a /proc/asound/cardN/streamM file (snd-usb-audio: "Playback:" and "Capture:" sections, each with
    "Channels: N" lines under its altsets), or None when there is no capture section or no count."""
    in_capture = False
    best = None
    for line in text.splitlines():
        word = line.strip()
        if word in ("Playback:", "Capture:"):
            in_capture = word == "Capture:"
            continue
        m = re.match(r"Channels:\s*(\d+)", word)
        if in_capture and m:
            best = max(best or 0, int(m.group(1)))
    return best


def capture_channels(device, proc="/proc/asound"):
    """How many channels the capture side of the ALSA device `hw:CARD,DEV` (or plughw:) has, from the card's stream files; None when it cannot be told."""
    m = re.match(r"(?:plug)?hw:(\d+)", device)
    if m is None:
        return None
    card = os.path.join(proc, "card" + m.group(1))
    try:
        names = sorted(n for n in os.listdir(card) if n.startswith("stream"))
    except OSError:
        return None
    for name in names:
        try:
            with open(os.path.join(card, name), "r", errors="replace") as f:
                found = parse_capture_channels(f.read())
        except OSError:
            continue
        if found:
            return found
    return None


def record(device, seconds, channels, rate=48000, arecord="arecord", timeout_margin=15.0, proc="/proc/asound"):
    """The WAV bytes of `seconds` of `device` (arecord, S16_LE, to stdout). `channels` None: what the device reports (capture_channels), else BOOT_CHANNELS."""
    if device == "auto":
        device = find_unit(list_devices(arecord))
        if device is None:
            raise AudioError("no capture device looks like the unit (arecord -l): give --device hw:CARD,DEV")
    if channels is None:
        channels = capture_channels(device, proc) or BOOT_CHANNELS
    command = [arecord, "-q", "-D", device, "-f", "S16_LE", "-r", str(rate), "-c", str(channels), "-d", str(int(math.ceil(seconds))), "-t", "wav", "-"]
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=seconds + timeout_margin)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AudioError("arecord failed to run: %s" % error)
    if run.returncode != 0 or not run.stdout:
        raise AudioError("arecord exited %d: %s" % (run.returncode, run.stderr.decode("utf-8", "replace").strip()))
    return run.stdout


def parse_wav(data):
    """(channels, rate, [array('h') per channel]) from the bytes of a 16-bit PCM WAV. arecord to a pipe may leave the sizes of its header at their maximum: the data runs to the end then."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AudioError("not a WAV file")
    at = 12
    channels = rate = bits = None
    pcm = None
    while at + 8 <= len(data):
        kind, size = struct.unpack("<4sI", data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b"fmt ":
            if len(body) < 16:
                raise AudioError("the WAV's fmt chunk is cut short")
            tag, channels, rate, _byte_rate, _align, bits = struct.unpack("<HHIIHH", body[:16])
            if tag not in (1, 0xFFFE) or bits != 16:
                raise AudioError("only 16-bit PCM is analysed (format %d, %d bits)" % (tag, bits))
        elif kind == b"data":
            pcm = data[at + 8:at + 8 + size] if at + 8 + size <= len(data) else data[at + 8:]
            break
        at += 8 + size + (size & 1)
    if channels is None or pcm is None:
        raise AudioError("the WAV has no fmt or data chunk")
    frame = 2 * channels
    pcm = pcm[:len(pcm) - len(pcm) % frame]
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder == "big":
        samples.byteswap()
    return channels, rate, [samples[c::channels] for c in range(channels)]


# ---- analysis --------------------------------------------------------------------------------------------------------------------

def _fft(values):
    """In-place radix-2 FFT of a list of complex numbers whose length is a power of two."""
    n = len(values)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            values[i], values[j] = values[j], values[i]
    size = 2
    while size <= n:
        step = cmath.exp(-2j * math.pi / size)
        half = size >> 1
        for start in range(0, n, size):
            w = 1.0
            for k in range(start, start + half):
                even = values[k]
                odd = values[k + half] * w
                values[k] = even + odd
                values[k + half] = even - odd
                w *= step
        size <<= 1


def _db(x):
    return 20.0 * math.log10(x) if x > 0.0 else -200.0


def find_pitch(samples, rate, points=FFT_POINTS):
    """(frequency in Hz, how far above the median of the spectrum it stands in dB), or (None, 0.0). Looks at the loudest `points`-long stretch."""
    n = len(samples)
    if n < points:
        points = 1 << (n.bit_length() - 1) if n >= 256 else 0
    if points < 256:
        return None, 0.0
    best_start, best_energy = 0, -1.0
    for start in range(0, n - points + 1, points // 2):  # the loudest half-overlapping stretch
        energy = sum(abs(samples[i]) for i in range(start, start + points, 16))
        if energy > best_energy:
            best_start, best_energy = start, energy
    if best_energy <= 0:
        return None, 0.0
    mean = sum(samples[best_start:best_start + points]) / points
    if math.sqrt(sum((samples[best_start + i] - mean) ** 2 for i in range(points)) / points) < 3.0:
        return None, 0.0  # nothing but DC and the converter's last bit: no line to speak of
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / points) for i in range(points)]
    values = [complex((samples[best_start + i] - mean) * window[i], 0.0) for i in range(points)]
    _fft(values)
    half = points // 2
    mags = [abs(values[k]) for k in range(half)]
    lo = max(2, int(20.0 * points / rate))  # nothing below 20 Hz is a pitch
    peak = max(range(lo, half - 1), key=lambda k: mags[k])
    if mags[peak] <= 0.0:
        return None, 0.0
    floor = sorted(mags[lo:])[(half - lo) // 2]
    prominence = _db(mags[peak]) - _db(floor) if floor > 0.0 else 200.0
    if prominence < PROMINENCE_DB:
        return None, prominence
    a, b, c = (_db(mags[peak - 1]), _db(mags[peak]), _db(mags[peak + 1]))
    denom = a - 2.0 * b + c
    shift = 0.5 * (a - c) / denom if denom != 0.0 else 0.0
    return (peak + max(-0.5, min(0.5, shift))) * rate / points, prominence


def analyze_channel(samples, rate):
    n = len(samples)
    if n == 0:
        raise AudioError("a channel with no samples")
    peak = max(max(samples), -min(samples))
    mean = sum(samples) / n
    rms = math.sqrt(sum(s * s for s in samples) / n)
    rail = [abs(s) >= RAIL for s in samples]
    clipped = sum(rail)
    run = longest = 0
    for flag in rail:
        run = run + 1 if flag else 0
        longest = max(longest, run)
    pitch, prominence = find_pitch(samples, rate)
    return {
        "samples": n,
        "peak": peak,
        "peak_db": _db(peak / 32768.0),
        "rms_db": _db(rms / 32768.0),
        "dc": mean / 32768.0,
        "clipped": clipped,
        "clip_run": longest,
        "silent": _db(rms / 32768.0) < SILENCE_DB,
        "pitch_hz": pitch,
        "pitch_prominence_db": prominence,
    }


def analyze_wav(data):
    channels, rate, per_channel = parse_wav(data)
    return {"channels": channels, "rate": rate, "results": [analyze_channel(c, rate) for c in per_channel]}


def cents(a, b):
    return 1200.0 * math.log2(a / b)


def check_channel(result, expect):
    """Problems (a list of strings) of one channel's result against `expect`: pitch_hz (+ cents), silent (True / False), min_rms_db, max_peak_db, clipping (False = none allowed)."""
    problems = []
    if "pitch_hz" in expect:
        want = float(expect["pitch_hz"])
        got = result["pitch_hz"]
        tol = float(expect.get("cents", 40.0))
        if got is None:
            problems.append("no pitch found (wanted %.1f Hz)" % want)
        elif abs(cents(got, want)) > tol:
            problems.append("pitch %.1f Hz, wanted %.1f Hz within %.0f cents" % (got, want, tol))
    if expect.get("silent") is True and not result["silent"]:
        problems.append("not silent: RMS %.1f dBFS (floor %.0f)" % (result["rms_db"], SILENCE_DB))
    if expect.get("silent") is False and result["silent"]:
        problems.append("silent: RMS %.1f dBFS" % result["rms_db"])
    if "min_rms_db" in expect and result["rms_db"] < float(expect["min_rms_db"]):
        problems.append("RMS %.1f dBFS is below %.1f" % (result["rms_db"], float(expect["min_rms_db"])))
    if "max_peak_db" in expect and result["peak_db"] > float(expect["max_peak_db"]):
        problems.append("peak %.1f dBFS is above %.1f" % (result["peak_db"], float(expect["max_peak_db"])))
    if expect.get("clipping") is False and (result["clip_run"] >= 3 or result["clipped"] > 10):
        problems.append("clipping: %d samples at the rails, longest run %d" % (result["clipped"], result["clip_run"]))
    return problems


def describe(analysis):
    lines = ["%d channels at %d Hz" % (analysis["channels"], analysis["rate"])]
    for i, r in enumerate(analysis["results"]):
        pitch = "pitch %.1f Hz (%.0f dB above the floor)" % (r["pitch_hz"], r["pitch_prominence_db"]) if r["pitch_hz"] is not None else "no pitch"
        lines.append("  ch%-2d peak %6.1f dBFS  rms %6.1f dBFS  dc %+.4f  clipped %d (run %d)  %s%s" %
                     (i + 1, r["peak_db"], r["rms_db"], r["dc"], r["clipped"], r["clip_run"], pitch, "  SILENT" if r["silent"] else ""))
    return "\n".join(lines)


# ---- command line ----------------------------------------------------------------------------------------------------------------

def expectations(args):
    e = {}
    if args.expect_pitch is not None:
        e["pitch_hz"] = args.expect_pitch
        e["cents"] = args.cents
    if args.expect_silence:
        e["silent"] = True
    if args.not_silent:
        e["silent"] = False
    if args.min_rms is not None:
        e["min_rms_db"] = args.min_rms
    if args.max_peak is not None:
        e["max_peak_db"] = args.max_peak
    if args.no_clipping:
        e["clipping"] = False
    return e


def run_checks(analysis, channel, expect):
    if not expect:
        return 0
    if channel < 0 or channel > analysis["channels"]:
        print("o3audio: channel %d is not in a %d channel capture" % (channel, analysis["channels"]), file=sys.stderr)
        return 2
    failed = 0
    wanted = range(analysis["channels"]) if channel == 0 else [channel - 1]
    for i in wanted:
        problems = check_channel(analysis["results"][i], expect)
        for p in problems:
            print("o3audio: FAIL ch%d: %s" % (i + 1, p))
        failed += 1 if problems else 0
    if not failed:
        print("o3audio: pass")
    return 1 if failed else 0


def add_checks(p):
    p.add_argument("--channel", type=int, default=1, help="1-based channel the checks apply to, 0 for every channel (default 1)")
    p.add_argument("--expect-pitch", type=float, metavar="HZ")
    p.add_argument("--cents", type=float, default=40.0, help="the pitch tolerance in cents (default 40)")
    p.add_argument("--expect-silence", action="store_true")
    p.add_argument("--not-silent", action="store_true")
    p.add_argument("--min-rms", type=float, metavar="DBFS")
    p.add_argument("--max-peak", type=float, metavar="DBFS")
    p.add_argument("--no-clipping", action="store_true")


def main(argv=None):
    parser = argparse.ArgumentParser(description="Record the unit's USB AUDIO device and measure it.")
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list", help="the capture devices")
    r = sub.add_parser("record", help="record and analyse")
    r.add_argument("--device", default="auto", help="alsa device (hw:CARD,DEV) or auto (the one that names the unit)")
    r.add_argument("--seconds", type=float, default=3.0)
    r.add_argument("--channels", type=int, default=None, help="the profile's channel count (2, 8, 10 or 11); default: what the device reports in /proc/asound, else 8")
    r.add_argument("--rate", type=int, default=48000)
    r.add_argument("--save", metavar="OUT.wav")
    r.add_argument("--arecord", default="arecord")
    add_checks(r)
    a = sub.add_parser("analyze", help="analyse a WAV file")
    a.add_argument("file")
    add_checks(a)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        if args.command == "list":
            devices = list_devices()
            for hw, line in devices:
                print("%-8s %s" % (hw, line))
            print("the unit: %s" % (find_unit(devices) or "not found"))
            return 0
        if args.command == "record":
            data = record(args.device, args.seconds, args.channels, args.rate, args.arecord)
            if args.save:
                with open(args.save, "wb") as f:
                    f.write(data)
        elif args.command == "analyze":
            with open(args.file, "rb") as f:
                data = f.read()
        else:
            parser.print_usage(sys.stderr)
            return 2
        analysis = analyze_wav(data)
    except (AudioError, OSError) as error:
        print("o3audio: %s" % error, file=sys.stderr)
        return 2
    print(describe(analysis))
    return run_checks(analysis, args.channel, expectations(args))


# ---- self-test -------------------------------------------------------------------------------------------------------------------

def make_wav(channel_samples, rate=48000, streaming=False):
    """A 16-bit WAV of the channels (lists of ints). `streaming`: the sizes in the header are the maximum, as arecord leaves them on a pipe."""
    channels = len(channel_samples)
    frames = len(channel_samples[0])
    interleaved = array.array("h", [0]) * (frames * channels)
    for c, chan in enumerate(channel_samples):
        interleaved[c::channels] = array.array("h", chan)
    pcm = interleaved.tobytes()
    size = 0x7FFFF000 if streaming else len(pcm)
    return (b"RIFF" + struct.pack("<I", 0x7FFFF024 if streaming else 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) +
            b"data" + struct.pack("<I", size) + pcm)


def sine(freq, seconds, rate=48000, amp=0.5, phase=0.0):
    return [int(round(amp * 32767 * math.sin(2.0 * math.pi * freq * i / rate + phase))) for i in range(int(seconds * rate))]


def self_test():
    problems = []
    rate = 48000
    # pitch and level of sines across the range, with a little noise: within 5 cents
    state = 12345
    for freq in (55.0, 220.0, 440.0, 1000.0, 3520.0, 12000.0):
        base = sine(freq, 0.5, rate, 0.25)
        noisy = []
        for v in base:
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            noisy.append(max(-32768, min(32767, v + (state % 201) - 100)))
        got = analyze_channel(array.array("h", noisy), rate)
        if got["pitch_hz"] is None or abs(cents(got["pitch_hz"], freq)) > 5.0:
            problems.append("pitch of %.0f Hz: %r" % (freq, got["pitch_hz"]))
        if abs(got["peak_db"] - _db(0.25)) > 0.5 or abs(got["rms_db"] - (_db(0.25) - 3.01)) > 0.5:
            problems.append("level of %.0f Hz: peak %.2f rms %.2f" % (freq, got["peak_db"], got["rms_db"]))
        if got["silent"] or got["clipped"] != 0:
            problems.append("a quiet sine was silent or clipped")
    # silence, DC, noise (no pitch), clipping
    flat = analyze_channel(array.array("h", [0] * 24000), rate)
    if not flat["silent"] or flat["pitch_hz"] is not None or flat["peak_db"] > -100.0:
        problems.append("silence was not silence: %r" % flat)
    dc = analyze_channel(array.array("h", [3277] * 24000), rate)
    if abs(dc["dc"] - 0.1) > 0.001 or dc["pitch_hz"] is not None:
        problems.append("a DC offset was not seen: %r" % dc)
    state = 777
    noise = []
    for _ in range(24000):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise.append((state >> 8) % 20001 - 10000)
    nz = analyze_channel(array.array("h", noise), rate)
    if nz["pitch_hz"] is not None:
        problems.append("white noise has a pitch: %r" % nz["pitch_hz"])
    clip = analyze_channel(array.array("h", [max(-32768, min(32767, v)) for v in sine(440.0, 0.5, rate, 1.6)]), rate)
    if clip["clip_run"] < 3 or clip["clipped"] < 100 or check_channel(clip, {"clipping": False}) == []:
        problems.append("clipping was not found: %r" % clip)
    if check_channel(analyze_channel(array.array("h", sine(440.0, 0.5, rate, 0.5)), rate), {"clipping": False, "pitch_hz": 440, "silent": False, "max_peak_db": -3.0, "min_rms_db": -12.0}):
        problems.append("a clean sine failed its checks")
    bad = check_channel(analyze_channel(array.array("h", sine(466.16, 0.5, rate, 0.5)), rate), {"pitch_hz": 440, "cents": 40})
    if not bad or "pitch" not in bad[0]:
        problems.append("a pitch a semitone away passed a 40 cents check")
    if not check_channel(flat, {"silent": False}) or check_channel(flat, {"silent": True}):
        problems.append("the silence checks are wrong")
    if not check_channel(analyze_channel(array.array("h", sine(440.0, 0.5, rate, 0.9)), rate), {"max_peak_db": -6.0}):
        problems.append("a peak above the limit passed")
    # a multi channel WAV: each channel keeps its own signal; a header with the streaming sizes is read to the end
    wav = make_wav([sine(440.0, 0.5, rate, 0.4), [0] * 24000, sine(880.0, 0.5, rate, 0.1)], rate, streaming=True)
    analysis = analyze_wav(wav)
    res = analysis["results"]
    if analysis["channels"] != 3 or analysis["rate"] != rate or abs(cents(res[0]["pitch_hz"], 440.0)) > 5 or not res[1]["silent"] or abs(cents(res[2]["pitch_hz"], 880.0)) > 5:
        problems.append("the channels of a WAV were mixed up: %s" % describe(analysis))
    if "ch1 " not in describe(analysis) or "SILENT" not in describe(analysis):
        problems.append("describe() lost a channel")
    for text, wanted in ((b"junk", "not a WAV"), (make_wav([[1, 2]])[:30], "cut short"), (make_wav([[1, 2]])[:12], "no fmt or data")):
        try:
            parse_wav(text)
            problems.append("a broken WAV was accepted")
        except AudioError as error:
            if wanted not in str(error):
                problems.append("wrong error for a broken WAV: %s" % error)
    # arecord as a subprocess: a stand-in on the path that records a file, and one that fails
    with tempfile.TemporaryDirectory() as tmp:
        good_wav = os.path.join(tmp, "in.wav")
        with open(good_wav, "wb") as f:
            f.write(make_wav([sine(1000.0, 1.0, rate, 0.3)] * 2, rate, streaming=True))
        fake = os.path.join(tmp, "arecord")
        with open(fake, "w") as f:
            f.write("#!/bin/sh\nif [ \"$1\" = \"-l\" ]; then\n  echo 'card 0: PCH [HDA Intel PCH], device 0: ALC [ALC]'\n  echo 'card 3: Track3r [Track3r], device 0: USB Audio [USB Audio]'\n  exit 0\nfi\n"
                    "echo \"$@\" > %s/args.txt\ncat %s\n" % (tmp, good_wav))
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
        devices = list_devices(fake)
        if find_unit(devices) != "hw:3,0" or len(devices) != 2:
            problems.append("the unit was not found among %r" % devices)
        wav2 = record("auto", 1.0, 2, 48000, fake)
        with open(os.path.join(tmp, "args.txt")) as f:
            args_text = f.read()
        if "-D hw:3,0" not in args_text or "-c 2" not in args_text or "-f S16_LE" not in args_text or "-r 48000" not in args_text or "-d 1" not in args_text:
            problems.append("arecord was given %r" % args_text)
        a2 = analyze_wav(wav2)
        if a2["channels"] != 2 or abs(cents(a2["results"][0]["pitch_hz"], 1000.0)) > 5:
            problems.append("the recorded stand-in was not analysed")
        # no --channels: the card's own capture channel count (snd-usb-audio's stream file: the Playback section's 2 must not be taken for the capture's 10), else 8
        stream = ("Track3r Track3r at usb-0000:00:14.0-1, full speed : USB Audio\n\nPlayback:\n  Status: Stop\n  Interface 4\n    Altset 1\n    Format: S16_LE\n    Channels: 2\n"
                  "    Endpoint: 0x04 (4 OUT) (ASYNC)\n\nCapture:\n  Status: Stop\n  Interface 3\n    Altset 1\n    Format: S16_LE\n    Channels: 10\n    Endpoint: 0x84 (4 IN) (ASYNC)\n")
        if parse_capture_channels(stream) != 10 or parse_capture_channels(stream.split("Capture:")[0]) is not None or parse_capture_channels("") is not None:
            problems.append("the stream file was not parsed: %r" % parse_capture_channels(stream))
        proc = os.path.join(tmp, "asound")
        os.makedirs(os.path.join(proc, "card3"))
        with open(os.path.join(proc, "card3", "stream0"), "w") as f:
            f.write(stream)
        if capture_channels("hw:3,0", proc) != 10 or capture_channels("plughw:3,0", proc) != 10 or capture_channels("hw:4,0", proc) is not None or capture_channels("default", proc) is not None:
            problems.append("the card's capture channels were not found")
        record("auto", 1.0, None, 48000, fake, proc=proc)
        with open(os.path.join(tmp, "args.txt")) as f:
            if "-c 10" not in f.read():
                problems.append("--channels left out did not follow the device")
        record("auto", 1.0, None, 48000, fake, proc=os.path.join(tmp, "no-asound"))
        with open(os.path.join(tmp, "args.txt")) as f:
            if "-c 8" not in f.read():
                problems.append("--channels left out with no proc entry was not 8")
        record("auto", 1.0, 2, 48000, fake, proc=proc)
        with open(os.path.join(tmp, "args.txt")) as f:
            if "-c 2" not in f.read():
                problems.append("an explicit --channels was overridden")
        failing = os.path.join(tmp, "arecord_fail")
        with open(failing, "w") as f:
            f.write("#!/bin/sh\necho 'no such device' >&2\nexit 1\n")
        os.chmod(failing, os.stat(failing).st_mode | stat.S_IXUSR)
        try:
            record("hw:9,9", 1.0, 2, 48000, failing)
            problems.append("a failing arecord was not an error")
        except AudioError as error:
            if "no such device" not in str(error):
                problems.append("arecord's message was lost: %s" % error)
        try:
            record("auto", 1.0, 2, 48000, os.path.join(tmp, "missing"))
            problems.append("a missing arecord was not an error")
        except AudioError:
            pass
        # the command line end to end on a saved file: pass and fail exit codes
        path = os.path.join(tmp, "t.wav")
        with open(path, "wb") as f:
            f.write(make_wav([sine(440.0, 1.0, rate, 0.4)], rate))
        quiet_out, quiet_err = sys.stdout, sys.stderr
        try:
            sys.stdout = sys.stderr = open(os.devnull, "w")
            ok = main(["analyze", path, "--expect-pitch", "440", "--no-clipping", "--min-rms", "-20"])
            fail = main(["analyze", path, "--expect-pitch", "523.25"])
            missing = main(["analyze", os.path.join(tmp, "nope.wav")])
        finally:
            sys.stdout.close()
            sys.stdout, sys.stderr = quiet_out, quiet_err
        if (ok, fail, missing) != (0, 1, 2):
            problems.append("exit codes %r, wanted (0, 1, 2)" % ((ok, fail, missing),))
    if problems:
        print("o3audio self-test FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print("o3audio self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
