#!/usr/bin/env python3
"""PreToolUse(Bash|Edit|Write|NotebookEdit) guard: nothing that could flash a unit or touch a card.

CLAUDE.md (Hard rules) and docs/M2-PLAN.md (Rules): firmware reaches a unit only through the documented update path (the
bootloader's .ptf from the SD card, a step the maintainer performs); no debug-probe flashing, no improvised uploaders; formatting a
card, or deleting files you did not put there, needs the maintainer's ask; "never write to a card" while milestone M2 is open.

What it refuses (exit code 2, the reason is shown to the model):
  1. a hardware uploader, debug probe or console updater in command position (teensy_loader_cli, tycmd upload, openocd, pyocd, J-Link,
     dfu-util, st-flash, probe-rs, ..., o3console --put/--update);
  2. a raw block-device write or a partition/format tool (dd of=/dev/sdX, > /dev/mmcblk0, mkfs, wipefs, fdisk, parted, ...);
  3. any write to removable media (/media, /run/media, /mnt) by a shell command or by the Edit/Write tools. Reads are fine. The
     maintainer opens a card for one purpose (handing over a verified .ptf) by putting O3_ALLOW_CARD_WRITE=1 in the "env" of
     .claude/settings.local.json (see settings.local.json.example); a command prefix does not count. Even then only a mounted volume
     under /media/<user>/<vol> or /run/media/<user>/<vol> is writable (never /mnt), and formatting stays refused;
  4. `git add` naming a firmware image or other binary (.ptf .hex .elf .bin .a .o .wav): no binaries in git.

Fails open on a payload it cannot parse. `guard_flash.py --selftest` checks the rules.
"""
import json
import os
import re
import shlex
import sys

# A tool only counts when it is in command position, so `grep teensy_loader_cli` or a doc that mentions these names passes.
CMD_START = (r"(?:^|[;&|`(\n]|\$\(|\bsudo\s+|\bexec\s+|\benv\s+(?:\S+=\S+\s+)*|\bnice\s+(?:-n\s*-?\d+\s+)?"
             r"|\btime\s+|\btimeout\s+\S+\s+)\s*(?:\S*/)?")
UPLOADERS = (
    r"teensy_loader_cli|teensy_post_compile|teensy_reboot|teensy_restart"
    r"|openocd|pyocd|probe-rs|JLinkExe|JLink|JLinkGDBServer\w*|dfu-util|st-flash|st-util"
    r"|bossac|nrfjprog|blhost|kinetis-flash|arm-none-eabi-gdb"
)
DISK_TOOLS = r"mkfs(?:\.\w+)?|mke2fs|mkswap|wipefs|fdisk|sfdisk|cfdisk|gdisk|sgdisk|parted|blkdiscard"
BLOCKDEV = re.compile(r"^/dev/(?:sd[a-z]|mmcblk\d|nvme\d|disk\d|hd[a-z]|vd[a-z]|loop\d|sr\d|mapper/)")

FLASH_WHY = ("Updating hardware is a physical action that needs a verified target, a passed gate and the maintainer's go. "
             "Produce the candidate image and its SHA-256 (skill fw-gate-check) and stop: the maintainer carries the .ptf to "
             "the unit by the bootloader's SD update path.")
CARD_WHY = ("Agents do not write to removable media. Work in the repository or the scratchpad; the maintainer copies a verified "
            ".ptf to a card himself, or opens one card for you with O3_ALLOW_CARD_WRITE=1 in .claude/settings.local.json "
            "(a mounted volume under /media or /run/media only). Formatting a card is always the maintainer's.")

RULES = [
    (re.compile(CMD_START + r"(?:" + UPLOADERS + r")\b"), "hardware uploader or debug-probe tool", FLASH_WHY),
    (re.compile(CMD_START + r"tycmd\s+(?:upload|reset|bootloader)\b"), "tycmd upload/reset", FLASH_WHY),
    (re.compile(r"\bo3console(?:\.py)?\b[^|;&]*--(?:put|update|yes-update)\b"), "firmware update over the console", FLASH_WHY),
    (re.compile(CMD_START + r"(?:" + DISK_TOOLS + r")\b"), "partition or format tool", CARD_WHY),
    (re.compile(r"\bdd\b[^|;&]*\bof=/dev/(?:sd|mmcblk|nvme|disk|hd|vd|loop|mapper/)"), "dd to a block device", CARD_WHY),
    (re.compile(r">\s*/dev/(?:sd[a-z]|mmcblk\d|nvme\d|disk\d|hd[a-z]|vd[a-z])"), "redirect into a block device", CARD_WHY),
    (re.compile(r"\bgit\s+add\b[^|;&]*\.(?:ptf|hex|elf|bin|a|o|wav)(?:\s|$|['\"])"),
     "staging a firmware image or binary (no binaries in git)", "Images and build products stay in build*/ (untracked)."),
]

MEDIA = ("/media/", "/run/media/", "/mnt/")
WRITABLE_MEDIA = ("/media/", "/run/media/")
MEDIA_PATH = re.compile(r"(?:/run/media/|/media/|/mnt/)[^\s'\";|&<>]*")
WRITES_LAST_ARG = {"cp", "rsync", "install", "scp"}
WRITES_ANY_ARG = {"mv", "rm", "rmdir", "touch", "mkdir", "tee", "truncate", "ln", "chmod", "chown", "shred", "unlink"}
OPERATORS = {";", "&", "|", "&&", "||", "(", ")", "\n"}
REDIRECTS = {">", ">>", ">|", "&>", "1>", "2>"}
WRAPPERS = {"sudo", "env", "exec", "nice", "time", "command", "ionice"}
INTERP = re.compile(r"\b(?:python[0-9.]*|perl|ruby|node(?:js)?)\b")
SCRIPT_WRITE = re.compile(r"write|shutil|\.copy|copyfile|unlink|remove\(|rename\(|replace\(|mkdir|rmtree|os\.system"
                          r"""|open\([^)]*['"][rwabxt]*[wax+]""")


HEREDOC = re.compile(r"<<-?\s*(['\"]?)(\w+)\1")
DATA_CONSUMER = re.compile(r"(?:^|[\s(`\"'])(?:cat|tee)\b[^|;&]*$")  # the last command before `<<` is cat or tee


def strip_data_heredocs(command):
    """The command without the bodies of heredocs that only feed `cat` or `tee` (text written to a file is data, not a command:
    a doc that mentions openocd must not look like an openocd call). Heredocs fed to bash, python and the like are kept."""
    out, tag = [], None
    for line in command.split("\n"):
        if tag is not None:
            if line.strip() == tag:
                tag = None
                out.append(line)
            continue
        out.append(line)
        match = HEREDOC.search(line)
        if match and DATA_CONSUMER.search(line[:match.start()]):
            tag = match.group(2)
    return "\n".join(out)


def _media_root(path):
    """Mount root: /media/<user>/<vol>, /run/media/<user>/<vol> or /mnt/<vol>."""
    for prefix, depth in (("/run/media/", 2), ("/media/", 2), ("/mnt/", 1)):
        if path.startswith(prefix):
            parts = [p for p in path[len(prefix):].split("/") if p]
            return prefix + "/".join(parts[:depth]) if len(parts) >= depth else None
    return None


def card_writable(path, allow_card, ismount):
    """True when the maintainer opened cards (O3_ALLOW_CARD_WRITE=1) and path is on a mounted /media or /run/media volume."""
    root = _media_root(path)
    return bool(allow_card and root and root.startswith(WRITABLE_MEDIA) and ismount(root))


def _segments(command):
    """Split a shell command into simple commands; quotes are respected."""
    lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|()<>")
    lexer.whitespace_split = True
    segment = []
    try:
        for tok in lexer:
            if tok in OPERATORS:
                yield segment
                segment = []
            else:
                segment.append(tok)
    except ValueError:  # unbalanced quotes: fall back to a plain split
        segment = command.split()
    yield segment


def _strip_wrappers(toks):
    while toks and (os.path.basename(toks[0]) in WRAPPERS or re.match(r"^\w+=", toks[0])):
        wrapper = os.path.basename(toks[0])
        toks = toks[1:]
        if wrapper in ("nice", "ionice"):
            while toks and (toks[0].startswith("-") or toks[0].isdigit()):
                toks = toks[1:]
    return toks


def _destinations(cmd, toks):
    """The paths a simple command writes to (a best-effort list)."""
    args = [t for t in toks[1:] if not t.startswith("-")]
    out = []
    for i, t in enumerate(toks[1:], 1):
        if t in ("-t", "--target-directory") and i + 1 < len(toks):
            out.append(toks[i + 1])
        elif t.startswith("--target-directory="):
            out.append(t.split("=", 1)[1])
    if cmd in WRITES_LAST_ARG and args:
        out.append(args[-1])
    if cmd == "mv" or cmd in WRITES_ANY_ARG:
        out += args  # mv also removes its sources
    if cmd == "sed" and any(t.startswith("-i") or t.startswith("--in-place") for t in toks[1:]):
        out += args
    if cmd == "dd":
        out += [t[3:] for t in toks[1:] if t.startswith("of=")]
    if cmd == "unzip":
        out += [toks[i + 1] for i, t in enumerate(toks[:-1]) if t == "-d"]
    return out


def media_write(command, allow_card, ismount):
    """The reason a shell command writes to a block device or to media that is not open for writing, else None."""
    def blocked(path):
        if BLOCKDEV.match(path):
            return "writing to a block device"
        if path.startswith(MEDIA) and not card_writable(path, allow_card, ismount):
            return "writing to removable media" + ("" if allow_card else " (cards are closed to agents)")
        return None

    for toks in _segments(command):
        for i, tok in enumerate(toks[:-1]):
            if tok in REDIRECTS:
                what = blocked(toks[i + 1])
                if what:
                    return "redirecting output: " + what
        toks = _strip_wrappers(toks)
        if not toks:
            continue
        cmd = os.path.basename(toks[0])
        for dest in _destinations(cmd, toks):
            what = blocked(dest)
            if what:
                return f"{cmd}: {what}"
    paths = MEDIA_PATH.findall(command)
    if paths and INTERP.search(command) and SCRIPT_WRITE.search(command):
        if not all(card_writable(p, allow_card, ismount) for p in paths):
            return "a script that writes near removable media"
    return None


def evaluate(tool_input, allow_card=False, ismount=os.path.ismount):
    """None to let the call through, else (what, why)."""
    path = tool_input.get("file_path") or tool_input.get("notebook_path") or ""
    if path.startswith(MEDIA) and not card_writable(path, allow_card, ismount):
        return "editing a file on removable media", CARD_WHY
    command = strip_data_heredocs(tool_input.get("command") or "")
    if not command:
        return None
    for pattern, what, why in RULES:
        if pattern.search(command):
            return what, why
    what = media_write(command, allow_card, ismount)
    return (what, CARD_WHY) if what else None


SELFTEST = [
    # (tool_input, allow_card, blocked?)
    ({"command": "teensy_loader_cli --mcu=TEENSY36 -w fw.hex"}, False, True),
    ({"command": "cd build && sudo openocd -f x.cfg"}, False, True),
    ({"command": "nice -n 10 pyocd flash fw.hex"}, False, True),
    ({"command": "/opt/segger/JLinkExe -device MK66FX1M0"}, False, True),
    ({"command": "tycmd upload fw.hex"}, False, True),
    ({"command": "probe-rs download fw.elf"}, False, True),
    ({"command": "python3 platform/mk66/tools/o3console.py --put x.ptf"}, False, True),
    ({"command": "python3 -I o3console.py --update x --crc 1 --yes-update"}, False, True),
    ({"command": "dd if=hello.ptf of=/dev/sdb bs=4M"}, False, True),
    ({"command": "dd if=hello.ptf of=/dev/sdb bs=4M"}, True, True),
    ({"command": "cat hello.hex > /dev/mmcblk0"}, True, True),
    ({"command": "mkfs.vfat /dev/sdb1"}, True, True),
    ({"command": "sudo wipefs -a /dev/sdb"}, True, True),
    ({"command": "git add build-mk66/apps/hello/hello.ptf"}, False, True),
    ({"command": "git add apps/hello/src/hello_app.cpp && git add *.hex"}, False, True),
    ({"command": "cp hello.ptf /media/user/CARD/Firmware/"}, False, True),
    ({"command": "cp -t /media/user/CARD/Firmware hello.ptf"}, False, True),
    ({"command": "nice -n 10 cp hello.ptf /run/media/user/CARD/Firmware/"}, False, True),
    ({"command": "cp hello.ptf /mnt/disk/Firmware/"}, True, True),
    ({"command": "mv /media/user/CARD/Firmware/x.ptf /tmp/"}, False, True),
    ({"command": "rm /media/user/CARD/Firmware/x.ptf"}, False, True),
    ({"command": "echo x > /media/user/CARD/a.txt"}, False, True),
    ({"command": "sed -i s/a/b/ /media/user/CARD/a.txt"}, False, True),
    ({"command": "unzip pack.zip -d /media/user/CARD/"}, False, True),
    ({"command": "python3 -c \"import shutil; shutil.copy('hello.ptf','/media/user/CARD/')\""}, False, True),
    ({"file_path": "/media/user/CARD/notes.txt"}, False, True),
    ({"file_path": "/mnt/disk/notes.txt"}, True, True),
    # open cards: a mounted volume under /media or /run/media may be written, nothing else
    ({"command": "cp hello.ptf /media/user/CARD/Firmware/"}, True, False),
    ({"command": "cp hello.ptf /media/user/OTHER/Firmware/"}, True, True),   # not mounted
    ({"command": "python3 -c \"import shutil; shutil.copy('a','/media/user/CARD/')\""}, True, False),
    ({"file_path": "/media/user/CARD/notes.txt"}, True, False),
    # reads, builds and mentions pass
    ({"command": "cmake --build build-host -j4 2>&1 | tail -20"}, False, False),
    ({"command": "nice -n 10 ctest --test-dir build-host --output-on-failure"}, False, False),
    ({"command": "build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello"}, False, False),
    ({"command": "grep -rn openocd docs/ && grep st-flash README.md"}, False, False),
    ({"command": "git log --oneline | grep -i teensy_loader_cli"}, False, False),
    ({"command": "python3 -I platform/mk66/tools/check_image.py check build-mk66/apps/hello/hello.hex"}, False, False),
    ({"command": "sha256sum build-mk66/apps/hello/hello.ptf"}, False, False),
    ({"command": "cp /media/user/CARD/Firmware/x.ptf /tmp/x.ptf"}, False, False),
    ({"command": "ls -la /media/user/CARD/Firmware/ && cat /media/user/CARD/readme.txt"}, False, False),
    ({"command": "rsync -a /media/user/CARD/ /tmp/backup/"}, False, False),
    ({"command": "dd if=/dev/zero of=/tmp/blank.img bs=1M count=1"}, False, False),
    ({"command": "echo hi > /dev/null"}, False, False),
    ({"command": "git add apps/hello/src/hello_app.cpp CMakeLists.txt"}, False, False),
    ({"command": "git commit -m 'drop the old st-flash note'"}, False, False),
    ({"command": "python3 - <<'EOF'\nprint(open('/media/user/CARD/readme.txt').read())\nEOF"}, False, False),
    ({"command": "nice -n 10 cmake -S . -B build-m2-audio-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=/opt/arm-gnu-toolchain"}, False, False),
    ({"command": "ctest --test-dir build-m2-audio-host -L mk66 -R 'audio' --output-on-failure"}, False, False),
    ({"command": "arm-none-eabi-nm -u build-mk66/kit/dsp/libo3sdk_dsp.a | grep __aeabi_d"}, False, False),
    ({"command": "cp -r ../track3r-os/platform/mk66/audio platform/mk66/"}, False, False),
    ({"command": "sed -i 's/o3os/o3sdk/g' platform/mk66/audio/slot_ring.cpp && rm -rf build-m2-audio-host"}, False, False),
    ({"command": "python3 -I tools/font_convert.py --out /tmp/font && mkdir -p /tmp/hello"}, False, False),
    ({"command": "cat > docs/notes.md <<'EOF'\nDebug probes (openocd, st-flash) are not used.\n| openocd | no |\nEOF"}, False, False),
    ({"command": "cd docs && cat >> notes.md <<'EOF'\n; openocd -f x.cfg\nEOF\ngit status"}, False, False),
    ({"command": "git commit -m \"$(cat <<'EOF'\nfix: drop the st-flash note\nopenocd is gone\nEOF\n)\""}, False, False),
    ({"command": "cat notes.txt | python3 - <<'EOF'\nimport subprocess\nsubprocess.run(['x'])\nopenocd -f x.cfg\nEOF"}, False, True),
    ({"command": "bash <<'EOF'\nopenocd -f x.cfg\nEOF"}, False, True),
    ({"command": "cat > /media/user/CARD/a.txt <<'EOF'\nhello\nEOF"}, False, True),
    ({"command": "cat <<EOF > x.txt\nhi\nEOF\nopenocd -f x.cfg"}, False, True),
    ({"file_path": "/home/user/Code/o3-sdk/kit/core/src/ring.cpp"}, False, False),
    ({"file_path": "/home/user/Code/o3-sdk/kit/core/src/ring.cpp", "old_string": "a", "new_string": "b"}, False, False),
]


def selftest():
    def mounted(path):
        return path == "/media/user/CARD"
    bad = []
    for tool_input, allow, want in SELFTEST:
        got = evaluate(tool_input, allow, mounted) is not None
        if got != want:
            bad.append((tool_input, allow, want))
    for tool_input, allow, want in bad:
        print(f"FAIL want {'block' if want else 'pass'} (allow_card={allow}): {tool_input!r}")
    print(f"{len(SELFTEST) - len(bad)}/{len(SELFTEST)} ok")
    return 1 if bad else 0


def main() -> int:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    try:
        payload = json.load(sys.stdin)
    except json.JSONDecodeError:
        return 0
    if not isinstance(payload, dict):
        return 0
    verdict = evaluate(payload.get("tool_input") or {}, os.environ.get("O3_ALLOW_CARD_WRITE") == "1")
    if verdict:
        print(f"BLOCKED by project guard: {verdict[0]}. {verdict[1]}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
