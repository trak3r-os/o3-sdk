#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3install.py: install a firmware image on the Tracker over USB with one command, and say the way back.

    python3 -I tools/o3install.py build-mk66/app/<id>.ptf --yes [--name NAME] [--port PORT] [--expect-version TEXT]
    python3 -I tools/o3install.py --self-test

It is a script over platform/mk66/tools/o3console.py, which does the talking and stays as it is; this tool only orders its steps and stops at the
first one that fails, with the console's own exit status. The steps:
  1. check:   check_image.py (the gate's static check of the image) on the file; an image that fails it is never sent, and the unit is not touched.
              The build label the unit must report afterwards is read out of the image (the banner `Track3r diag - <label> - 'help'`);
              --expect-version TEXT replaces it, and an image with neither stops here.
  2. look:    `version` and `fw`: what runs now, the card (mounted, writable, Firmware/ there) and the images on it. The name the file gets on
              the card is --name, or <stem>-<crc32>.ptf: the same build always has the same name.
  3. put:     the file goes to Firmware/ (the console's --put) unless that name is already on the card with the same size. A name that is there with
              another size is refused: the card is not changed; use another --name. A put of an existing name is refused by the unit, and a
              file that is there under the right name and size is updated as it stands: the unit checks the CRC when it arms the update,
              so a file that is not this image stops the install (exit 4).
  4. update:  the console's --update with the CRC: the unit leaves for the bootloader, flashes, and comes back; the console waits for it on
              USB (--reboot-timeout seconds) and asks `version`, which must contain the label.
Before the update it prints the way back: the images the card holds (the console's --update of one of them with its CRC) and the card's
first .ptf, the file the hidden reset button flashes.

Because the update restarts the unit, nothing is sent without --yes (the console's --yes-update): without it the tool checks the image, prints
what it would do and exits 1, with no device access. Only with the maintainer's go, and only an image that passed the gate (docs/NEW-APP.md, section 11).

Exit status (the console's): 0 installed and `version` matches, 1 a usage error or no --yes, 2 no prompt from the unit, 3 no usable card, 4 the image
failed its check or the unit or the card refused, 5 the unit did not come back, 6 `version` does not match, 7 the bootloader did not take over.

--console and --check-image name other scripts for these two (the self-test uses fakes: no device is ever touched by it). Python 3 standard
library only; run it with `python3 -I`.
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import zlib

SDK_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(SDK_ROOT, "platform", "mk66", "tools")
CONSOLE = os.path.join(TOOLS, "o3console.py")
CHECK_IMAGE = os.path.join(TOOLS, "check_image.py")
# What the build's check holds an image to (o3console.py's own constants; the gate's values for this unit).
EXPECT_SP = "0x20030000"
RAMFUNC_L = "0x1FFF0000:0xC000"
NAME_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,47}\.ptf$", re.IGNORECASE | re.ASCII)
LABEL_RE = re.compile(rb"Track3r diag - (.{1,120}?) - 'help'")
MAX_IMAGE = 4 * 1024 * 1024  # the console refuses more


class Stop(Exception):
    """Ends the run with `status` after printing `message`."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def say(text):
    print("o3install: " + text, flush=True)


def crc32_of(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def default_name(path, crc):
    """<stem>-<crc32 in lower case hex>.ptf, the stem cut so the whole name stays inside the 48 characters the unit takes."""
    stem = re.sub(r"[^0-9A-Za-z._-]", "_", os.path.splitext(os.path.basename(path))[0]).lstrip("._-") or "image"
    return "%s-%08x.ptf" % (stem[:48 - len(".ptf") - 9], crc)


def valid_name(name):
    return bool(NAME_RE.fullmatch(name)) and ".." not in name


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def image_label(path, check_image=CHECK_IMAGE):
    """The build label the image says about itself (its console banner), or None. The hex is read with check_image.py's own parser."""
    try:
        parse = load_module(check_image, "o3install_check_image").parse_ihex
        with open(path, "r", encoding="ascii") as f:
            memory = parse(f.read())
    except (Exception, SystemExit):  # a file that is no image has no label; the check says why
        return None
    if not memory:
        return None
    blob = bytearray(max(memory) + 1)
    for address, byte in memory.items():
        blob[address] = byte
    found = LABEL_RE.search(bytes(blob))
    return found.group(1).decode("ascii", "replace") if found else None


def run_check(path, check_image, out):
    """check_image.py `check`: True when it passes (its report is printed when it does not)."""
    done = subprocess.run([sys.executable, "-I", check_image, "check", path, "--expect-sp", EXPECT_SP, "--ramfunc-l", RAMFUNC_L],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if done.returncode != 0:
        print(done.stdout, file=out, end="")
        return False
    return True


class Console:
    """o3console.py as a subprocess, `python3 -I`, with the port when one was given."""

    def __init__(self, script, port=None, timeout=None):
        self.script = script
        self.prefix = (["--port", port] if port else []) + (["--timeout", str(timeout)] if timeout else [])

    def command_line(self, args):
        return [sys.executable, "-I", self.script] + self.prefix + list(args)

    def call(self, args, echo):
        """Runs it: (exit status, its output). `echo` prints the lines as they come (a put and an update take a while)."""
        try:
            proc = subprocess.Popen(self.command_line(args), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        except OSError as error:
            raise Stop(1, "cannot run %s: %s" % (self.script, error))
        lines = []
        for line in proc.stdout:
            lines.append(line)
            if echo:
                print("  | " + line.rstrip("\n"), flush=True)
        return proc.wait(), "".join(lines)


def parse_fw(text):
    """What `fw` says: the write path, the card and the images in Firmware/ in directory order, as a dict."""
    info = {"enabled": False, "why": "", "mounted": False, "dir": False, "write_protect": False, "files": [], "first": None}
    found = re.search(r"^fw: write path (ENABLED|OFF: ?(.*))$", text, re.MULTILINE)
    if found:
        info["enabled"] = found.group(1) == "ENABLED"
        info["why"] = (found.group(2) or "").strip()
    card = re.search(r"^card: volume (\S+), .*Firmware/ (yes|no), write-protect (yes|no)", text, re.MULTILINE)
    if card:
        info["mounted"] = True
        info["volume"] = card.group(1).rstrip(",")
        info["dir"] = card.group(2) == "yes"
        info["write_protect"] = card.group(3) == "yes"
    in_list = False
    for line in text.splitlines():
        if line.startswith("Firmware/ in directory order"):
            in_list = True
            continue
        if line.startswith("upload:"):
            in_list = False
        entry = re.match(r"^\s*(\*)?\s*(\d+)\s+(\S+)\s*$", line) if in_list else None
        if entry:
            info["files"].append({"name": entry.group(3), "size": int(entry.group(2))})
            if entry.group(1):
                info["first"] = entry.group(3)
    return info


def is_ptf(name):
    return name.lower().endswith(".ptf")


def version_line(text):
    """The first non-empty line of a `version` reply."""
    for line in text.splitlines():
        if line.strip():
            return line.strip()
    return ""


def look(console):
    """`version` and `fw`: (the version line, the parsed fw). The console's status is the failure's."""
    status, text = console.call(["version", "fw"], False)
    if status != 0:
        print(text, end="")
        raise Stop(status, "cannot read the unit (the console exited %d)" % status)
    head, _, fw_text = text.partition("fw: write path")
    return version_line(head), parse_fw("fw: write path" + fw_text)


def refuse_card(info):
    if not info["enabled"]:
        raise Stop(4, "the unit's firmware write path is off (%s): nothing was sent" % (info["why"] or "no reason given"))
    if not info["mounted"]:
        raise Stop(3, "there is no card mounted in the unit: nothing was sent")
    if not info["dir"]:
        raise Stop(4, "the card has no Firmware/ folder: make it by hand, nothing was sent")
    if info["write_protect"]:
        raise Stop(4, "the card is write-protected: nothing was sent")


def print_way_back(console, name, info, before, expect):
    others = [f["name"] for f in info["files"] if is_ptf(f["name"]) and f["name"] != name]
    say("the way back (the unit runs: %s):" % (before or "unknown"))
    if others:
        say("  by the console, an image already on the card. The card does not list a file's CRC; for a file that is only there, ask the unit: "
            "`files crc Firmware/<name>` (the console's, the unit running this image has it), or read it in that image's package CANDIDATE.md")
        for other in others[:4]:
            say("    python3 -I %s%s --update %s --crc <crc32 of %s> --yes-update --expect-version <its version>" % (
                console.script, (" " + " ".join(console.prefix)) if console.prefix else "", other, other))
        say("    (`files crc Firmware/%s`, run through %s%s)" % (others[0], console.script, (" " + " ".join(console.prefix)) if console.prefix else ""))
    else:
        say("  WARNING: no other .ptf is on the card, so the console has no earlier image to go back to: a known-good image goes into Firmware/ first "
            "(docs/NEW-APP.md, section 11)")
    first = info["first"]
    say("  by hand, the hidden reset button flashes the card's first .ptf: %s%s" % (first or "(none listed)", " (the image just put there)" if first == name else ""))


def install(args, out=None):
    """The whole run; returns the exit status."""
    path = args.image
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as error:
        raise Stop(1, "cannot read %s: %s" % (path, error))
    if not data or len(data) > MAX_IMAGE:
        raise Stop(1, "%s is empty or over 4 MiB" % path)
    crc = crc32_of(data)
    name = args.name or default_name(path, crc)
    if not valid_name(name):
        raise Stop(1, "%r is not a name the unit accepts (<1..48 of 0-9A-Za-z._-, first alphanumeric>.ptf, no '..'): give --name" % name)
    say("image %s: %d bytes, CRC-32 %08X, to be Firmware/%s" % (path, len(data), crc, name))

    # 1. check: nothing reaches the unit before the image has passed.
    if not run_check(path, args.check_image, sys.stdout):
        raise Stop(4, "%s does not pass check_image.py: not sent, the unit was not touched" % path)
    say("check_image.py: passed")
    expect = args.expect_version or image_label(path)
    if not expect:
        raise Stop(1, "no build label in the image (an image built without the console has none): give --expect-version TEXT, the text `version` must contain")
    say("after the update `version` must contain: %s" % expect)
    if not args.yes:
        raise Stop(1, "dry run, the unit was not touched: the update restarts it, so add --yes (only with the maintainer's go, for an image that passed the gate)")

    console = Console(args.console, args.port, args.timeout)
    extra = (["--expect-volume", args.expect_volume] if args.expect_volume else [])

    # 2. look.
    before, info = look(console)
    say("the unit runs: %s" % (before or "(no version line)"))
    refuse_card(info)
    say("card: volume %s, %d file(s) in Firmware/, first .ptf %s" % (info.get("volume", "?"), len(info["files"]), info["first"] or "(none)"))
    on_card = [f for f in info["files"] if f["name"] == name]
    if on_card and on_card[0]["size"] != len(data):
        raise Stop(4, "Firmware/%s is on the card with %d bytes, this image has %d: not the same file. Nothing was changed; give another --name" % (
            name, on_card[0]["size"], len(data)))

    # 3. put.
    if on_card:
        say("Firmware/%s is already on the card with the same size: it is updated as it stands (the unit checks its CRC)" % name)
    else:
        status, text = console.call(["--put", path, "--name", name] + extra, True)
        if status != 0 and re.search(r"already exists", text) is None:
            raise Stop(status, "the put failed (the console exited %d)" % status)
        if status != 0:
            say("the unit says Firmware/%s exists already: updating that file" % name)
        else:
            done = re.search(r"^ok (\S+) (\d+) ([0-9A-Fa-f]{8}) image (\d+)", text, re.MULTILINE)
            if not done or done.group(1) != name or int(done.group(2)) != len(data) or int(done.group(3), 16) != crc:
                raise Stop(4, "the unit's reply does not match what was sent (name, size, CRC): not updating")
            say("put ok: Firmware/%s, CRC %08X" % (name, crc))
            _, info = look(console)  # a put can move the card's first .ptf

    # The way back, before the unit leaves: it is in front of the user whatever happens next.
    print_way_back(console, name, info, before, expect)

    # 4. update, and wait for the unit to come back with `version`.
    update = ["--update", name, "--crc", "%08X" % crc, "--yes-update", "--expect-version", expect] + extra
    if args.reboot_timeout:
        update += ["--reboot-timeout", str(args.reboot_timeout)]
    say("updating: the unit restarts into the bootloader, flashes and comes back on USB")
    status, text = console.call(update, True)
    if status != 0:
        hint = ""
        if on_card and status == 4:
            hint = " (the file on the card under that name is not this image if the unit says the CRC differs: give another --name)"
        raise Stop(status, "the update failed (the console exited %d)%s" % (status, hint))
    say("installed: the unit reports %s" % (version_line(text.split("waiting for it to come back", 1)[-1]) or expect))
    say("the way back is above; the card keeps Firmware/%s" % name)
    return 0


# ---- self-test: a fake console and a fake check, no device ---------------------------------------------------------------------

FAKE_CONSOLE = r'''
import json, os, sys, zlib

STATE = os.environ["O3INSTALL_FAKE_STATE"]
state = json.load(open(STATE))
argv = sys.argv[1:]
state["log"].append(argv)
rc = 0

def opt(name):
    return argv[argv.index(name) + 1] if name in argv else None

def crc_hex(path):
    return zlib.crc32(open(path, "rb").read()) & 0xFFFFFFFF

def fw_text():
    c = state["card"]
    t = "fw: write path " + ("ENABLED" if c["enabled"] else "OFF: " + c["why"]) + "\n"
    if c["mounted"]:
        t += "card: volume ABCD-1234, FAT32, Firmware/ %s, write-protect %s\n" % ("yes" if c["dir"] else "no", "yes" if c["wp"] else "no")
    else:
        t += "card: not mounted\n"
    t += "Firmware/ in directory order (* = the .ptf the reset button flashes):\n"
    first = True
    for f in c["files"]:
        t += "  %s%9d  %s\n" % ("* " if first and f["name"].endswith(".ptf") else "  ", f["size"], f["name"])
        first = first and not f["name"].endswith(".ptf")
    t += "upload: idle\nlast: nothing yet\n"
    return t

if opt("--put"):
    path, name = opt("--put"), opt("--name")
    data = open(path, "rb").read()
    names = [f["name"] for f in state["card"]["files"]]
    if state.get("put_rc"):
        print("ERR " + state.get("put_err", "write failed") + " at 0")
        rc = state["put_rc"]
    elif name in names:
        print("ERR already exists")
        rc = 4
    else:
        state["card"]["files"].append({"name": name, "size": len(data), "crc": crc_hex(path)})
        print("ok %s %d %08X image %d" % (name, len(data), crc_hex(path) ^ state.get("put_crc_flip", 0), len(data) // 2))
        print("first-ptf " + next(f["name"] for f in state["card"]["files"] if f["name"].endswith(".ptf")))
elif opt("--update"):
    name, crc = opt("--update"), int(opt("--crc"), 16)
    found = [f for f in state["card"]["files"] if f["name"] == name]
    if "--yes-update" not in argv:
        print("o3console: --update restarts the unit into the bootloader: add --yes-update", file=sys.stderr)
        rc = 1
    elif not found:
        print("ERR not found")
        rc = 4
    elif found[0].get("crc") is not None and found[0]["crc"] != crc:
        print("ERR the card changed since arm")
        rc = 4
    elif state.get("update_rc"):
        print("o3console: scripted failure")
        rc = state["update_rc"]
    else:
        print("o3console: the unit left (hand-over to the bootloader); waiting for it to come back")
        state["version"] = state["after_version"]
        print(state["after_version"])
        want = opt("--expect-version")
        if want and want not in state["after_version"]:
            print("o3console: the version does not contain %r" % want)
            rc = 6
else:
    for command in [a for i, a in enumerate(argv) if not a.startswith("--") and (i == 0 or argv[i - 1] not in ("--port", "--timeout"))]:
        if command == "version":
            print(state["version"] + "\nusb 16D0:0E9F rev 0001 serial 0123456789ABCDEF")
        elif command == "fw":
            print(fw_text(), end="")
    rc = state.get("look_rc", 0)

json.dump(state, open(STATE, "w"))
sys.exit(rc)
'''

FAKE_CHECK = r'''
import sys
sys.exit(int(open(sys.argv[2] + ".verdict").read().strip()) if len(sys.argv) > 2 else 3)
'''


def make_hex(path, label=None):
    """A small Intel HEX file: bytes 0..0x5FF of a pattern, the console banner (when `label`) at 0x500. Not a valid image for the real check."""
    blob = bytearray(range(256)) * 6
    if label is not None:
        banner = ("Track3r diag - %s - 'help'" % label).encode()
        blob[0x500:0x500 + len(banner)] = banner
    lines = []
    for address in range(0, len(blob), 16):
        chunk = blob[address:address + 16]
        record = bytes([len(chunk), address >> 8, address & 0xFF, 0]) + bytes(chunk)
        lines.append(":" + (record + bytes([(-sum(record)) & 0xFF])).hex().upper())
    lines.append(":00000001FF")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


class Bench:
    """One scenario's directory: an image, the fake console's state, and the helpers that run install() against them."""

    def __init__(self, root, label="abc123def456 / o3sdk 0123456789ab"):
        self.dir = tempfile.mkdtemp(prefix="o3install-", dir=root)
        self.image = os.path.join(self.dir, "demo app.ptf")
        make_hex(self.image, label)
        self.console = os.path.join(self.dir, "fake_console.py")
        with open(self.console, "w") as f:
            f.write(FAKE_CONSOLE)
        self.check = os.path.join(self.dir, "fake_check.py")
        with open(self.check, "w") as f:
            f.write(FAKE_CHECK)
        with open(self.image + ".verdict", "w") as f:
            f.write("0")
        self.state_path = os.path.join(self.dir, "state.json")
        self.state = {"version": "Track3r o3sdk, build old", "after_version": "Track3r o3sdk, build %s" % label, "log": [],
                      "card": {"enabled": True, "why": "", "mounted": True, "dir": True, "wp": False,
                               "files": [{"name": "o3os_1.0.0.ptf", "size": 700000}]}}
        self.save()

    def save(self):
        with open(self.state_path, "w") as f:
            json.dump(self.state, f)

    def load(self):
        with open(self.state_path) as f:
            return json.load(f)

    def run(self, *extra, yes=True, image=None):
        os.environ["O3INSTALL_FAKE_STATE"] = self.state_path
        argv = [image or self.image, "--console", self.console, "--check-image", self.check] + (["--yes"] if yes else []) + list(extra)
        parsed = build_parser().parse_args(argv)
        self.save()
        text = io_capture()
        try:
            status = install(parsed)
        except Stop as stop:
            print("o3install: %s" % stop, flush=True)
            status = stop.status
        output = text.finish()
        self.state = self.load()  # what the fake console did to the card stays for a second run
        return status, output

    def calls(self, kind):
        return [c for c in self.load()["log"] if (kind in c or (kind == "look" and "version" in c))]


class io_capture:
    """Captures stdout of this process (print and the subprocess echo go through sys.stdout) for the scenario."""

    def __init__(self):
        import io
        self.buffer = io.StringIO()
        self.saved = sys.stdout
        sys.stdout = self.buffer

    def finish(self):
        sys.stdout = self.saved
        return self.buffer.getvalue()


def self_test():
    failures = []

    def expect(condition, what, text=""):
        if not condition:
            failures.append(what + (("\n" + text) if text else ""))

    # Pure parts.
    expect(default_name("build/app/o3_303.ptf", 0x1A2B3C4D) == "o3_303-1a2b3c4d.ptf", "default name")
    expect(valid_name(default_name("x" * 100 + ".ptf", 1)) and len(default_name("x" * 100 + ".ptf", 1)) <= 48, "a long stem is cut inside 48")
    expect(default_name("a b$c.ptf", 255) == "a_b_c-000000ff.ptf", "odd characters become '_'")
    expect(valid_name("a.ptf") and not valid_name("../a.ptf") and not valid_name("a.bin") and not valid_name(".a.ptf") and not valid_name("a..b.ptf"), "name rule")
    fw = parse_fw("fw: write path ENABLED\ncard: volume ABCD-1234, FAT32, Firmware/ yes, write-protect no\nFirmware/ in directory order (* = the .ptf the reset button flashes):\n"
                  "    12345  readme.txt\n  *    700000  o3os_1.0.0.ptf\n      9999  b.ptf\nupload: idle\n")
    expect(fw["enabled"] and fw["mounted"] and fw["dir"] and not fw["write_protect"] and fw["first"] == "o3os_1.0.0.ptf", "fw status parsed")
    expect([f["name"] for f in fw["files"]] == ["readme.txt", "o3os_1.0.0.ptf", "b.ptf"] and fw["files"][1]["size"] == 700000, "fw listing parsed")
    off = parse_fw("fw: write path OFF: card write protected\ncard: not mounted\n")
    expect(not off["enabled"] and off["why"] == "card write protected" and not off["mounted"], "fw off parsed")
    expect(version_line("\n  Track3r o3sdk, build x\nusb 16D0\n") == "Track3r o3sdk, build x", "version line")

    with tempfile.TemporaryDirectory() as root:
        # The label of an image, and no label.
        b = Bench(root)
        expect(image_label(b.image, CHECK_IMAGE) == "abc123def456 / o3sdk 0123456789ab", "label read from the image")
        plain = os.path.join(b.dir, "plain.ptf")
        make_hex(plain)
        expect(image_label(plain, CHECK_IMAGE) is None, "no banner, no label")
        expect(image_label(os.path.join(b.dir, "absent.ptf"), CHECK_IMAGE) is None, "no file, no label")
        garbage = os.path.join(b.dir, "garbage.ptf")
        with open(garbage, "w") as f:
            f.write("this is not intel hex\n")
        expect(image_label(garbage, CHECK_IMAGE) is None, "garbage, no label")

        # 1. A fresh install: look, put, look, update; the CRC and the label reach the console.
        b = Bench(root)
        status, text = b.run("--port", "/dev/fake0")
        crc = crc32_of(open(b.image, "rb").read())
        name = default_name(b.image, crc)
        expect(status == 0, "fresh install exits 0", text)
        log = b.load()["log"]
        expect([c[2] if c[:2] == ["--port", "/dev/fake0"] else None for c in log][:1] == ["version"], "the first call is the look, with the port", str(log))
        puts = [c for c in log if "--put" in c]
        updates = [c for c in log if "--update" in c]
        expect(len(puts) == 1 and puts[0][puts[0].index("--name") + 1] == name, "one put, under <stem>-<crc>.ptf", str(log))
        expect(len(updates) == 1 and updates[0][updates[0].index("--update") + 1] == name and updates[0][updates[0].index("--crc") + 1] == "%08X" % crc
               and "--yes-update" in updates[0] and updates[0][updates[0].index("--expect-version") + 1] == "abc123def456 / o3sdk 0123456789ab", "one update with the CRC and the label", str(log))
        expect(all(c[:2] == ["--port", "/dev/fake0"] for c in log), "the port goes to every call", str(log))
        expect(log.index(puts[0]) < log.index(updates[0]), "put before update")
        expect("the way back" in text and "o3os_1.0.0.ptf" in text and "--update o3os_1.0.0.ptf" in text and "reset button" in text, "the way back is printed", text)
        expect("files crc Firmware/o3os_1.0.0.ptf" in text and "CANDIDATE.md" in text and "<crc32 of" in text, "the way back says how to get a file's CRC", text)
        expect("installed: the unit reports" in text, "the end says it is installed", text)
        expect(b.state["card"]["files"][0]["name"] == "o3os_1.0.0.ptf" and "dry run" not in text, "sanity")

        # 2. Run again: the name is on the card with the same size: no put, an update of that file.
        status, text = b.run()
        expect(status == 0, "a re-run exits 0", text)
        log = b.load()["log"]
        expect(len([c for c in log if "--put" in c]) == 1 and len([c for c in log if "--update" in c]) == 2, "no second put, a second update", str(log))
        expect("already on the card with the same size" in text, "the re-run says so", text)
        expect("--port" not in log[-1], "no port when none was given", str(log[-1]))

        # 3. The name is on the card with another size: refused, nothing sent.
        b = Bench(root)
        b.state["card"]["files"].append({"name": name, "size": 5})
        status, text = b.run()
        expect(status == 4 and "give another --name" in text, "same name, other size: exit 4", text)
        expect(not b.calls("--put") and not b.calls("--update"), "and neither put nor update", str(b.load()["log"]))

        # 4. The name is there with the right size but other content: the unit's CRC check refuses the update (exit 4) and the hint says why.
        b = Bench(root)
        b.state["card"]["files"].append({"name": name, "size": len(open(b.image, "rb").read()), "crc": 1})
        status, text = b.run()
        expect(status == 4 and "another --name" in text, "same size, other content: the update is refused with exit 4", text)
        expect(not b.calls("--put"), "no put over an existing file")

        # 5. An image that fails the check: never sent, the console is not called at all.
        b = Bench(root)
        with open(b.image + ".verdict", "w") as f:
            f.write("1")
        status, text = b.run()
        expect(status == 4 and "does not pass check_image.py" in text, "a failing image exits 4", text)
        expect(b.load()["log"] == [], "the console was never called", str(b.load()["log"]))

        # 6. No --yes: a dry run that touches nothing.
        b = Bench(root)
        status, text = b.run(yes=False)
        expect(status == 1 and "dry run" in text and "--yes" in text, "no --yes: exit 1", text)
        expect(b.load()["log"] == [], "and no device access", str(b.load()["log"]))

        # 7. The put fails: its status, no update. Then the unit's own refusal of a name that exists although the listing lacked it (a race): updated.
        for code in (2, 4):
            b = Bench(root)
            b.state["put_rc"] = code
            status, text = b.run()
            expect(status == code and not b.calls("--update"), "a put that exits %d stops with %d and no update" % (code, code), text)
        b = Bench(root)
        b.state["put_crc_flip"] = 1
        status, text = b.run()
        expect(status == 4 and "does not match what was sent" in text and not b.calls("--update"), "a put whose ok line has another CRC is not updated", text)

        # 8. The update's statuses pass through.
        for code in (2, 4, 5, 6, 7):
            b = Bench(root)
            b.state["update_rc"] = code
            status, text = b.run()
            expect(status == code, "an update that exits %d: %d" % (code, status), text)
        b = Bench(root)
        status, text = b.run("--expect-version", "not the label")
        expect(status == 6 and "the version does not contain" in text, "a version that does not match: 6", text)

        # 9. The card.
        for label, field, value, code in (("not mounted", "mounted", False, 3), ("write path off", "enabled", False, 4), ("no Firmware/", "dir", False, 4), ("write-protected", "wp", True, 4)):
            b = Bench(root)
            b.state["card"][field] = value
            b.state["card"]["why"] = "scripted"
            status, text = b.run()
            expect(status == code and not b.calls("--put") and not b.calls("--update"), "card %s: exit %d and nothing sent" % (label, code), text)
        b = Bench(root)
        b.state["look_rc"] = 2
        status, text = b.run()
        expect(status == 2 and not b.calls("--put"), "no prompt from the unit while looking: 2", text)

        # 10. The label: none in the image and none given is a usage error; --expect-version stands in.
        b = Bench(root, label=None)
        status, text = b.run()
        expect(status == 1 and "--expect-version" in text and b.load()["log"] == [], "no label, no --expect-version: exit 1, no device access", text)
        b = Bench(root, label=None)
        status, text = b.run("--expect-version", "build abc")
        b.state["after_version"] = "x"
        expect(status == 6 or status == 0, "with --expect-version the run goes on")
        expect(any("build abc" in c for c in b.load()["log"] if "--update" in c), "and the console gets it")

        # 11. A name the unit would refuse; a missing file; a missing console.
        b = Bench(root)
        status, text = b.run("--name", "bad name.ptf")
        expect(status == 1 and b.load()["log"] == [], "a bad --name is exit 1", text)
        status, text = b.run(image=os.path.join(b.dir, "absent.ptf"))
        expect(status == 1, "a missing image is exit 1", text)
        parsed = build_parser().parse_args([b.image, "--yes", "--console", os.path.join(b.dir, "none.py"), "--check-image", b.check])
        os.environ["O3INSTALL_FAKE_STATE"] = b.state_path
        capture = io_capture()
        try:
            status = install(parsed)
        except Stop as stop:
            status = stop.status
        capture.finish()
        expect(status != 0, "a missing console is not a success")

        # 12. The real check_image.py on a file that is no image: refused with 4 before the console is called.
        b = Bench(root)
        junk = os.path.join(b.dir, "junk.ptf")
        make_hex(junk, "abc")  # well-formed hex, but its vectors, SP and flash configuration are wrong
        parsed = build_parser().parse_args([junk, "--yes", "--console", b.console])
        os.environ["O3INSTALL_FAKE_STATE"] = b.state_path
        b.save()
        capture = io_capture()
        try:
            status = install(parsed)
        except Stop as stop:
            status = stop.status
        capture.finish()
        expect(status == 4 and b.load()["log"] == [], "the real check refuses a bad image (exit 4, console untouched)")

        # 13. The way back warns when the card holds no other image.
        b = Bench(root)
        b.state["card"]["files"] = []
        status, text = b.run()
        expect(status == 0 and "WARNING: no other .ptf is on the card" in text, "no earlier image on the card: a warning", text)

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("o3install self-test: ok")
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description="Install a firmware image on the Tracker over USB (check, put, update, wait, version) and print the way back.")
    parser.add_argument("image", nargs="?", help="the .ptf to install (the build's, e.g. build-mk66/app/<id>.ptf)")
    parser.add_argument("--yes", action="store_true", help="the update restarts the unit: confirm it (the maintainer's go); without it nothing touches the unit")
    parser.add_argument("--name", help="the file name on the card (default <stem>-<crc32>.ptf)")
    parser.add_argument("--port", help="the unit's serial port (default: the console's own pick)")
    parser.add_argument("--expect-version", metavar="TEXT", help="what `version` must contain afterwards (default: the build label found in the image)")
    parser.add_argument("--expect-volume", metavar="XXXX-XXXX", help="refuse unless the card's volume serial is this")
    parser.add_argument("--timeout", type=float, help="seconds the console waits for each reply")
    parser.add_argument("--reboot-timeout", type=float, help="seconds to wait for the unit to come back (console default 120)")
    parser.add_argument("--console", default=CONSOLE, help=argparse.SUPPRESS)
    parser.add_argument("--check-image", default=CHECK_IMAGE, help=argparse.SUPPRESS)
    parser.add_argument("--self-test", action="store_true", help="run the checks of this tool against a fake console (no device) and exit")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.image:
        parser.print_usage(sys.stderr)
        print("o3install: give the .ptf to install", file=sys.stderr)
        return 1
    try:
        return install(args)
    except Stop as stop:
        print("o3install: %s" % stop, file=sys.stderr if stop.status else sys.stdout)
        return stop.status


if __name__ == "__main__":
    sys.exit(main())
