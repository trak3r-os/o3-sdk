#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3hil: hardware-in-the-loop scenarios for the Tracker over its USB `diag` console (OS-HIL; stdlib only; POSIX).

A scenario is a JSON file of steps (YAML is not in the standard library). The runner drives the unit through the console commands the firmware gives a test host: `key`, `pad`,
`enc` (input injection into the scanner's own queue), `state` (the app as one key=value line), `shot` (the screen, decoded by o3shot.py and compared with the simulator's
picture), `leds`, `mix` and `voices` (the audio graph's meters and the sampler), and, over the USB AUDIO device, o3audio.py (level, pitch, silence, clipping). It prints a
pass / fail line per step and per scenario and exits 0 only when nothing failed.

    o3hil.py list                                        the scenarios in hil/ next to this file, with their titles
    o3hil.py validate [SCENARIO ...]                     check the files without a unit: step names, regular expressions, button names, references
    o3hil.py run SCENARIO ... --port /dev/ttyACM0 [--audio-device auto|hw:3,0] [--midi-device /dev/snd/midiC1D0] [--sim build-os-host/sim/o3sim] [--expect-volume ABCD-1234] [--report report.json] [--var NAME=VALUE ...]
    o3hil.py --self-test                                 against a pseudo-terminal that plays a unit: the runner itself, no hardware

ONE SESSION OWNS THE PORT AT A TIME: run this while nothing else has the console open. It never sends `fw` commands other than the read-only `fw` status, and a step that
could write the card (the save scenario) first checks, with `fw`, that a card is mounted and not write protected (and, when --expect-volume is given, that it is that volume; any SD card may be written, there is no marker file: OS-ANYCARD).

Scenario file (all keys but "steps" optional):
  {"name": "...", "title": "...", "needs": ["audio", "sim", "card"], "steps": [...], "finally": [...]}
needs: "audio" (a capture device: --audio-device), "midi" (a raw MIDI node of the unit's MIDI out: --midi-device), "sim" (the simulator binary for goldens: --sim, or PNGs in --golden-dir), "card" (a card in the unit: the scenario's `card` step checks it; --expect-volume is optional and only adds the check of its volume serial),
"disk" (the unit as a USB disk on THIS computer, o3disk.py: Linux only; combine it with "card"; --expect-volume, when given, must be the volume of the unit's disk). A scenario whose needs
are not there is SKIPPED, not failed (--strict makes it a failure), except "disk" off Linux: Windows and macOS are UNTESTED, so the scenario is "not run, not failed" even with --strict. "finally" steps run after the steps, whatever happened. A step {"run": "_name"} splices the steps of
hil/_name.json (a snippet; its own "finally" is spliced into the scenario's).

A scenario may declare "vars": {"RUN": "2"} (defaults); --var NAME=VALUE overrides one for the whole run (a name the scenario does not declare is an error). `${NAME}` inside any
string of a step is replaced by the value, `${NAME-1}` / `${NAME+1}` by the value minus / plus a number; a field that is exactly one such token and a whole number becomes an int ("pad": "${RUN-1}").

Steps (each one object; "note" is printed; "soft": true reports a failure but does not stop the scenario; "if_state": {"playing": "1"} runs the step only while
`state` has those values, "~regex" allowed: a way to stop what runs only if it runs):
  {"cmd": "version", "expect": ["regex", ...], "expect_not": ["regex", ...], "timeout": 3, "within": 30, "poll": 1.0, "fail_on": "regex", "show": true, "if_reply_not": "regex"}
        run a console command. "expect" / "expect_not": regular expressions searched in the reply (a string is one). With "within" the command is repeated every "poll"
        seconds until every "expect" matches (a "fail_on" match ends it early as a failure). "show": the reply of the passing run is printed, line by line after a `|`, and kept
        in the --report (key "reply" of the step): the way a scenario CAPTURES a reading (`diag mod`, `diag bench`) for the person who reads the log.
        "if_reply_not": a regex; a reply that matches it makes the step pass as "skipped: <the regex>" without its other checks (`"not in this image"`: a command a release
        image lacks, such as `bench`, which only an -DO3SDK_BENCH=ON image has).
  {"cmd": "cpu", "collect": {"as": "U", "re": "render last (\\d+)"}}
        "collect": keep the first group of the regex (a number) of the reply under the name "as", for a "stat" step (a mean the console does not print: `cpu` gives last and max only).
  {"stat": "U", "min_n": 12, "show": true}
        the count, mean, min and max of what "collect" kept under that name (a failure with fewer than min_n values), then the series is emptied; "show" prints and keeps the line.
  {"keys": ["play", "+shift", "f7", "-shift", "song:600"], "repeat": 1, "gap": 0.3}
        buttons by name or code (kit/app/include/o3sdk/app/panel.h): "name" taps, "+name" holds down, "-name" releases, "name:ms" holds for ms (the runner waits it out).
  {"keys": ["f8"], "until": {"msg": "~^NOTHING_TO_UNDO$"}, "max": 60, "gap": 0.25}
        repeat the keys until `state` matches every pair of "until" (as "state" does), checked after each round, at most "max" rounds (1..200; a step that has not
        got there is a failure); "until" replaces "repeat". It drains a history of unknown depth (the undo journal also holds what was done by hand).
  {"keys": ["+f7", "+f8", "-f8", "-f7"], "burst": true}
        a chord: with "burst" every token is sent as its own console command back to back, with no "gap" between them (the unit answers each in a few ms, as in one
        `o3console "key f7 down" "key f8 down" "key f8 up" "key f7 up"`), so the second button lands inside the firmware's 40 ms chord window (kGestureTimings.chord_ms,
        core/include/o3sdk/core/gesture.h). "gap" then only separates the rounds of "repeat" / "until", which burst allows. A "name:ms" hold waits out its time and
        so cannot be part of a burst (refused).
  {"pad": 5, "gesture": "tap|down|up", "vel": 100}      {"enc": -3, "repeat": 1, "gap": 0.2}
        the encoder: `enc` detents at once, `repeat` times with `gap` seconds after each. The unit accelerates a burst of detents (asked 40, got 100), so a value that must land
        exactly is turned one detent at a time: {"enc": 1, "repeat": 40, "gap": 0.25}.
  {"wait": 1.5}
  {"state": {"screen": "PATTERN", "playing": "1", "msg": "~DONE$"}, "ne": {"row": "0"}, "within": 3, "poll": 0.3, "snap": "name"}
        poll `state` until every pair matches ("~regex" matches by search, anything else exactly) and no "ne" pair does; "snap" keeps the line for later steps.
  {"state": {"msg": "~^SAVE_TRACK_[0-9]+_DONE$"}, "keep": {"as": "SAVED", "field": "msg", "re": "^SAVE_TRACK_([0-9]+)_DONE$"}}
        "keep": once the pairs match, the first group of the regex over that `state` field is kept under the name "as" (a failure when it does not match); a later "state" / "ne" /
        "if_state" value, or "cmd" line (`ls Projects/Track {{SAVED}}`), may say {{SAVED}}, which is replaced by the kept text (a name nothing kept is a failure). The way a scenario acts on the one thing it made, not on any of its kind.
        The names are forgotten at the start of each scenario.
  {"mix": {"min_tracks": 4, "channels": {"MAIN L": {"min_rms_db": -50, "max_peak_db": -0.5}}, "snap": "a", "drop": {"channel": "MAIN L", "field": "rms", "from": "a", "min_db": 6},
           "rise": {...}, "within": 5}}
        parse `mix`: tracks T01.. whose meter has sounded, per channel limits, a snapshot, and a change against an earlier snapshot (field: pk, hold or rms).
  {"leds": {"min_lit": 1, "max_lit": 48, "sync": true, "snap": "a", "differs_from": "a", "within": 3, "lit": [12, 17], "unlit": [13]}}
        "lit" / "unlit": pads (0..47) whose level is not 0 / is 0 (a pad's state read back, e.g. the MUTE row 12..23 and the SOLO row 36..47 while Shift is held).
  any step may carry "if_leds": {"lit": [12], "unlit": [13]}: it runs only while those pads are lit / unlit (one `leds` read before the step), so a toggle is made only when the state is wrong.
  {"voices": {"min_in_use": 1, "max_steals": 0}}
  {"shot": "name", "compare": {"sim_scenario": "sim/scenarios/screens.txt", "snap": "pattern_start", "regions": ["0,458,800,22"], "ignore": [], "tol": 24, "aa": 1, "max_bad": 0.005},
   "non_blank": true, "format": "565"}
        take the screen into <out>/<scenario>_<name>.png; "compare" holds it against the simulator's picture of the same screen ("png": a file instead of the simulator).
  {"audio": {"seconds": 2, "channels": 10 (omit: the device's own count), "channel": 9, "pitch_hz": 440, "cents": 40, "silent": false, "min_rms_db": -50, "max_peak_db": -1, "clipping": false}}
  {"midi": {"start": true}}     {"midi": {"on": [60, 64, 67], "exact": false, "relative": false, "channel": 1, "settle": 0.3, "off": false, "timeout": 2}}
        the unit's MIDI out over a raw MIDI node (--midi-device /dev/snd/midiC<card>D<dev>; needs "midi"). "start" empties the buffer and starts listening in the background;
        the second form stops listening and compares the NOTE-ONs received (velocity > 0; clock and other real-time bytes are ignored) with "on", in order: the first
        len(on) of them, or all of them with "exact": true; with "relative": true "on" are semitones above the FIRST note received (it starts with 0: the way to check a chord or an
        arpeggio without knowing the octave the grid shows or the MIDI out transposition); "channel" (1..16) checks the channel of each. "settle" seconds are waited before it stops (default 0.3).
        "off": true also checks the NOTE-OFFs: every note-on counted above (the first len(on), or all with "exact") needs a later note-off of the same note and channel (a status 0x8n message, or a
        note-on of velocity 0), the one note-off closing the oldest note-on still open; the buffer is polled for up to "timeout" seconds (0.1..60, default 2) after the settle for the ones still missing
        (a note that ends late: a long note, the stop), and the step fails naming the notes that never got one. Without "off" the note-offs are not looked at (as before).
  {"card": {}}                                          a card is mounted in the unit and not write protected, and is --expect-volume's when that was given (checks the `fw` status line; writes nothing)
  {"disk": "enumerate", "within": 30, "ro": true}       OS-USBDISK U1d, the PC side (o3disk.py, Linux): waits for the unit's USB disk (16d0:0e9f, bcdDevice 0002, a medium) and checks the write-protect bit the computer sees
  {"disk": "mount", "within": 30}                       mounts it (udisksctl, no root) when it is not, and proves it is the unit's removable disk (and --expect-volume's, when that was given); no marker file is needed
  {"disk": "read", "size_mb": 8}                        copies files (the largest first, up to size_mb MiB) off the card and checks every copy against two cache-dropped reads of the card
  {"disk": "write", "size_mb": 8}                       writes a deterministic file into <mount>/o3-usbdisk-test/ (the ONLY place the PC side writes), unmounts and mounts again, reads it back and compares the checksum; refuses a read-only disk
  {"disk": "remount"}                                   unmounts and mounts again (the computer's cache is gone, the unit sees no eject)
  {"disk": "eject", "within": 20}                       the computer ejects the medium (udisksctl power-off: START STOP UNIT); the block device is gone or reads 0 blocks
  {"disk": "gone", "within": 20}                        the unit's disk is no longer on the bus (the unit restarted)

Exit status: 0 everything that ran passed, 1 a step failed, 2 usage, a scenario file, or no port, 3 no prompt from the unit.
"""
import argparse
import json
import os
import re
import select
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PLATFORM = sys.platform  # (a seam: the self-test sets it to see the Windows / macOS answer)
DISK_OPS = ("enumerate", "mount", "read", "write", "remount", "eject", "gone")
SCENARIO_DIR = os.path.join(HERE, "hil")
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

_MODULES = {}


def sibling(name):
    """o3console / o3shot / o3audio as modules (python -I puts no script directory on the path)."""
    if name not in _MODULES:
        import importlib.util

        spec = importlib.util.spec_from_file_location("o3_" + name, os.path.join(HERE, name + ".py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _MODULES[name] = module
    return _MODULES[name]


# The button names of kit/app/src/panel.cpp (the self-test compares this list with the kit's table when the tree is there).
BUTTONS = ["f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "params", "performance", "file", "config", "master", "sampleload", "sampleplay", "sampleedit", "samplerec", "song",
           "note", "instr", "vol", "fx", "pattern", "play", "copy", "insert", "up", "delete", "rec", "shift", "left", "down", "right"]

STEP_KINDS = ("cmd", "keys", "pad", "enc", "wait", "state", "mix", "leds", "voices", "stat", "shot", "audio", "midi", "card", "disk", "run", "say")
COMMON_KEYS = ("note", "soft", "if_state", "if_leds")
KEEP_NAME = r"[A-Za-z_][A-Za-z0-9_]*"  # a name for `keep`; a state value says {{NAME}}
ALLOWED_KEYS = {
    "cmd": {"cmd", "expect", "expect_not", "timeout", "within", "poll", "fail_on", "show", "if_reply_not", "collect"},
    "keys": {"keys", "repeat", "gap", "until", "max", "burst"},
    "pad": {"pad", "gesture", "vel"},
    "enc": {"enc", "repeat", "gap"},
    "wait": {"wait"},
    "state": {"state", "ne", "within", "poll", "snap", "keep"},
    "mix": {"mix"},
    "leds": {"leds"},
    "stat": {"stat", "min_n", "show"},
    "voices": {"voices"},
    "shot": {"shot", "compare", "non_blank", "format"},
    "audio": {"audio"},
    "midi": {"midi"},
    "card": {"card"},
    "disk": {"disk", "within", "ro", "size_mb"},
    "run": {"run"},
    "say": {"say"},
}


# ---- parsing what the unit says ---------------------------------------------------------------------------------------------------

def parse_pairs(line):
    out = {}
    for word in line.split():
        if "=" in word:
            key, _, value = word.partition("=")
            out[key] = value
    return out


def parse_state(text):
    for line in text.splitlines():
        if line.startswith("screen=") or line.startswith("app="):
            return parse_pairs(line)
    return None


def parse_leds(text):
    for line in text.splitlines():
        if line.startswith("leds "):
            d = parse_pairs(line)
            levels = d.get("levels", "")
            if len(levels) != 96:
                return None
            d["levels"] = [int(levels[i:i + 2], 16) for i in range(0, 96, 2)]
            for k in ("online", "errors", "reinits", "lit", "sync"):
                d[k] = int(d.get(k, "0"))
            return d
    return None


_MIX = re.compile(r"^\s*(T\d\d|IN [LR]|MAIN [LR]|CUE [LR]|SNDA|SNDB|REC [LR]|MST [LR])\s+pk\s+(\S+)\s+hold\s+(\S+)\s+rms\s+(\S+)\s+clip\s+(\d+)\s*$", re.MULTILINE)


def leds_pads_problem(leds, spec):
    """None when every pad of spec["lit"] has a level and every pad of spec["unlit"] has none; else the first that does not."""
    for n in spec.get("lit", []):
        if leds["levels"][n] == 0:
            return "pad %d is dark, lit wanted" % n
    for n in spec.get("unlit", []):
        if leds["levels"][n] != 0:
            return "pad %d is lit (level %d), dark wanted" % (n, leds["levels"][n])
    return None


def _db(text):
    return -200.0 if text.endswith("inf") else float(text)


def parse_mix(text):
    """{channel: {"pk": dB, "hold": dB, "rms": dB, "clip": n}} of the channels the unit lists (-200 stands for -inf)."""
    out = {}
    for m in _MIX.finditer(text):
        out[m.group(1)] = {"pk": _db(m.group(2)), "hold": _db(m.group(3)), "rms": _db(m.group(4)), "clip": int(m.group(5))}
    return out


_VOICES = re.compile(r"voices\s+in use (\d+)/(\d+)\s+peak (\d+)\s+steals (\d+)")


def parse_voices(text):
    m = _VOICES.search(text)
    return {"in_use": int(m.group(1)), "pool": int(m.group(2)), "peak": int(m.group(3)), "steals": int(m.group(4))} if m else None


# ---- scenarios --------------------------------------------------------------------------------------------------------------------

class ScenarioError(Exception):
    pass


def scenario_path(name, directory=SCENARIO_DIR):
    if os.path.isfile(name):
        return name
    path = os.path.join(directory, name if name.endswith(".json") else name + ".json")
    if os.path.isfile(path):
        return path
    raise ScenarioError("no scenario %r (looked for it as a file and in %s)" % (name, directory))


_VAR = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?:([+-])(\d+))?\}")


def _substitute(value, variables, path):
    if isinstance(value, str):
        def one(m):
            if m.group(1) not in variables:
                raise ScenarioError("%s: ${%s} is not declared in the scenario's vars" % (path, m.group(1)))
            text = variables[m.group(1)]
            if m.group(2):
                if not re.fullmatch(r"-?\d+", text):
                    raise ScenarioError("%s: ${%s%s%s}: %r is not a whole number" % (path, m.group(1), m.group(2), m.group(3), text))
                text = str(int(text) + (int(m.group(3)) if m.group(2) == "+" else -int(m.group(3))))
            return text
        out = _VAR.sub(one, value)
        if out != value and _VAR.fullmatch(value) and re.fullmatch(r"-?\d+", out):
            return int(out)
        return out
    if isinstance(value, list):
        return [_substitute(v, variables, path) for v in value]
    if isinstance(value, dict):
        return {k: _substitute(v, variables, path) for k, v in value.items()}
    return value


def load_scenario(name, directory=SCENARIO_DIR, _depth=0, variables=None):
    """The scenario as a dict with its snippets spliced in and its ${vars} replaced (variables: {name: text} overrides of the scenario's own "vars"). ScenarioError for anything that is not a scenario."""
    path = scenario_path(name, directory)
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError) as error:
        raise ScenarioError("%s: %s" % (path, error))
    if not isinstance(data, dict) or not isinstance(data.get("steps"), list):
        raise ScenarioError("%s: a scenario is an object with a list of steps" % path)
    data.setdefault("name", os.path.splitext(os.path.basename(path))[0])
    data["_path"] = path
    for section in ("steps", "finally"):
        data[section] = _splice(data.get(section, []), directory, _depth, path, data)
    declared = data.get("vars", {})
    if not isinstance(declared, dict) or any(not isinstance(v, str) for v in declared.values()):
        raise ScenarioError("%s: vars is an object of name: text" % path)
    merged = dict(declared)
    merged.update({k: v for k, v in (variables or {}).items() if k in declared})
    if _depth == 0:
        for section in ("steps", "finally"):
            data[section] = _substitute(data[section], merged, path)
        data["vars"] = merged
    return data


def _splice(steps, directory, depth, path, scenario):
    out = []
    for step in steps:
        if isinstance(step, dict) and "run" in step:
            if depth > 4:
                raise ScenarioError("%s: snippets nest too deep (a loop?)" % path)
            snippet = load_scenario(step["run"], directory, depth + 1)
            out.extend(snippet["steps"])
            scenario.setdefault("_snippet_finally", []).extend(snippet["finally"])
        else:
            out.append(step)
    return out


def validate_step(step, where):
    """A list of problems (strings) of one step."""
    problems = []
    if not isinstance(step, dict):
        return ["%s: a step is an object" % where]
    kinds = [k for k in step if k in STEP_KINDS]
    if len(kinds) != 1:
        return ["%s: a step has exactly one of %s (found %s)" % (where, ", ".join(STEP_KINDS), kinds or "none")]
    kind = kinds[0]
    for key in step:
        if key not in ALLOWED_KEYS[kind] and key not in COMMON_KEYS:
            problems.append("%s: %s step has no key %r" % (where, kind, key))
    def regexes(value, label):
        for r in ([value] if isinstance(value, str) else value):
            try:
                re.compile(r)
            except (re.error, TypeError) as error:
                problems.append("%s: %s %r: %s" % (where, label, r, error))
    if kind == "cmd":
        if not isinstance(step["cmd"], str) or not step["cmd"].strip():
            problems.append("%s: cmd is a command line" % where)
        else:
            word = step["cmd"].split()[0]
            if word in ("reset", "fw", "poke", "audio") and not (word == "fw" and step["cmd"].strip() == "fw"):
                problems.append("%s: a scenario may not send %r (it changes the unit)" % (where, step["cmd"]))
        for k in ("expect", "expect_not", "fail_on", "if_reply_not"):
            if k in step:
                regexes(step[k], k)
        if "show" in step and not isinstance(step["show"], bool):
            problems.append("%s: show is true or false" % where)
    elif kind == "keys":
        if not isinstance(step["keys"], list) or not step["keys"]:
            problems.append("%s: keys is a list of buttons" % where)
        else:
            for token in step["keys"]:
                name = str(token).lstrip("+-").split(":")[0]
                if name not in BUTTONS and not (name.isdigit() and int(name) < 33):
                    problems.append("%s: %r is not a button" % (where, token))
                if ":" in str(token) and not str(token).split(":")[1].isdigit():
                    problems.append("%s: the hold of %r is a number of ms" % (where, token))
        if "burst" in step:
            if not isinstance(step["burst"], bool):
                problems.append("%s: burst is true or false" % where)
            elif step["burst"] and any(":" in str(token) for token in step["keys"] if isinstance(step["keys"], list)):
                problems.append("%s: burst cannot hold a name:ms token (the hold waits out its time)" % where)
        if "until" in step:
            if not isinstance(step["until"], dict) or not step["until"]:
                problems.append("%s: until is an object of key: value" % where)
            else:
                for value in step["until"].values():
                    if str(value).startswith("~"):
                        regexes(str(value)[1:], "until")
            if "repeat" in step or not isinstance(step.get("max"), int) or not 1 <= step["max"] <= 200:
                problems.append("%s: until replaces repeat and needs max, 1..200 rounds" % where)
        elif "max" in step:
            problems.append("%s: max belongs to until" % where)
    elif kind == "pad":
        if not isinstance(step["pad"], int) or not 0 <= step["pad"] <= 47 or step.get("gesture", "tap") not in ("tap", "down", "up"):
            problems.append("%s: pad is 0..47 with a gesture tap, down or up" % where)
    elif kind == "enc":
        if not isinstance(step["enc"], int) or step["enc"] == 0 or abs(step["enc"]) > 64:
            problems.append("%s: enc is -64..64, not 0" % where)
        if not isinstance(step.get("repeat", 1), int) or not 1 <= step.get("repeat", 1) <= 200 or not isinstance(step.get("gap", 0.2), (int, float)) or not 0 <= step.get("gap", 0.2) <= 10:
            problems.append("%s: enc repeat is 1..200 and gap 0..10 seconds" % where)
    elif kind == "midi":
        spec = step["midi"]
        if not isinstance(spec, dict) or ("start" in spec) == ("on" in spec):
            problems.append("%s: midi is {\"start\": true} or {\"on\": [notes], ...}" % where)
        elif "on" in spec and (not isinstance(spec["on"], list) or not spec["on"] or any(not isinstance(n, int) or not 0 <= n <= 127 for n in spec["on"])):
            problems.append("%s: midi on is a list of notes 0..127" % where)
        elif "channel" in spec and (not isinstance(spec["channel"], int) or not 1 <= spec["channel"] <= 16):
            problems.append("%s: midi channel is 1..16" % where)
        elif spec.get("relative") and spec["on"][0] != 0:
            problems.append("%s: a relative midi list starts with 0" % where)
        elif "off" in spec and (not isinstance(spec["off"], bool) or "on" not in spec):
            problems.append("%s: midi off is true or false, and belongs to the {\"on\": [notes]} form" % where)
        elif "timeout" in spec and (not spec.get("off") or isinstance(spec["timeout"], bool) or not isinstance(spec["timeout"], (int, float)) or not 0.1 <= spec["timeout"] <= 60):
            problems.append("%s: midi timeout is 0.1..60 seconds and belongs to \"off\": true" % where)
        elif set(spec) - {"start", "on", "exact", "relative", "channel", "settle", "off", "timeout"}:
            problems.append("%s: midi has no key %s" % (where, sorted(set(spec) - {"start", "on", "exact", "relative", "channel", "settle", "off", "timeout"})))
    elif kind == "wait":
        if not isinstance(step["wait"], (int, float)) or step["wait"] < 0 or step["wait"] > 600:
            problems.append("%s: wait is 0..600 seconds" % where)
    elif kind == "disk":
        if step["disk"] not in DISK_OPS:
            problems.append("%s: disk is one of %s" % (where, ", ".join(DISK_OPS)))
        if "within" in step and (not isinstance(step["within"], (int, float)) or not 0 <= step["within"] <= 600):
            problems.append("%s: disk within is 0..600 seconds" % where)
        if "ro" in step and not isinstance(step["ro"], bool):
            problems.append("%s: disk ro is true or false" % where)
        if "size_mb" in step and (not isinstance(step["size_mb"], int) or not 1 <= step["size_mb"] <= 1024):
            problems.append("%s: disk size_mb is 1..1024" % where)
        if "ro" in step and step["disk"] != "enumerate":
            problems.append("%s: ro belongs to enumerate" % where)
        if "size_mb" in step and step["disk"] not in ("read", "write"):
            problems.append("%s: size_mb belongs to read and write" % where)
    for key in ("if_leds",) + (("leds",) if kind == "leds" else ()):
        spec = step.get(key)
        if spec is None:
            continue
        keys = ("lit", "unlit") if key == "if_leds" else ("lit", "unlit", "online", "min_lit", "max_lit", "sync", "snap", "differs_from", "within")
        if not isinstance(spec, dict) or (key == "if_leds" and not spec) or set(spec) - set(keys):
            problems.append("%s: %s is an object with %s" % (where, key, " / ".join(keys[:2])))
            continue
        for k in ("lit", "unlit"):
            if k in spec and (not isinstance(spec[k], list) or any(not isinstance(n, int) or isinstance(n, bool) or not 0 <= n <= 47 for n in spec[k])):
                problems.append("%s: %s %s is a list of pads 0..47" % (where, key, k))
    if kind == "stat":
        if not isinstance(step["stat"], str) or not step["stat"]:
            problems.append("%s: stat is the name a collect step used" % where)
        if "min_n" in step and (not isinstance(step["min_n"], int) or isinstance(step["min_n"], bool) or not 1 <= step["min_n"] <= 10000):
            problems.append("%s: stat min_n is 1..10000" % where)
        if "show" in step and not isinstance(step["show"], bool):
            problems.append("%s: show is true or false" % where)
    if kind == "cmd" and "collect" in step:
        spec = step["collect"]
        if not isinstance(spec, dict) or set(spec) != {"as", "re"} or not isinstance(spec["as"], str) or not spec["as"] or not isinstance(spec["re"], str):
            problems.append("%s: collect is {\"as\": name, \"re\": regex with one group}" % where)
        else:
            try:
                if re.compile(spec["re"]).groups < 1:
                    problems.append("%s: the collect regex needs a group (the number)" % where)
            except re.error as error:
                problems.append("%s: collect %r: %s" % (where, spec["re"], error))
    if "if_state" in step:
        if not isinstance(step["if_state"], dict):
            problems.append("%s: if_state is an object of key: value" % where)
        else:
            for value in step["if_state"].values():
                if str(value).startswith("~"):
                    regexes(str(value)[1:], "if_state")
    if kind == "state":
        if not isinstance(step["state"], dict):
            problems.append("%s: state is an object of key: value" % where)
        else:
            for value in list(step["state"].values()) + list(step.get("ne", {}).values()):
                if str(value).startswith("~"):
                    regexes(str(value)[1:], "state")
        if "keep" in step:
            spec = step["keep"]
            if not isinstance(spec, dict) or set(spec) != {"as", "field", "re"} or not all(isinstance(spec[k], str) and spec[k] for k in spec) or not re.fullmatch(KEEP_NAME, str(spec.get("as", ""))):
                problems.append("%s: keep is {\"as\": NAME (letters, digits, _), \"field\": a state key, \"re\": regex with one group}" % where)
            else:
                try:
                    if re.compile(spec["re"]).groups < 1:
                        problems.append("%s: the keep regex needs a group (the text to keep)" % where)
                except re.error as error:
                    problems.append("%s: keep %r: %s" % (where, spec["re"], error))
    elif kind == "shot":
        compare = step.get("compare")
        if compare is not None:
            if not isinstance(compare, dict) or not ("png" in compare or ("sim_scenario" in compare and "snap" in compare)):
                problems.append("%s: compare needs a png, or a sim_scenario and a snap" % where)
            else:
                shot = sibling("o3shot")
                for r in list(compare.get("regions", [])) + list(compare.get("ignore", [])):
                    try:
                        shot.parse_region(r)
                    except ValueError as error:
                        problems.append("%s: %s" % (where, error))
        if step.get("format", "565") not in ("565", "8888"):
            problems.append("%s: format is 565 or 8888" % where)
    return problems


def validate_scenario(data):
    problems = []
    for section in ("steps", "finally"):
        for i, step in enumerate(data.get(section, [])):
            problems.extend(validate_step(step, "%s %s[%d]" % (data["name"], section, i)))
    for tag in data.get("needs", []):
        if tag not in ("audio", "midi", "sim", "card", "disk"):
            problems.append("%s: needs %r (audio, midi, sim, card or disk)" % (data["name"], tag))
    return problems


def list_scenarios(directory=SCENARIO_DIR):
    names = []
    if os.path.isdir(directory):
        for f in sorted(os.listdir(directory)):
            if f.endswith(".json") and not f.startswith("_"):
                names.append(f[:-5])
    return names


# ---- the unit ---------------------------------------------------------------------------------------------------------------------

class Unit:
    """The console of the unit on an open port."""

    def __init__(self, fd, timeout=3.0):
        self.console = sibling("o3console")
        self.fd = fd
        self.timeout = timeout

    def cmd(self, line, timeout=None):
        return self.console.exchange(self.fd, line, timeout or self.timeout)


class Options:
    def __init__(self, **kw):
        self.audio_device = None
        self.midi_device = None
        self.arecord = "arecord"
        self.sim = None
        self.golden_dir = None
        self.expect_volume = None
        self.out_dir = None
        self.repo = REPO
        self.strict = False
        self.scenario_dir = SCENARIO_DIR
        self.quiet = False
        self.variables = {}
        self.__dict__.update(kw)


def midi_note_events(data):
    """The (kind, note, channel 1..16) of every note event in a raw MIDI byte stream, in order: kind is "on" (velocity > 0) or "off" (a 0x8n message, or a note-on of velocity 0).
    Running status honoured, real-time bytes (F8..FF) skipped."""
    out = []
    status = 0
    args = []
    for b in data:
        if b >= 0xF8:
            continue
        if b & 0x80:
            status = b if b < 0xF0 else 0
            args = []
            continue
        if status == 0:
            continue
        args.append(b)
        need = 1 if 0xC0 <= status < 0xE0 else 2
        if len(args) == need:
            if need == 2 and status & 0xF0 == 0x90:
                out.append(("on" if args[1] > 0 else "off", args[0], (status & 0x0F) + 1))
            elif need == 2 and status & 0xF0 == 0x80:
                out.append(("off", args[0], (status & 0x0F) + 1))
            args = []
    return out


def midi_note_ons(data):
    """The (note, channel 1..16) of every note-on (velocity > 0) in a raw MIDI byte stream (midi_note_events)."""
    return [(note, channel) for kind, note, channel in midi_note_events(data) if kind == "on"]


def midi_unclosed(data, count=None):
    """The (note, channel) of the first `count` note-ons (all of them when None) of a raw MIDI byte stream that no later note-off of the same note and channel ends. One note-off ends the oldest
    note-on of its note and channel still open; a note-off with nothing open is ignored."""
    events = midi_note_events(data)
    opened = []  # [note, channel, counted] of the note-ons still sounding, oldest first
    seen = 0
    for kind, note, channel in events:
        if kind == "on":
            opened.append([note, channel, count is None or seen < count])
            seen += 1
        else:
            for i, entry in enumerate(opened):
                if entry[0] == note and entry[1] == channel:
                    del opened[i]
                    break
    return [(entry[0], entry[1]) for entry in opened if entry[2]]


class StepFailed(Exception):
    pass


class Runner:
    def __init__(self, unit, options, log=print):
        self.unit = unit
        self.o = options
        self.log = log
        self.snaps = {}
        self.samples = {}  # name -> the numbers `collect` kept, for `stat`
        self.kept = {}  # name -> the text a `keep` of a state step took from the state line, for {{NAME}} (forgotten at the start of each scenario)
        self.sim_cache = {}
        self.midi_thread = None
        self.midi_bytes = bytearray()
        self.midi_halt = threading.Event()
        self.disk_session = None

    # -- helpers
    def need(self, cond, message):
        if not cond:
            raise StepFailed(message)

    def command(self, line, expect_ok=True, timeout=None):
        text, ok = self.unit.cmd(line, timeout)
        if expect_ok and not ok:
            raise StepFailed("no prompt after %r" % line)
        return text

    def poll(self, within, interval, attempt):
        """Calls attempt() until it returns None (success); then returns. A string is the reason of its failure: retried until `within` seconds have passed."""
        deadline = time.monotonic() + (within or 0.0)
        while True:
            reason = attempt()
            if reason is None:
                return
            if time.monotonic() >= deadline:
                raise StepFailed(reason)
            time.sleep(interval)

    # -- steps
    def step_say(self, step):
        self.log("    " + str(step["say"]))

    def step_wait(self, step):
        time.sleep(float(step["wait"]))

    def step_cmd(self, step):
        patterns = step.get("expect", [])
        patterns = [patterns] if isinstance(patterns, str) else patterns
        absent = step.get("expect_not", [])
        absent = [absent] if isinstance(absent, str) else absent
        fail_on = step.get("fail_on")
        not_here = step.get("if_reply_not")
        skipped = []
        line = self.expand({"cmd": step["cmd"]})["cmd"]  # (a {{NAME}} a `keep` took is the text of the thing the scenario made)

        last = []

        def attempt():
            text = self.command(line, timeout=step.get("timeout"))
            last[:] = [text]
            if not_here and re.search(not_here, text, re.MULTILINE):  # the image lacks the command: not a failure, and not a reading either
                skipped[:] = [not_here]
                return None
            if fail_on and re.search(fail_on, text, re.MULTILINE):
                raise StepFailed("%r answered %r, which says it failed" % (line, text.strip()[:200]))
            for p in patterns:
                if not re.search(p, text, re.MULTILINE):
                    return "%r: no match for /%s/ in %r" % (line, p, text.strip()[:300])
            for p in absent:
                if re.search(p, text, re.MULTILINE):
                    if step.get("within"):
                        return "%r: /%s/ is in the reply" % (line, p)
                    raise StepFailed("%r: /%s/ is in the reply %r" % (line, p, text.strip()[:300]))
            return None

        self.poll(step.get("within", 0), step.get("poll", 1.0), attempt)
        if skipped:
            self.log("    (skipped: %s)" % skipped[0])
            return None
        if step.get("collect") and last:
            spec = step["collect"]
            m = re.search(spec["re"], last[0], re.MULTILINE)
            self.need(m is not None, "%r: collect /%s/ found no number in %r" % (line, spec["re"], last[0].strip()[:300]))
            try:
                self.samples.setdefault(spec["as"], []).append(float(m.group(1)))
            except ValueError:
                raise StepFailed("%r: collect /%s/ took %r, which is no number" % (line, spec["re"], m.group(1)))
        if step.get("show") and last:
            return last[0].strip("\r\n")  # (run_scenario prints and keeps it)

    def step_stat(self, step):
        series = self.samples.pop(step["stat"], [])
        self.need(len(series) >= step.get("min_n", 1), "stat %s: %d values collected, at least %d wanted" % (step["stat"], len(series), step.get("min_n", 1)))
        line = "stat %s: n %d  mean %.1f  min %.0f  max %.0f" % (step["stat"], len(series), sum(series) / len(series), min(series), max(series))
        if step.get("show"):
            return line
        self.log("    " + line)

    def inject(self, line):
        text = self.command(line)
        self.need(re.search(r"^ok ", text, re.MULTILINE) is not None, "%r: %s" % (line, text.strip()[:200]))

    @staticmethod
    def key_command(token):
        """The console command of one key token (a "name:ms" hold is the caller's to wait out)."""
        if token.startswith("+"):
            return "key %s down" % token[1:]
        if token.startswith("-"):
            return "key %s up" % token[1:]
        if ":" in token:
            name, _, ms = token.partition(":")
            return "key %s hold %d" % (name, int(ms))
        return "key %s" % token

    def step_keys(self, step):
        gap = float(step.get("gap", 0.3))
        until = step.get("until")
        burst = bool(step.get("burst"))
        rounds = int(step["max"]) if until else int(step.get("repeat", 1))
        for done in range(1, rounds + 1):
            for token in step["keys"]:
                token = str(token)
                self.inject(self.key_command(token))
                if burst:
                    continue  # no pacing: the chord window of the firmware is 40 ms
                if ":" in token:
                    time.sleep(int(token.partition(":")[2]) / 1000.0 + 0.1)
                time.sleep(gap)
            if burst:
                time.sleep(gap)
            if until and self.state_problem(self.read_state(), until, {}) is None:
                return
        self.need(not until, "until: %s still not true after %d rounds" % (until, rounds))

    def step_pad(self, step):
        line = "pad %d %s" % (step["pad"], step.get("gesture", "tap"))
        if "vel" in step:
            line += " %d" % step["vel"]
        self.inject(line)
        time.sleep(0.2)

    def step_enc(self, step):
        for _ in range(int(step.get("repeat", 1))):
            self.inject("enc %d" % step["enc"])
            time.sleep(float(step.get("gap", 0.2)))

    def read_state(self):
        state = parse_state(self.command("state"))
        self.need(state is not None, "`state` gave no line of key=value pairs")
        return state

    @staticmethod
    def state_problem(have, want, differ):
        """Why the `state` reading `have` is not what `want` (every pair matches) and `differ` (no pair matches) ask, or None. "~regex" matches by search."""
        def matches(key, value):
            value = str(value)
            if value.startswith("~"):
                return re.search(value[1:], have.get(key, "")) is not None
            return have.get(key) == value

        for key, value in want.items():
            if not matches(key, value):
                return "state %s is %r, wanted %s%r" % (key, have.get(key), "~" if str(value).startswith("~") else "", str(value).lstrip("~"))
        for key, value in differ.items():
            if matches(key, value):
                return "state %s is still %r" % (key, have.get(key))
        return None

    def expand(self, pairs):
        """`pairs` (a state / ne / if_state object) with each {{NAME}} of a value replaced by the text a `keep` took (the regex-escaped text in a "~" value)."""
        def one(value):
            value = str(value)

            def kept(m):
                self.need(m.group(1) in self.kept, "{{%s}}: no earlier state step kept it" % m.group(1))
                return re.escape(self.kept[m.group(1)]) if value.startswith("~") else self.kept[m.group(1)]

            return re.sub(r"\{\{(%s)\}\}" % KEEP_NAME, kept, value)

        return {k: one(v) for k, v in pairs.items()}

    def step_state(self, step):
        want = self.expand(step["state"])
        differ = self.expand(step.get("ne", {}))
        keep = step.get("keep")

        def attempt():
            have = self.read_state()
            problem = self.state_problem(have, want, differ)
            if problem:
                return problem
            if keep:
                found = re.search(keep["re"], have.get(keep["field"], ""))
                if not found:
                    return "keep: state %s is %r, which %r does not match" % (keep["field"], have.get(keep["field"]), keep["re"])
                self.kept[keep["as"]] = found.group(1)
            if "snap" in step:
                self.snaps[step["snap"]] = have
            return None

        self.poll(step.get("within", 0), step.get("poll", 0.3), attempt)

    def step_mix(self, step):
        spec = step["mix"]

        def attempt():
            mix = parse_mix(self.command("mix"))
            tracks = [c for c, v in mix.items() if c.startswith("T") and v["hold"] > -150.0]
            if "min_tracks" in spec and len(tracks) < spec["min_tracks"]:
                return "%d tracks have sounded (%s), %d wanted" % (len(tracks), " ".join(sorted(tracks)), spec["min_tracks"])
            for channel, limits in spec.get("channels", {}).items():
                if channel not in mix:
                    return "no %s line in `mix`" % channel
                have = mix[channel]
                if "min_rms_db" in limits and have["rms"] < limits["min_rms_db"]:
                    return "%s rms %.1f dBFS is below %.1f" % (channel, have["rms"], limits["min_rms_db"])
                if "max_rms_db" in limits and have["rms"] > limits["max_rms_db"]:
                    return "%s rms %.1f dBFS is above %.1f" % (channel, have["rms"], limits["max_rms_db"])
                if "max_peak_db" in limits and have["pk"] > limits["max_peak_db"]:
                    return "%s peak %.1f dBFS is above %.1f" % (channel, have["pk"], limits["max_peak_db"])
                if "max_clip" in limits and have["clip"] > limits["max_clip"]:
                    return "%s clipped in %d windows" % (channel, have["clip"])
            for kind in ("drop", "rise"):
                if kind in spec:
                    c = spec[kind]
                    before = self.snaps.get(c["from"])
                    if before is None or c["channel"] not in before or c["channel"] not in mix:
                        return "%s: no snapshot %r of %s to compare with" % (kind, c["from"], c["channel"])
                    field = {"rms": "rms", "pk": "pk", "hold": "hold"}[c.get("field", "rms")]
                    delta = mix[c["channel"]][field] - before[c["channel"]][field]
                    if kind == "drop" and delta > -c["min_db"]:
                        return "%s %s moved %+.1f dB since %r, a drop of at least %.1f dB was wanted" % (c["channel"], field, delta, c["from"], c["min_db"])
                    if kind == "rise" and delta < c["min_db"]:
                        return "%s %s moved %+.1f dB since %r, a rise of at least %.1f dB was wanted" % (c["channel"], field, delta, c["from"], c["min_db"])
            if "snap" in spec:
                self.snaps[spec["snap"]] = mix
            return None

        self.poll(spec.get("within", 0), 0.5, attempt)

    def step_leds(self, step):
        spec = step["leds"]

        def attempt():
            leds = parse_leds(self.command("leds"))
            if leds is None:
                return "`leds` gave no line of levels"
            if spec.get("online", True) and not leds["online"]:
                return "the LED driver is offline"
            if "min_lit" in spec and leds["lit"] < spec["min_lit"]:
                return "%d pads lit, at least %d wanted" % (leds["lit"], spec["min_lit"])
            if "max_lit" in spec and leds["lit"] > spec["max_lit"]:
                return "%d pads lit, at most %d wanted" % (leds["lit"], spec["max_lit"])
            if spec.get("sync") and not leds["sync"]:
                return "the chip does not hold what is staged yet"
            pads = leds_pads_problem(leds, spec)
            if pads:
                return pads
            if "differs_from" in spec:
                before = self.snaps.get("leds:" + spec["differs_from"])
                if before is None or before == leds["levels"]:
                    return "the pad levels are the same as in %r" % spec["differs_from"]
            if "snap" in spec:
                self.snaps["leds:" + spec["snap"]] = leds["levels"]
            return None

        self.poll(spec.get("within", 0), 0.4, attempt)

    def step_voices(self, step):
        spec = step["voices"]
        v = parse_voices(self.command("voices"))
        self.need(v is not None, "`voices` gave no line")
        self.need(v["in_use"] >= spec.get("min_in_use", 0), "%d voices in use, at least %d wanted" % (v["in_use"], spec.get("min_in_use", 0)))
        self.need(v["steals"] <= spec.get("max_steals", 1 << 30), "%d steals, at most %d wanted" % (v["steals"], spec.get("max_steals", 0)))

    def golden(self, compare):
        shot = sibling("o3shot")
        if "png" in compare:
            return os.path.join(self.o.repo, compare["png"]) if not os.path.isabs(compare["png"]) else compare["png"]
        key = compare["sim_scenario"]
        wanted = "%s.png" % compare["snap"]
        for directory in ([self.o.golden_dir] if self.o.golden_dir else []):
            path = os.path.join(directory, wanted)
            if os.path.isfile(path):
                return path
        if key not in self.sim_cache:
            sim = self.o.sim
            self.need(sim and os.path.isfile(sim), "no simulator binary (--sim) and no golden PNG %s in --golden-dir" % wanted)
            out = tempfile.mkdtemp(prefix="o3hil-sim-")
            scenario = compare["sim_scenario"] if os.path.isabs(compare["sim_scenario"]) else os.path.join(self.o.repo, compare["sim_scenario"])
            run = subprocess.run([sim, "--scenario", scenario, "--out", out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.need(run.returncode == 0, "o3sim exited %d: %s" % (run.returncode, run.stdout[-300:]))
            self.sim_cache[key] = out
        path = os.path.join(self.sim_cache[key], wanted)
        self.need(os.path.isfile(path), "the simulator's scenario has no snap %r" % compare["snap"])
        return path

    def step_shot(self, step, scenario_name):
        shot = sibling("o3shot")
        out_dir = self.o.out_dir or tempfile.gettempdir()
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, "%s_%s.png" % (scenario_name, step["shot"]))
        import io

        sink = io.StringIO()
        status = self.unit.console.shot_unit(self.unit.fd, path, sink, fmt=step.get("format"), retries=2)
        if status == 4 and "UI animated" in sink.getvalue() and os.path.isfile(path):
            self.log("    (the CPU readout animates the screen: taking the picture anyway)")
            status = 0
        self.need(status == 0, "shot failed (%d): %s" % (status, sink.getvalue().strip()[-300:]))
        width, height, rgb = shot.read_png(path)
        if step.get("non_blank", True):
            colours = {rgb[i:i + 3] for i in range(0, len(rgb), 3 * 97)}
            self.need(len(colours) > 3, "the picture is blank (%d colours)" % len(colours))
        compare = step.get("compare")
        if compare:
            golden = self.golden(compare)
            gw, gh, grgb = shot.read_png(golden)
            self.need((gw, gh) == (width, height), "the golden %s is %dx%d" % (golden, gw, gh))
            result = shot.compare_images(rgb, grgb, width, height, compare.get("tol", 24), compare.get("aa", 1), [shot.parse_region(r) for r in compare.get("regions", [])],
                                         [shot.parse_region(r) for r in compare.get("ignore", [])], want_diff=True)
            limit = compare.get("max_bad", 0.005)
            self.log("    %s: %d of %d compared pixels differ (%.3f %%, allowed %.3f %%) against %s" % (path, result.bad, result.compared, 100.0 * result.fraction, 100.0 * limit, golden))
            if result.fraction > limit:
                diff = path[:-4] + "_diff.png"
                shot.write_png(diff, width, height, result.diff)
                raise StepFailed("the screen differs from the golden: %d pixels (%.3f %%), in x %d..%d y %d..%d; difference picture %s" %
                                 ((result.bad, 100.0 * result.fraction) + (result.bbox[0], result.bbox[2], result.bbox[1], result.bbox[3], diff)))

    def step_audio(self, step):
        audio = sibling("o3audio")
        spec = step["audio"]
        try:
            channels = spec.get("channels")  # omitted: the capture device's own channel count (o3audio.record)
            data = audio.record(self.o.audio_device, float(spec.get("seconds", 2.0)), None if channels is None else int(channels), arecord=self.o.arecord)
            analysis = audio.analyze_wav(data)
        except audio.AudioError as error:
            raise StepFailed("the audio capture failed: %s" % error)
        self.log("    " + audio.describe(analysis).replace("\n", "\n    "))
        channel = int(spec.get("channel", 1))
        self.need(1 <= channel <= analysis["channels"], "channel %d is not in the %d channel capture" % (channel, analysis["channels"]))
        problems = audio.check_channel(analysis["results"][channel - 1], spec)
        self.need(not problems, "ch%d: %s" % (channel, "; ".join(problems)))

    def step_midi(self, step):
        spec = step["midi"]
        if "start" in spec:
            self.midi_stop()
            self.midi_bytes = bytearray()
            self.midi_halt = threading.Event()
            try:
                fd = os.open(self.o.midi_device, os.O_RDONLY | os.O_NONBLOCK)
            except OSError as error:
                raise StepFailed("the MIDI device %s cannot be opened: %s" % (self.o.midi_device, error))
            self.midi_thread = threading.Thread(target=self.midi_listen, args=(fd, self.midi_bytes, self.midi_halt), daemon=True)
            self.midi_thread.start()
            return
        self.need(self.midi_thread is not None, "midi: no {\"start\": true} step before this one")
        time.sleep(float(spec.get("settle", 0.3)))
        want = spec["on"]
        if spec.get("off"):  # the note-offs may come later than the settle (a long note, the stop): poll for them up to the timeout
            deadline = time.monotonic() + float(spec.get("timeout", 2.0))
            while midi_unclosed(bytes(self.midi_bytes), None if spec.get("exact") else len(want)) and time.monotonic() < deadline:
                time.sleep(0.05)
        self.midi_stop()
        notes = midi_note_ons(bytes(self.midi_bytes))
        got = [n for n, _ in notes] if spec.get("exact") else [n for n, _ in notes[:len(want)]]
        if spec.get("relative") and got:
            got = [n - notes[0][0] for n in got]
        self.log("    MIDI note-ons: %s" % " ".join("%d/%d" % n for n in notes[:48]) if notes else "    MIDI note-ons: none")
        self.need(got == want, "MIDI notes %s, expected %s%s" % (got, want, " (all of them)" if spec.get("exact") else ""))
        if "channel" in spec:
            wrong = [(n, c) for n, c in notes[:len(want)] if c != spec["channel"]]
            self.need(not wrong, "MIDI channel is not %d: %s" % (spec["channel"], wrong))
        if spec.get("off"):
            unclosed = midi_unclosed(bytes(self.midi_bytes), None if spec.get("exact") else len(want))
            self.need(not unclosed, "MIDI note-off missing for %s within %s s" % (" ".join("%d/%d" % n for n in unclosed), spec.get("timeout", 2.0)))

    @staticmethod
    def midi_listen(fd, sink, halt):
        try:
            while not halt.is_set():
                ready, _, _ = select.select([fd], [], [], 0.05)
                if ready:
                    try:
                        data = os.read(fd, 4096)
                    except BlockingIOError:
                        continue
                    if data:
                        sink.extend(data)
                    else:
                        time.sleep(0.01)  # a FIFO with no writer reads as end of file; a real node never does
        finally:
            os.close(fd)

    def midi_stop(self):
        if self.midi_thread is not None:
            self.midi_halt.set()
            self.midi_thread.join(2.0)
            self.midi_thread = None

    def step_card(self, step):
        text = self.command("fw")
        m = re.search(r"volume ([0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}), \S+,.*write-protect (yes|no)", text)
        self.need(m is not None, "`fw` reports no card: %s" % text.strip()[:200])
        if self.o.expect_volume:  # optional: any card may be written (OS-ANYCARD), but a named one must be the one in the unit
            self.need(m.group(1).upper() == self.o.expect_volume.upper(), "the card is volume %s, not %s: nothing was done" % (m.group(1), self.o.expect_volume))
        self.need(m.group(2) == "no", "the card %s is write protected" % m.group(1))

    def step_disk(self, step):
        """OS-USBDISK U1d: the PC side of the disk scenarios (o3disk.py, Linux). Every op that reads or writes a file goes through Session.target(), which proves the unit's removable disk first."""
        disk = sibling("o3disk")
        if self.disk_session is None:
            self.disk_session = disk.Session(self.o.expect_volume, log=self.log, scratch=None)
        sess = self.disk_session
        op = step["disk"]
        try:
            if op == "enumerate":
                sess.enumerate(step.get("within", 30), step.get("ro"))
            elif op == "mount":
                self.log("    mounted at %s%s" % (sess.mount_card(step.get("within", 30)), " (volume %s)" % sess.expect_volume if sess.expect_volume else ""))
            elif op == "read":
                sess.read_check(step.get("size_mb", 8))
            elif op == "write":
                sess.write_roundtrip(step.get("size_mb", 8))
            elif op == "remount":
                self.log("    remounted at %s" % sess.remount(step.get("within", 30)))
            elif op == "eject":
                sess.eject(step.get("within", 20))
            elif op == "gone":
                deadline = time.monotonic() + step.get("within", 20)
                while [d for d in disk.find_unit_disks() if sess.disk and d["name"] == sess.disk["name"] and d["blocks"] > 0]:
                    self.need(time.monotonic() < deadline, "the unit's disk %s is still on the bus" % sess.disk["name"])
                    time.sleep(0.5)
        except disk.DiskError as error:
            raise StepFailed(str(error))

    def run_step(self, step, scenario_name):
        kind = [k for k in STEP_KINDS if k in step][0]
        if "if_state" in step:
            have = self.read_state()
            for key, value in self.expand(step["if_state"]).items():
                value = str(value)
                if not (re.search(value[1:], have.get(key, "")) if value.startswith("~") else have.get(key) == value):
                    self.log("    (skipped: state %s is %r)" % (key, have.get(key)))
                    return None
        if "if_leds" in step:
            leds = parse_leds(self.command("leds"))
            self.need(leds is not None, "`leds` gave no line of levels")
            why = leds_pads_problem(leds, step["if_leds"])
            if why:
                self.log("    (skipped: %s)" % why)
                return None
        if kind == "shot":
            return self.step_shot(step, scenario_name)
        return getattr(self, "step_" + kind)(step)

    def available(self, needs):
        missing = []
        if "audio" in needs and not self.o.audio_device:
            missing.append("audio (no --audio-device)")
        if "sim" in needs and not (self.o.golden_dir or (self.o.sim and os.path.isfile(self.o.sim))):
            missing.append("sim (no --sim binary and no --golden-dir)")
        if "midi" in needs and not self.o.midi_device:
            missing.append("midi (no --midi-device)")
        return missing

    @staticmethod
    def untested(needs):
        """The reason a scenario is not run at all, whatever --strict says: the PC side of the disk scenarios exists for Linux only (Windows and macOS are untested: not run, not failed)."""
        if "disk" in needs and not PLATFORM.startswith("linux"):
            return "needs disk: untested on %s (the PC side is Linux only; not run, not failed)" % PLATFORM
        return None

    def run_scenario(self, data):
        name = data["name"]
        result = {"name": name, "title": data.get("title", ""), "status": "pass", "steps": [], "seconds": 0.0}
        started = time.monotonic()
        untested = self.untested(data.get("needs", []))
        if untested:
            result["status"] = "skip"
            result["reason"] = untested
            self.log("SKIP %s: %s" % (name, untested))
            return result
        missing = self.available(data.get("needs", []))
        if missing:
            result["status"] = "fail" if self.o.strict else "skip"
            result["reason"] = "needs " + ", ".join(missing)
            self.log("SKIP %s: %s" % (name, result["reason"]) if not self.o.strict else "FAIL %s: %s" % (name, result["reason"]))
            return result
        self.log("RUN  %s%s" % (name, ": " + data["title"] if data.get("title") else ""))
        self.kept = {}
        stopped = False
        for section in ("steps", "finally"):
            steps = data[section] if section == "steps" else list(data["finally"]) + list(data.get("_snippet_finally", []))
            if section == "finally" and not steps:
                break
            for index, step in enumerate(steps):
                if section == "steps" and stopped:
                    break
                kind = [k for k in STEP_KINDS if k in step][0]
                label = "%s[%d] %s" % (section, index, kind)
                if "note" in step:
                    self.log("  " + str(step["note"]))
                t = time.monotonic()
                reply = None
                try:
                    reply = self.run_step(step, name)
                    status, message = "pass", ""
                except StepFailed as error:
                    status, message = "fail", str(error)
                except Exception as error:  # a bug of the runner must not look like a pass
                    status, message = "fail", "%s: %s" % (type(error).__name__, error)
                if status == "fail" and step.get("soft"):
                    status = "soft-fail"
                entry = {"step": label, "status": status, "message": message, "seconds": round(time.monotonic() - t, 2)}
                if isinstance(reply, str) and step.get("show"):
                    entry["reply"] = reply
                result["steps"].append(entry)
                self.log("  %-9s %s%s" % (status.upper(), label, ("  " + message) if message else ""))
                if "reply" in entry:
                    for line in entry["reply"].splitlines():
                        self.log("    | " + line.rstrip())
                if status == "fail":
                    result["status"] = "fail"
                    if section == "steps":
                        stopped = True
        self.midi_stop()
        result["seconds"] = round(time.monotonic() - started, 2)
        self.log("%s %s (%.1f s)" % ("PASS" if result["status"] == "pass" else "FAIL", name, result["seconds"]))
        return result


# ---- command line -----------------------------------------------------------------------------------------------------------------

def run_scenarios(names, port, options, report=None, log=print):
    console = sibling("o3console")
    try:
        datas = [load_scenario(n, options.scenario_dir, variables=options.variables) for n in names]
    except ScenarioError as error:
        print("o3hil: %s" % error, file=sys.stderr)
        return 2
    unused = [k for k in options.variables if not any(k in d.get("vars", {}) for d in datas)]
    if unused:
        print("o3hil: no scenario declares --var %s" % ", ".join(unused), file=sys.stderr)
        return 2
    problems = [p for d in datas for p in validate_scenario(d)]
    if problems:
        print("o3hil: the scenarios are not valid:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 2
    if port is None:
        print("o3hil: give --port (or plug the unit in)", file=sys.stderr)
        return 2
    try:
        fd = console.open_port(port)
    except OSError as error:
        print("o3hil: cannot open %s: %s" % (port, error), file=sys.stderr)
        return 2
    try:
        if not console.wake(fd, 3.0):
            print("o3hil: no prompt from the unit within 3 s", file=sys.stderr)
            return 3
        runner = Runner(Unit(fd), options, log)
        results = [runner.run_scenario(d) for d in datas]
    finally:
        os.close(fd)
    failed = [r for r in results if r["status"] == "fail"]
    log("")
    for r in results:
        log("%-5s %-28s %s" % (r["status"].upper(), r["name"], r.get("reason", "")))
    log("%d scenarios: %d passed, %d failed, %d skipped" % (len(results), sum(r["status"] == "pass" for r in results), len(failed), sum(r["status"] == "skip" for r in results)))
    if report:
        with open(report, "w") as f:
            json.dump({"scenarios": results}, f, indent=1)
    return 1 if failed else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Hardware-in-the-loop scenarios for the Tracker (see the text at the top of this file).")
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("list")
    v = sub.add_parser("validate")
    v.add_argument("scenarios", nargs="*")
    r = sub.add_parser("run")
    r.add_argument("scenarios", nargs="+")
    r.add_argument("--port")
    r.add_argument("--audio-device", help="alsa capture device of the unit (hw:CARD,DEV, or auto); without it the scenarios that need audio are skipped")
    r.add_argument("--midi-device", help="the raw MIDI node of the unit's MIDI out (/dev/snd/midiC<card>D<dev>, see hil/midi_clock_tool.py list); without it the scenarios that need midi are skipped")
    r.add_argument("--arecord", default="arecord")
    r.add_argument("--sim", default=os.path.join(REPO, "build-os-host", "sim", "o3sim"), help="the simulator binary the goldens come from (default build-os-host/sim/o3sim)")
    r.add_argument("--golden-dir", help="a directory of the simulator's PNGs (o3sim --scenario ... --out DIR) used instead of running it")
    r.add_argument("--expect-volume", metavar="XXXX-XXXX", help="optional: the volume serial the card in the unit must have (any SD card may be written; the card and disk steps fail on another one)")
    r.add_argument("--out", help="where the pictures go (default the temporary directory)")
    r.add_argument("--report", help="write the results as JSON")
    r.add_argument("--strict", action="store_true", help="a skipped scenario is a failure")
    r.add_argument("--var", action="append", default=[], metavar="NAME=VALUE", help="set a scenario variable declared in its \"vars\" (for example SIZE_MB of qa_usbdisk_rw)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "list":
        for name in list_scenarios():
            try:
                title = load_scenario(name).get("title", "")
            except ScenarioError as error:
                title = "INVALID: %s" % error
            print("%-26s %s" % (name, title))
        return 0
    if args.command == "validate":
        names = args.scenarios or list_scenarios()
        bad = 0
        for name in names:
            try:
                problems = validate_scenario(load_scenario(name))
            except ScenarioError as error:
                problems = [str(error)]
            for p in problems:
                print("o3hil: %s" % p)
            bad += 1 if problems else 0
        print("o3hil: %d scenarios checked, %d invalid" % (len(names), bad))
        return 1 if bad else 0
    if args.command == "run":
        variables = {}
        for item in args.var:
            if "=" not in item:
                print("o3hil: --var NAME=VALUE (got %r)" % item, file=sys.stderr)
                return 2
            variables[item.split("=", 1)[0]] = item.split("=", 1)[1]
        console = sibling("o3console")
        port = args.port or (console.find_ports() or [None])[0]
        options = Options(audio_device=args.audio_device, midi_device=args.midi_device, arecord=args.arecord, sim=args.sim, golden_dir=args.golden_dir, expect_volume=args.expect_volume, out_dir=args.out,
                          strict=args.strict, variables=variables)
        return run_scenarios(args.scenarios, port, options, args.report)
    parser.print_usage(sys.stderr)
    return 2


# ---- self-test: the runner against a pseudo-terminal that plays a unit ----------------------------------------------------------------

class MockUnit(threading.Thread):
    """A small unit on the far side of a pseudo-terminal: a screen, a cursor, a transport, meters that follow the transport, pads, a card. Enough for the runner's every step."""

    def __init__(self, fd, picture=None, shot_fmt="565", volume="ABCD-1234", protect=False):
        super().__init__(daemon=True)
        self.fd = fd
        self.stop = False
        self.seen = []
        self.screen = "PATTERN"
        self.playing = False
        self.row = 0
        self.pads = "NOTES"
        self.master_db = 0.0
        self.shift = False
        self.posts = 0
        self.msg = "-"
        self.picture = picture
        self.shot_fmt = shot_fmt
        self.volume = volume
        self.protect = protect
        self.lit = 0
        self.lit_pads = {14}  # pads the mock lights whatever the mode (the MUTE row of a muted track)
        self.cpu_n = 0
        self.tick = 0
        self.journal = 0  # undo steps in the history: F8 (no Shift) takes one off, or says NOTHING_TO_UNDO

    def say(self, text):
        os.write(self.fd, text.replace("\n", "\r\n").encode())

    def state_line(self):
        return "screen=%s page=0 mode=3 overlay=NONE pattern=1 track=0 row=%d col=NOTE playing=%d seq=%d tempo=12800 instrument=1 pads=%s shift=%d order=- play_pattern=1 play_step=0 posts=%d sel=- msg=%s" % (
            self.screen, self.row, int(self.playing), int(self.playing), self.pads, int(self.shift), self.posts, self.msg)

    def mix_text(self):
        if not self.playing:
            return "mix  windows 4  frames 1000  (dBFS; clip = windows at full scale)\n  nothing measured yet\n"
        out = "mix  windows 40  frames 40000  (dBFS; clip = windows at full scale)\n"
        for t in range(1, 7):
            out += "  T%02d  pk  %5.1f  hold  %5.1f  rms  %5.1f  clip 0\n" % (t, -12.0, -6.0, -20.0)
        out += "  MAIN L  pk  %5.1f  hold  %5.1f  rms  %5.1f  clip 0\n" % (-6.0 + self.master_db, -3.0 + self.master_db, -14.0 + self.master_db)
        out += "  MAIN R  pk  %5.1f  hold  %5.1f  rms  %5.1f  clip 0\n" % (-6.5 + self.master_db, -3.5 + self.master_db, -14.5 + self.master_db)
        return out

    def leds_line(self):
        on = [(self.pads == "VU" and self.playing and i < 12) or i in self.lit_pads for i in range(48)]
        levels = "".join("%02X" % (0xA0 if on[i] else 0) for i in range(48))
        lit = sum(on)
        return "leds online=1 errors=0 reinits=0 lit=%d sync=1 levels=%s" % (lit, levels)

    def key(self, args):
        names = ["f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "params", "performance", "file", "config", "master", "sampleload", "sampleplay", "sampleedit", "samplerec", "song",
                 "note", "instr", "vol", "fx", "pattern", "play", "copy", "insert", "up", "delete", "rec", "shift", "left", "down", "right"]
        name = args[0] if args else ""
        if name not in names and not (name.isdigit() and int(name) < 33):
            self.say("ERR unknown key\n")
            return
        gesture = args[1] if len(args) > 1 else "tap"
        if name == "shift":
            self.shift = gesture != "up"
        elif gesture != "up":
            screens = {"master": "MIXER", "song": "SONG", "pattern": "PATTERN", "performance": "PERFORM", "sampleload": "SAMPLE_LOAD", "file": "FILE", "config": "CONFIG"}
            if name in screens:
                self.screen = screens[name]
            elif name == "play":
                self.playing = not self.playing
            elif name == "down":
                self.row += 1
            elif name == "f8" and not self.shift:
                if self.journal:
                    self.journal -= 1
                    self.msg = "-"
                else:
                    self.msg = "NOTHING_TO_UNDO"
            elif name == "f7" and self.shift:
                self.pads = "VU"
            elif name == "f5" and self.screen == "FILE":
                self.posts += 1
                self.msg = "SAVE_TRACK_01_DONE"
        self.say("ok key %d\n" % (names.index(name) if name in names else int(name)))

    def shot(self, args):
        shot = sibling("o3shot")
        self.say(shot.make_capture(self.picture, self.shot_fmt))

    def handle(self, line):
        words = line.split()
        if not words:
            return
        self.seen.append(line)
        c = words[0]
        if c == "version":
            self.say("Track3r o3sdk, build mock\nusb 16D0:0E9F rev 0001 serial 0123456789ABCDEF\n")
        elif c == "key":
            self.key(words[1:])
        elif c == "pad":
            self.say("ok pad %s\n" % words[1])
        elif c == "enc":
            if self.screen == "MIXER":
                self.master_db += int(words[1]) * 0.5
            self.say("ok encoder %+d\n" % int(words[1]))
        elif c == "state":
            self.say(self.state_line() + "\n")
        elif c == "mix":
            self.say(self.mix_text())
        elif c == "leds":
            self.say(self.leds_line() + "\n")
        elif c == "cpu":
            self.cpu_n += 100
            self.say("audio  blocks 10  renders 10  underrun 0 (run 0, max 0)\nrender last %d  max 999 cyc = 0.4%% of the block (261120 cyc)  over-budget 0\n" % self.cpu_n)
        elif c == "voices":
            self.say("voices  in use %d/32  peak 5  steals 0\n" % (3 if self.playing else 0))
        elif c == "sd":
            self.tick += 1
            self.say("sd: card ok\nload:    Projects/t-v16-copy: v2, 48 patterns, 16 instruments\n" + ("pcm:     samples 16 loaded, 0 missing, 7595 KB\n" if self.tick > 1 else ""))
        elif c == "load":
            self.tick = 0
            self.say("load: reading %s\n" % " ".join(words[1:]))
        elif c == "fw" and len(words) == 1:
            self.say("fw: write path ENABLED\ncard: volume %s, FAT32, Firmware/ yes, write-protect %s\n" % (self.volume, "yes" if self.protect else "no"))
        elif c == "shot":
            self.shot(words[1:])
        else:
            self.say("unknown command: %s (help lists the commands)\n" % c)

    def run(self):
        import select

        self.say("\nTrack3r diag - o3sdk mock - 'help' lists the commands\n")
        os.write(self.fd, b"o3> ")
        line = b""
        while not self.stop:
            try:
                ready, _, _ = select.select([self.fd], [], [], 0.05)
                data = os.read(self.fd, 4096) if ready else b""
            except (OSError, ValueError):
                return
            for byte in data:
                ch = bytes([byte])
                if ch in (b"\r", b"\n"):
                    os.write(self.fd, b"\r\n")
                    self.handle(line.decode())
                    line = b""
                    os.write(self.fd, b"o3> ")
                else:
                    line += ch
                    os.write(self.fd, ch)


def _golden_pictures():
    """A 565 picture with some structure (a chrome strip, a pattern grid), and the same as 888 for the golden file."""
    import struct

    shot = sibling("o3shot")
    w, h = 800, 480
    data = bytearray()
    for y in range(h):
        for x in range(w):
            if y >= 458:
                v = 0xFFE0 if (x // 100) % 2 == 0 else 0x07E0  # the key row
            elif (x // 10 + y // 24) % 5 == 0:
                v = 0xF800
            else:
                v = 0x1082 + (y // 24)
            data += struct.pack("<H", v)
    data = bytes(data)
    rgb = shot.shot_rgb(shot.Shot("565", w, h, data, False, 0, 0, []))
    return data, rgb


def self_test():
    import io
    import stat

    problems = []
    console = sibling("o3console")
    shot_tool = sibling("o3shot")
    # the parsers
    st = parse_state("o3> state\r\nscreen=SONG page=1 mode=3 overlay=NONE msg=-\r\n")
    if not st or st["screen"] != "SONG" or st["page"] != "1":
        problems.append("parse_state: %r" % st)
    lv = parse_leds("leds online=1 errors=0 reinits=2 lit=1 sync=1 levels=" + "00" * 47 + "FF\n")
    if not lv or lv["lit"] != 1 or lv["levels"][47] != 255 or lv["reinits"] != 2 or parse_leds("leds online=1 levels=00\n") is not None:
        problems.append("parse_leds: %r" % lv)
    mx = parse_mix("mix  windows 40  frames 40000  (dBFS; clip = windows at full scale)\n  T01  pk  -12.0  hold  -6.0  rms  -20.0  clip 0\n  T02  pk  -inf  hold  -inf  rms  -inf  clip 3\n  MAIN L  pk   -6.0  hold   -3.0  rms  -14.0  clip 0\n  IN L  pk  +0.0  hold   0.0  rms   -9.9  clip 0\n")
    if sorted(mx) != ["IN L", "MAIN L", "T01", "T02"] or mx["T02"]["pk"] != -200.0 or mx["T02"]["clip"] != 3 or mx["MAIN L"]["rms"] != -14.0:
        problems.append("parse_mix: %r" % mx)
    if parse_voices("voices  in use 3/32  peak 5  steals 1\n") != {"in_use": 3, "pool": 32, "peak": 5, "steals": 1}:
        problems.append("parse_voices")
    # the button list is the kit's (kit/app/src/panel.cpp, the kNames table)
    header = os.path.join(REPO, "kit", "app", "src", "panel.cpp")
    if os.path.isfile(header):
        with open(header) as f:
            table = re.search(r"kNames\[[^\]]*\]\s*=\s*\{(.*?)\};", f.read(), re.S)
        names = re.findall(r'"(\w+)"', table.group(1)) if table else []
        if names != BUTTONS:
            problems.append("the button names differ from kit/app/src/panel.cpp: %r" % (set(names) ^ set(BUTTONS),))
    # every shipped scenario is valid
    for name in list_scenarios():
        try:
            for p in validate_scenario(load_scenario(name)):
                problems.append(p)
        except ScenarioError as error:
            problems.append(str(error))
    if not list_scenarios():
        problems.append("no scenario in %s" % SCENARIO_DIR)
    # save_test_card waits for the PROJECT's own message (the samples' SAVE_INSTRnn_DONE come first: a wait that takes one ends while the save runs), for up to 200 s
    if os.path.isfile(os.path.join(SCENARIO_DIR, "save_test_card.json")):  # (the tracker's scenario: o3sdk ships none; the check runs when it is in hil/)
        try:
            waits = [st for st in load_scenario("save_test_card")["steps"] if "keep" in st]
            want = waits[0]["state"]["msg"][1:] if waits else ""
            if len(waits) != 1 or waits[0].get("within", 0) < 200 or re.search(want, "SAVE_INSTR01_DONE") or not re.search(want, "SAVE_TRACK_07_DONE"):
                problems.append("save_test_card does not wait for the SAVE_TRACK_NN_DONE of the project within 200 s: %r" % (waits,))
        except (ScenarioError, KeyError, TypeError, re.error) as error:
            problems.append("save_test_card: %s" % error)
    # scenario variables: ${NAME} / ${NAME-1} in strings, a whole-token number becomes an int, --var overrides, an undeclared name is an error
    try:
        sub = _substitute({"pad": "${RUN-1}", "cmd": "ls 0hilp${RUN}", "keys": ["x${ROW}"]}, {"RUN": "3", "ROW": "7"}, "t")
        if sub != {"pad": 2, "cmd": "ls 0hilp3", "keys": ["x7"]}:
            problems.append("the scenario variables are not substituted as documented: %r" % (sub,))
        _substitute("${NOPE}", {}, "t")
        problems.append("an undeclared ${NAME} was not refused")
    except ScenarioError:
        pass
    # the validator catches what it should
    bad_steps = [{"cmd": "version", "expct": "x"}, {"cmd": "version", "show": "yes"}, {"keys": ["bogus"]}, {"keys": ["f1:x"]}, {"cmd": "reset"}, {"cmd": "fw put a 1 2"}, {"pad": 48}, {"enc": 0}, {"wait": -1},
                 {"state": {"msg": "~("}}, {"cmd": "x", "expect": "("}, {"shot": "a", "compare": {"snap": "x"}}, {"shot": "a", "compare": {"png": "x", "regions": ["1,2"]}},
                 {"cmd": "a", "wait": 1}, {}, "text",
                 {"cmd": "cpu", "collect": {"as": "U"}}, {"cmd": "cpu", "collect": {"as": "U", "re": "no group"}}, {"cmd": "cpu", "collect": {"as": "U", "re": "("}},
                 {"stat": "U", "min_n": 0}, {"stat": ""}, {"stat": "U", "show": "yes"}, {"leds": {"lit": [48]}}, {"leds": {"unlit": "12"}}, {"cmd": "cpu", "if_leds": {}},
                 {"cmd": "cpu", "if_leds": {"lit": [-1]}}, {"cmd": "cpu", "if_leds": {"dark": [1]}}]
    for step in bad_steps:
        if not validate_step(step, "t"):
            problems.append("the validator accepted %r" % (step,))
    for step in ({"cmd": "fw"}, {"cmd": "mod", "expect": "x", "show": True}, {"keys": ["+shift", "f7", "-shift", "song:600", "12"]}, {"state": {"msg": "~DONE$"}},
                 {"cmd": "cpu", "collect": {"as": "U", "re": "render last (\\d+)"}}, {"stat": "U", "min_n": 12, "show": True}, {"leds": {"lit": [12], "unlit": [13], "within": 3}}, {"cmd": "x", "if_leds": {"unlit": [12]}}):
        if validate_step(step, "t"):
            problems.append("the validator refused %r: %s" % (step, validate_step(step, "t")))

    # the runner against the mock unit
    data565, golden_rgb = _golden_pictures()
    with tempfile.TemporaryDirectory() as tmp:
        golden = os.path.join(tmp, "golden.png")
        shot_tool.write_png(golden, 800, 480, golden_rgb)
        arecord = os.path.join(tmp, "arecord")
        audio = sibling("o3audio")
        wav = os.path.join(tmp, "in.wav")
        with open(wav, "wb") as f:
            f.write(audio.make_wav([audio.sine(440.0, 1.0, 48000, 0.3)] * 3, 48000, streaming=True))
        with open(arecord, "w") as f:
            f.write("#!/bin/sh\ncat %s\n" % wav)
        os.chmod(arecord, os.stat(arecord).st_mode | stat.S_IXUSR)
        scen_dir = os.path.join(tmp, "scenarios")
        os.makedirs(scen_dir)

        def write(name, obj):
            with open(os.path.join(scen_dir, name + ".json"), "w") as f:
                json.dump(obj, f)

        write("_play", {"steps": [{"keys": ["play"], "gap": 0.0}, {"state": {"playing": "1"}, "within": 2}], "finally": [{"cmd": "state"}]})
        write("all_steps", {
            "title": "every step kind", "needs": ["audio", "sim", "card"],
            "steps": [
                {"say": "hello"},
                {"cmd": "version", "expect": ["Track3r o3sdk", "serial 0123456789ABCDEF"], "expect_not": "ERR"},
                {"cmd": "sd", "expect": "pcm:\\s+samples \\d+ loaded", "within": 5, "poll": 0.1, "fail_on": "FAILED"},
                {"run": "_play"},
                {"keys": ["+shift", "f7", "-shift"], "gap": 0.0},
                {"state": {"screen": "PATTERN", "pads": "VU", "playing": "1", "msg": "~^-$"}, "ne": {"row": "5"}, "snap": "first"},
                {"mix": {"min_tracks": 4, "channels": {"MAIN L": {"min_rms_db": -30, "max_peak_db": 0}}, "snap": "a"}},
                {"leds": {"min_lit": 1, "sync": True, "snap": "vu"}},
                {"voices": {"min_in_use": 1, "max_steals": 0}},
                {"keys": ["master"], "gap": 0.0},
                {"enc": -20},
                {"mix": {"drop": {"channel": "MAIN L", "field": "rms", "from": "a", "min_db": 6}, "within": 2}},
                {"keys": ["file", "f5"], "gap": 0.0},
                {"state": {"screen": "FILE", "msg": "~SAVE_.*_DONE"}, "within": 2, "poll": 0.1},
                {"card": {}},
                {"pad": 3, "gesture": "down", "vel": 90},
                {"wait": 0.01},
                {"shot": "chrome", "compare": {"png": golden, "regions": ["0,458,800,22"], "tol": 24, "aa": 1, "max_bad": 0.001}},
                {"shot": "whole", "compare": {"png": golden}},
                {"audio": {"seconds": 1, "channels": 3, "channel": 2, "pitch_hz": 440, "silent": False, "max_peak_db": -3, "clipping": False}},
            ],
            "finally": [{"keys": ["pattern"], "gap": 0.0}]})
        write("failing", {"steps": [{"cmd": "version"}, {"state": {"screen": "SONG"}, "within": 0.3, "poll": 0.1}, {"cmd": "version", "note": "never run"}],
                           "finally": [{"cmd": "state", "expect": "screen="}]})
        write("soft", {"steps": [{"state": {"screen": "SONG"}, "soft": True}, {"cmd": "version", "expect": "build mock"}]})
        write("not_here", {"steps": [{"cmd": "version", "if_reply_not": "build mock", "expect": "NEVER", "within": 0.2, "show": True}, {"cmd": "version", "if_reply_not": "build mock", "expect_not": "mock"}]})
        write("is_here", {"steps": [{"cmd": "version", "if_reply_not": "not in this image", "expect": "build mock"}]})
        write("not_there", {"steps": [{"cmd": "version", "if_reply_not": "not in this image", "expect": "NEVER"}]})
        write("shown", {"steps": [{"cmd": "version", "expect": "Track3r o3sdk", "show": True}, {"cmd": "version", "expect": "Track3r o3sdk"}]})
        write("leds_pads", {"steps": [{"leds": {"lit": [14], "unlit": [0, 15], "within": 0.2}}]})
        write("leds_pads_bad", {"steps": [{"leds": {"lit": [15], "within": 0.2}}]})
        write("if_leds", {"steps": [{"cmd": "voices", "if_leds": {"unlit": [14]}}, {"cmd": "sd", "if_leds": {"lit": [14], "unlit": [15]}}]})
        write("stats", {"steps": [{"cmd": "cpu", "collect": {"as": "U", "re": "render last (\\d+)"}} for _ in range(4)] + [{"stat": "U", "min_n": 4, "show": True},
                        {"stat": "U", "min_n": 1, "note": "(emptied by the first stat: this one fails)"}], "finally": [{"cmd": "version"}]})
        write("stats_ok", {"steps": [{"cmd": "cpu", "collect": {"as": "V", "re": "render last (\\d+)"}} for _ in range(4)] + [{"stat": "V", "min_n": 4, "show": True}]})
        write("stats_short", {"steps": [{"cmd": "cpu", "collect": {"as": "U", "re": "render last (\\d+)"}}, {"stat": "U", "min_n": 2}]})
        write("needs_audio", {"needs": ["audio"], "steps": [{"cmd": "version"}]})
        write("diff", {"steps": [{"shot": "d", "compare": {"png": golden, "regions": ["0,0,800,440"], "max_bad": 0.001}}]})
        write("blank", {"steps": [{"shot": "b"}]})
        write("drain", {"steps": [{"keys": ["f8"], "until": {"msg": "~^NOTHING_TO_UNDO$"}, "max": 30, "gap": 0.0}, {"state": {"msg": "~^NOTHING_TO_UNDO$"}}]})
        keep_save = {"state": {"msg": "~^SAVE_TRACK_[0-9]+_DONE$"}, "keep": {"as": "SAVED", "field": "msg", "re": "^SAVE_TRACK_([0-9]+)_DONE$"}}
        write("keep_ok", {"steps": [{"keys": ["file", "f5"], "gap": 0.0}, keep_save,
                                    {"state": {"msg": "~^SAVE_TRACK_{{SAVED}}_DONE$"}}, {"state": {"msg": "SAVE_TRACK_{{SAVED}}_DONE"}}, {"state": {"screen": "FILE"}, "ne": {"msg": "~_{{SAVED}}9_"}},
                                    {"cmd": "version", "if_state": {"msg": "~_{{SAVED}}_"}, "expect": "build mock"},
                                    {"cmd": "version", "if_state": {"msg": "~_{{SAVED}}9_"}, "expect": "NEVER"},
                                    {"cmd": "ls Projects/Track {{SAVED}}", "expect": "unknown command: ls "}]})
        write("keep_cmd_unknown", {"steps": [{"cmd": "ls Projects/Track {{SAVED}}"}]})
        write("keep_wrong", {"steps": [{"keys": ["file", "f5"], "gap": 0.0}, keep_save, {"state": {"msg": "~^SAVE_TRACK_{{SAVED}}9_DONE$"}, "within": 0.3, "poll": 0.1}]})
        write("keep_unknown", {"steps": [{"state": {"msg": "~_{{SAVED}}_"}, "soft": True}, {"state": {"msg": "~_{{NOPE}}_"}}]})
        write("keep_nomatch", {"steps": [{"keys": ["file", "f5"], "gap": 0.0}, {"state": {"screen": "FILE"}, "keep": {"as": "X", "field": "msg", "re": "^NEVER_([0-9]+)$"}}]})
        write("drain_short", {"steps": [{"keys": ["f8"], "until": {"msg": "~^NOTHING_TO_UNDO$"}, "max": 5, "gap": 0.0}]})
        write("midi_ok", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"enc": 1, "repeat": 3, "gap": 0.01}, {"midi": {"on": [60, 64, 66, 62], "exact": True, "channel": 3, "settle": 0.1}}]})
        write("midi_rel", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"midi": {"on": [0, 4, 6, 2], "relative": True, "exact": True, "settle": 0.1}}]})
        write("midi_bad", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"midi": {"on": [60, 65], "settle": 0.1}}]})
        write("midi_off_ok", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"midi": {"on": [60, 64], "exact": True, "channel": 3, "settle": 0.1, "off": True, "timeout": 2}}]})
        write("midi_off_bad", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"midi": {"on": [60, 64], "exact": True, "settle": 0.1, "off": True, "timeout": 0.3}}]})
        write("midi_off_first", {"needs": ["midi"], "steps": [{"midi": {"start": True}}, {"midi": {"on": [60], "settle": 0.1, "off": True, "timeout": 0.3}}]})

        def run(names, **kw):
            master, slave = os.openpty()
            picture = kw.pop("picture", data565)
            unit = MockUnit(master, picture=picture, volume=kw.pop("volume", "ABCD-1234"), protect=kw.pop("protect", False))
            unit.journal = kw.pop("journal", 0)
            unit.start()
            lines = []
            options = Options(scenario_dir=scen_dir, out_dir=os.path.join(tmp, "out"), arecord=arecord, golden_dir=tmp, **kw)
            report = os.path.join(tmp, "report.json")
            status = run_scenarios(names, os.ttyname(slave), options, report, log=lines.append)
            unit.stop = True
            unit.join(2.0)
            os.close(master)
            os.close(slave)
            return status, "\n".join(lines), unit, report

        status, text, unit, report = run(["all_steps"], audio_device="hw:9,9", sim=None, expect_volume="ABCD-1234")
        if status != 0:
            problems.append("all_steps: status %d:\n%s" % (status, text))
        else:
            with open(report) as f:
                steps = json.load(f)["scenarios"][0]["steps"]
            if any(s["status"] != "pass" for s in steps) or len(steps) < 18:
                problems.append("all_steps: the report says %r" % [s for s in steps if s["status"] != "pass"])
            for needle in ("key shift down", "key f7", "key shift up", "enc -20", "pad 3 down 90", "shot", "fw", "leds", "voices", "mix"):
                if not any(c.startswith(needle) for c in unit.seen):
                    problems.append("all_steps: the unit never saw %r" % needle)
            if not os.path.isfile(os.path.join(tmp, "out", "all_steps_chrome.png")):
                problems.append("all_steps: no picture was written")
        # a failing step stops the scenario, the finally steps run, the next scenario still runs
        status, text, unit, report = run(["failing", "soft"], audio_device=None)
        if status != 1 or "never run" in " ".join(unit.seen) or unit.seen.count("state") < 2 or "PASS soft" not in text or "FAIL failing" not in text:
            problems.append("failing: status %d, saw %r:\n%s" % (status, unit.seen, text))
        if "SOFT-FAIL" not in text:
            problems.append("a soft failure was not reported as one")
        # "if_reply_not": a reply that says the image lacks the command passes the step as skipped (no reading kept); a reply without it is checked as usual
        status, text, unit, report = run(["not_here", "is_here"], audio_device=None)
        with open(report) as f:
            skip_steps = json.load(f)["scenarios"][0]["steps"]
        if status != 0 or "(skipped: build mock)" not in text or "PASS not_here" not in text or "PASS is_here" not in text or "reply" in skip_steps[0]:
            problems.append("if_reply_not did not skip a matching reply: %d\n%s" % (status, text))
        status, text, unit, _ = run(["not_there"], audio_device=None)
        if status != 1 or "FAIL not_there" not in text:
            problems.append("if_reply_not hid a failure when the reply did not match it: %d\n%s" % (status, text))
        # "show": the reply of a passing command is in the log after a `|` and in the report, and only for the step that asks
        status, text, unit, report = run(["shown"], audio_device=None)
        with open(report) as f:
            shown = json.load(f)["scenarios"][0]["steps"]
        if status != 0 or "    | Track3r o3sdk" not in text or "reply" not in shown[0] or "Track3r o3sdk" not in shown[0]["reply"] or "reply" in shown[1] or text.count("    | ") != shown[0]["reply"].count("\n") + 1:
            problems.append("a shown reply was not printed and kept: %d %r\n%s" % (status, shown, text))
        # OS-ODD11: pads read back (`leds` lit / unlit), a step guarded by `if_leds`, and the mean of a collected number (`collect`, `stat`)
        status, text, unit, _ = run(["leds_pads"], audio_device=None)
        if status != 0:
            problems.append("leds lit / unlit did not pass: %d\n%s" % (status, text))
        status, text, unit, _ = run(["leds_pads_bad"], audio_device=None)
        if status != 1 or "pad 15 is dark" not in text:
            problems.append("leds lit did not fail on a dark pad: %d\n%s" % (status, text))
        status, text, unit, _ = run(["if_leds"], audio_device=None)
        if status != 0 or "voices" in unit.seen or "sd" not in unit.seen or "(skipped: pad 14 is lit" not in text:
            problems.append("if_leds did not skip / run as documented: %d %r\n%s" % (status, unit.seen, text))
        status, text, unit, report = run(["stats_ok"], audio_device=None)
        with open(report) as f:
            stat_steps = json.load(f)["scenarios"][0]["steps"]
        if status != 0 or stat_steps[-1].get("reply") != "stat V: n 4  mean 250.0  min 100  max 400":
            problems.append("stat did not report the mean of the collected values: %d %r\n%s" % (status, stat_steps[-1], text))
        status, text, unit, _ = run(["stats"], audio_device=None)
        if status != 1 or "stat U: 0 values collected" not in text:
            problems.append("stat did not empty its series: %d\n%s" % (status, text))
        status, text, unit, _ = run(["stats_short"], audio_device=None)
        if status != 1 or "1 values collected, at least 2 wanted" not in text:
            problems.append("stat accepted too few values: %d\n%s" % (status, text))
        # needs: skipped without audio (status 0), a failure with --strict; the card step: mounted, not write protected, and --expect-volume's when that was given (no marker, no flag needed)
        status, text, unit, _ = run(["needs_audio"], audio_device=None)
        if status != 0 or "SKIP needs_audio" not in text or "version" in unit.seen:
            problems.append("a scenario without its audio device was not skipped: %d %s" % (status, text))
        status, text, unit, _ = run(["needs_audio"], audio_device=None, strict=True)
        if status != 1:
            problems.append("--strict did not fail a skipped scenario")
        write("card", {"needs": ["card"], "steps": [{"card": {}}]})
        for kw, wanted in (({"expect_volume": "ABCD-1234"}, 0), ({"expect_volume": "1111-2222"}, 1), ({"expect_volume": "ABCD-1234", "protect": True}, 1), ({}, 0), ({"protect": True}, 1)):
            status, text, unit, _ = run(["card"], audio_device=None, **kw)
            if status != wanted:
                problems.append("card check %r: status %d, %d wanted: %s" % (kw, status, wanted, text))
        # OS-USBDISK U1d, the disk steps: validated; skipped (never failed, not even with --strict) off Linux; on Linux it runs without --expect-volume (OS-ANYCARD)
        for good in ({"disk": "enumerate", "within": 30, "ro": True}, {"disk": "mount"}, {"disk": "read", "size_mb": 8}, {"disk": "write", "size_mb": 100}, {"disk": "remount"}, {"disk": "eject", "within": 20}, {"disk": "gone"}):
            if validate_step(good, "t"):
                problems.append("a valid disk step was refused: %r: %s" % (good, validate_step(good, "t")))
        for bad in ({"disk": "format"}, {"disk": "enumerate", "ro": "yes"}, {"disk": "read", "size_mb": 0}, {"disk": "write", "size_mb": 5000}, {"disk": "mount", "ro": True}, {"disk": "eject", "size_mb": 4}, {"disk": "mount", "within": -1}):
            if not validate_step(bad, "t"):
                problems.append("an invalid disk step was accepted: %r" % (bad,))
        write("disk_s", {"needs": ["card", "disk"], "steps": [{"disk": "enumerate", "within": 0.1}]})
        saved_platform = globals()["PLATFORM"]
        try:
            for platform_name in ("darwin", "win32"):
                globals()["PLATFORM"] = platform_name
                for strict in (False, True):
                    status, text, unit, _ = run(["disk_s"], audio_device=None, strict=strict, expect_volume="ABCD-1234")
                    if status != 0 or "SKIP disk_s: needs disk: untested on %s" % platform_name not in text:
                        problems.append("the disk scenario was not skipped on %s (strict %s): %d %s" % (platform_name, strict, status, text))
            globals()["PLATFORM"] = "linux"
            status, text, unit, _ = run(["disk_s"], audio_device=None)
            if "SKIP disk_s" in text:
                problems.append("the disk scenario was skipped without --expect-volume (any card may be used now): %d %s" % (status, text))
        finally:
            globals()["PLATFORM"] = saved_platform
        # the MIDI capture: note-ons with running status, a note-off, real-time bytes and a note-on of velocity 0 in between; the encoder repeat sends one detent each time
        fifo = os.path.join(tmp, "midi.fifo")
        os.mkfifo(fifo)
        keeper = os.open(fifo, os.O_RDWR)
        stream = bytes([0xF8, 0x92, 60, 100, 0xF8, 0x82, 60, 0, 0x92, 64, 90, 66, 80, 62, 0, 0xFE, 0x92, 62, 70])
        # the start step empties the buffer: the bytes are written after it, by a timer, as a unit does
        timer = threading.Timer(0.15, os.write, (keeper, stream))
        timer.start()
        status, text, unit, _ = run(["midi_ok"], audio_device=None, midi_device=fifo)
        timer.join()
        if status != 0 or unit.seen.count("enc 1") != 3:
            problems.append("the MIDI capture did not pass: %d, saw %r:\n%s" % (status, unit.seen, text))
        status, text, unit, _ = run(["midi_ok"], audio_device=None)
        if status != 0 or "SKIP midi_ok" not in text:
            problems.append("a scenario without its MIDI device was not skipped: %d %s" % (status, text))
        timer = threading.Timer(0.15, os.write, (keeper, stream))
        timer.start()
        status, text, unit, _ = run(["midi_rel"], audio_device=None, midi_device=fifo)
        timer.join()
        if status != 0:
            problems.append("a relative MIDI list did not pass: %d\n%s" % (status, text))
        timer = threading.Timer(0.1, os.write, (keeper, stream))
        timer.start()
        status, text, unit, _ = run(["midi_bad"], audio_device=None, midi_device=fifo)
        timer.join()
        if status != 1 or "MIDI notes [60, 64], expected [60, 65]" not in text:
            problems.append("a wrong note was not caught: %d\n%s" % (status, text))
        # the note-off check ("off"): a 0x8n message and a note-on of velocity 0 both end a note, the oldest open note of the same note and channel; "timeout" waits for a late one
        off_stream = bytes([0x92, 60, 100, 0xF8, 0x92, 64, 90, 0x82, 60, 0x40, 0x92, 64, 0])
        timer = threading.Timer(0.1, os.write, (keeper, off_stream))
        timer.start()
        status, text, unit, _ = run(["midi_off_ok"], audio_device=None, midi_device=fifo)
        timer.join()
        if status != 0:
            problems.append("note-ons that all got their note-off did not pass: %d\n%s" % (status, text))
        late = [threading.Timer(0.1, os.write, (keeper, off_stream[:-3])), threading.Timer(0.5, os.write, (keeper, off_stream[-3:]))]  # (the note-off of 64 comes after the settle)
        for t in late:
            t.start()
        status, text, unit, _ = run(["midi_off_ok"], audio_device=None, midi_device=fifo)
        for t in late:
            t.join()
        if status != 0:
            problems.append("a note-off that comes within the timeout was not waited for: %d\n%s" % (status, text))
        timer = threading.Timer(0.1, os.write, (keeper, off_stream[:-3]))
        timer.start()
        status, text, unit, _ = run(["midi_off_bad"], audio_device=None, midi_device=fifo)
        timer.join()
        if status != 1 or "MIDI note-off missing for 64/3 within 0.3 s" not in text:
            problems.append("a missing note-off was not caught: %d\n%s" % (status, text))
        timer = threading.Timer(0.1, os.write, (keeper, bytes([0x92, 60, 100, 0x92, 64, 90, 0x82, 60, 0])))
        timer.start()
        status, text, unit, _ = run(["midi_off_first"], audio_device=None, midi_device=fifo)  # (not exact: only the first note-on counts, the note-off of the second is not asked)
        timer.join()
        if status != 0:
            problems.append("only the counted note-ons need a note-off: %d\n%s" % (status, text))
        if midi_note_events(bytes([0xF8, 0x91, 60, 100, 0x81, 60, 0, 0xC1, 5, 0x91, 61, 0, 0x91, 62, 9, 63, 7, 0xF8, 64, 0])) != [("on", 60, 2), ("off", 60, 2), ("off", 61, 2), ("on", 62, 2), ("on", 63, 2), ("off", 64, 2)]:  # (63 and 64 ride on the running status)
            problems.append("the MIDI note events are not the note-ons and note-offs of the stream (program change, velocity 0, running status)")
        open_stream = bytes([0x90, 60, 1, 0x90, 60, 1, 0x80, 60, 0, 0x93, 60, 1, 0x80, 61, 0])  # two note-ons of 60 on channel 1, one note-off (the oldest), a note-on on channel 4, a note-off with nothing open
        if (midi_unclosed(open_stream), midi_unclosed(open_stream, 1), midi_unclosed(open_stream, 2)) != ([(60, 1), (60, 4)], [], [(60, 1)]):
            problems.append("the unclosed note-ons are not the ones no note-off ends: %r" % ((midi_unclosed(open_stream), midi_unclosed(open_stream, 1), midi_unclosed(open_stream, 2)),))
        os.close(keeper)
        # the screen compare: a different picture fails and leaves a difference picture; a blank one is refused
        other = bytes(data565[:200 * 1600]) + bytes([0xFF] * (280 * 1600))
        status, text, unit, _ = run(["diff"], audio_device=None, picture=other)
        if status != 1 or "differs from the golden" not in text or not os.path.isfile(os.path.join(tmp, "out", "diff_d_diff.png")):
            problems.append("a different picture was not caught: %d\n%s" % (status, text))
        status, text, unit, _ = run(["blank"], audio_device=None, picture=bytes(768000))
        if status != 1 or "blank" not in text:
            problems.append("a blank picture was accepted: %d %s" % (status, text))
        # "until": the keys are pressed until the state says so (a history of 23 steps: 23 undos and the press that says it is empty), at most "max" rounds
        status, text, unit, _ = run(["drain"], audio_device=None, journal=23)
        if status != 0 or unit.seen.count("key f8") != 24 or unit.journal != 0:
            problems.append("until did not drain the history: %d, %d presses:\n%s" % (status, unit.seen.count("key f8"), text))
        status, text, unit, _ = run(["drain"], audio_device=None, journal=0)
        if status != 0 or unit.seen.count("key f8") != 1:
            problems.append("until on an empty history: %d, %d presses" % (status, unit.seen.count("key f8")))
        status, text, unit, _ = run(["drain_short"], audio_device=None, journal=23)
        if status != 1 or unit.seen.count("key f8") != 5 or "still not true after 5 rounds" not in text:
            problems.append("until past max was not a failure: %d, %d presses:\n%s" % (status, unit.seen.count("key f8"), text))
        # "keep": a state step takes a group of a state field; {{NAME}} in a later state / ne / if_state value is that text; a name nothing kept (also one kept by an earlier scenario) is a failure
        status, text, unit, _ = run(["keep_ok", "keep_unknown"], audio_device=None)
        if "PASS keep_ok" not in text or "(skipped: state msg is" not in text or "FAIL keep_unknown" not in text or "{{SAVED}}: no earlier state step kept it" not in text or status != 1:
            problems.append("keep: expected keep_ok to pass and keep_unknown (the name of the scenario before) to fail: %d\n%s" % (status, text))
        if "ls Projects/Track 01" not in unit.seen:
            problems.append("keep: a {{SAVED}} in a cmd line was not replaced by the kept text, saw %r" % unit.seen)
        status, text, unit, _ = run(["keep_cmd_unknown"], audio_device=None)
        if status != 1 or "{{SAVED}}: no earlier state step kept it" not in text or any(c.startswith("ls ") for c in unit.seen):
            problems.append("keep: a cmd line with a name nothing kept was sent or passed: %d, saw %r\n%s" % (status, unit.seen, text))
        status, text, unit, _ = run(["keep_wrong"], audio_device=None)
        if status != 1 or "FAIL keep_wrong" not in text:
            problems.append("keep: a wrong kept text matched: %d\n%s" % (status, text))
        status, text, unit, _ = run(["keep_nomatch"], audio_device=None)
        if status != 1 or "does not match" not in text:
            problems.append("keep: a regex that does not match was not a failure: %d\n%s" % (status, text))
        for bad in ({"state": {"msg": "x"}, "keep": {"as": "A"}}, {"state": {"msg": "x"}, "keep": {"as": "A", "field": "msg", "re": "no group"}},
                    {"state": {"msg": "x"}, "keep": {"as": "1A", "field": "msg", "re": "(x)"}}, {"state": {"msg": "x"}, "keep": {"as": "A", "field": "msg", "re": "("}}, {"cmd": "x", "keep": {"as": "A", "field": "msg", "re": "(x)"}}):
            if not validate_step(bad, "t"):
                problems.append("an invalid keep step was accepted: %r" % bad)
        if validate_step({"state": {"msg": "~_{{A}}_"}, "keep": {"as": "A", "field": "msg", "re": "_(\\d+)_"}}, "t"):
            problems.append("a valid keep step was refused")
        for bad in ({"keys": ["f8"], "until": {"msg": "x"}}, {"keys": ["f8"], "until": {"msg": "x"}, "max": 5, "repeat": 2}, {"keys": ["f8"], "max": 5},
                    {"keys": ["f8"], "until": {"msg": "~("}, "max": 5}, {"keys": ["f8"], "until": {}, "max": 5}):
            if not validate_step(bad, "t"):
                problems.append("an invalid until step was accepted: %r" % bad)
        if validate_step({"keys": ["f8"], "until": {"msg": "~^X$"}, "max": 5}, "t"):
            problems.append("a valid until step was refused")
        # "burst": the tokens of a chord go out back to back (no gap between them, only after the round), as the same console commands as without it
        write("burst", {"steps": [{"keys": ["+f7", "+f8", "-f8", "-f7"], "burst": True, "gap": 0.5}]})
        write("burst_until", {"steps": [{"keys": ["f8"], "burst": True, "until": {"msg": "~^NOTHING_TO_UNDO$"}, "max": 30, "gap": 0.0}]})
        started = time.monotonic()
        status, text, unit, _ = run(["burst"], audio_device=None)
        took = time.monotonic() - started
        if status != 0 or unit.seen[-4:] != ["key f7 down", "key f8 down", "key f8 up", "key f7 up"]:
            problems.append("the burst did not send the chord: %d %r" % (status, unit.seen[-4:]))
        if took >= 1.0:  # four paced tokens would take 4 x 0.5 s; the burst waits one gap, after the round
            problems.append("the burst was paced between its keys: %.2f s" % took)
        status, text, unit, _ = run(["burst_until"], audio_device=None, journal=3)
        if status != 0 or unit.seen.count("key f8") != 4:
            problems.append("burst with until: %d, %d presses" % (status, unit.seen.count("key f8")))
        for bad in ({"keys": ["+f7", "f8:300"], "burst": True}, {"keys": ["f8"], "burst": "yes"}):
            if not validate_step(bad, "t"):
                problems.append("an invalid burst step was accepted: %r" % bad)
        if validate_step({"keys": ["+f7", "+f8", "-f8", "-f7"], "burst": True, "gap": 0.0}, "t") or validate_step({"keys": ["f7:300"], "burst": False}, "t"):
            problems.append("a valid burst step was refused")
        # unknown scenario and no port are usage errors
        if run_scenarios(["nope"], "/dev/null", Options(scenario_dir=scen_dir), None, log=lambda s: None) != 2:
            problems.append("an unknown scenario was not a usage error")
    if problems:
        print("o3hil self-test FAILED:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    print("o3hil self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
