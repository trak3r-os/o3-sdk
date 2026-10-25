#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The audio hot path calls nothing in flash (stdlib only; needs the toolchain's objdump).

    check_ramfunc.py check ELF --objdump arm-none-eabi-objdump [--allow-empty]
    check_ramfunc.py --self-test

Code in `.ramfunc_l` runs from SRAM_L, 512 MiB of address space away from flash, where a plain BL cannot reach: the linker
quietly inserts a veneer for every such call, which works but costs cycles and puts a flash fetch (wait states, cache) into a path
that must be jitter-free (ADR 0013 D1, D7). The classic causes are a zeroing or copying loop the compiler turns into memset or
memcpy, and a small helper that did not get inlined. This check disassembles `.ramfunc_l` and fails if any direct branch or call
leaves the section or goes through a veneer. Calls through a register (function pointers: the cycle counter, the render
callback) are indirect and allowed: they are visible in the source.

An image with no `.ramfunc_l` (or an empty one) fails, because the audio path is supposed to be in it; `--allow-empty` passes it
instead, for an image built without the audio driver (the boot slice's skeleton, O3SDK_MK66_SKELETON).
"""
import argparse
import re
import subprocess
import sys

BRANCH = re.compile(r"^\s*([0-9a-f]+):\s+(b|bl|blx|b\.w|beq|bne|bcs|bhs|bcc|blo|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble|cbz|cbnz|"
                    r"beq\.w|bne\.w|bcs\.w|bcc\.w|bmi\.w|bpl\.w|bhi\.w|bls\.w|bge\.w|blt\.w|bgt\.w|ble\.w)\s+(?:r\d+,\s*)?([0-9a-f]{3,8})\s+<([^>]*)>")


def section_range(objdump, elf, section):
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1] == section:
            size = int(fields[2], 16)
            start = int(fields[3], 16)
            return start, start + size
    return None


def violations(disassembly, start, end):
    found = []
    for line in disassembly.splitlines():
        m = BRANCH.match(line)
        if not m:
            continue
        where, mnemonic, target, label = int(m.group(1), 16), m.group(2), int(m.group(3), 16), m.group(4)
        if "veneer" in label:
            found.append("0x%08x %s -> %s (a long-call veneer: the callee is not in SRAM_L)" % (where, mnemonic, label))
        elif not start <= target < end:
            found.append("0x%08x %s -> 0x%08x <%s> (outside .ramfunc_l)" % (where, mnemonic, target, label))
    return found


def check(elf, objdump, allow_empty=False):
    region = section_range(objdump, elf, ".ramfunc_l")
    if region is None or region[0] == region[1]:
        if allow_empty:
            print("PASS ramfunc: %s has no hot code in .ramfunc_l (allowed: an image without the audio path)" % elf)
            return 0
        print("FAIL ramfunc: %s has no .ramfunc_l (is the audio path built into the image?)" % elf)
        return 1
    start, end = region
    text = subprocess.run([objdump, "-d", "--no-show-raw-insn", "-j", ".ramfunc_l", elf], capture_output=True, text=True, check=True).stdout
    found = violations(text, start, end)
    if found:
        print("FAIL ramfunc: the hot path leaves SRAM_L (%d site(s)):" % len(found))
        for item in found:
            print("  " + item)
        return 1
    print("PASS ramfunc: .ramfunc_l 0x%08x..0x%08x makes no direct call or branch out of itself (indirect calls through registers are allowed)" % (start, end))
    return 0


def self_test():
    failures = 0
    base = "Disassembly of section .ramfunc_l:\n\n1fff0000 <f>:\n"
    cases = [
        ("inside call", "1fff0004:\tbl\t1fff0040 <g>\n", 0),
        ("inside branch", "1fff0008:\tbeq.n\t1fff0010 <f+0x10>\n1fff000a:\tb.n\t1fff0010 <f+0x10>\n", 0),
        ("indirect call", "1fff000c:\tblx\tr3\n", 0),
        ("literal data", "1fff0010:\t.word\t0x00000ff0\n", 0),
        ("call into flash", "1fff0004:\tbl\t00000a40 <memset>\n", 1),
        ("tail call into flash", "1fff0004:\tb.w\t00000a40 <memcpy>\n", 1),
        ("veneer", "1fff0004:\tbl\t1fff0040 <___ZN4o3sdk5audio11AudioStatus4bumpE_veneer>\n", 1),
        ("past the end", "1fff0004:\tbl\t1fff1000 <h>\n", 1),
    ]
    for label, body, expected in cases:
        got = len(violations(base + body, 0x1FFF0000, 0x1FFF0100))
        if (got != 0) != (expected != 0):
            failures += 1
            print("self-test FAILED: %s: %d violation(s), expected %s" % (label, got, "some" if expected else "none"))
    print("check_ramfunc self-test: %s (%d cases)" % ("ok" if not failures else "%d failure(s)" % failures, len(cases)))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    c = sub.add_parser("check")
    c.add_argument("elf")
    c.add_argument("--objdump", default="arm-none-eabi-objdump")
    c.add_argument("--allow-empty", action="store_true", help="pass an image whose .ramfunc_l is empty or absent")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "check":
        return check(args.elf, args.objdump, args.allow_empty)
    parser.error("a command or --self-test is required")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
