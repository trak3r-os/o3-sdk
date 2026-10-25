#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The USB console is in the image, and wired (stdlib only; needs the toolchain's nm).

    check_usb_image.py check ELF [--nm arm-none-eabi-nm]
    check_usb_image.py --self-test            (also --selftest)

An image whose USB does not come up is a unit nobody can reach but by the hidden reset button. These are the ways the build can
leave the driver out or unwired without a compiler error, each seen in a vector table or a symbol table and none of them on the host:

  1. the vector table entry 16 + 53 (IRQ 53, USB0; platform/mk66/include/o3sdk/platform/mk66/irq_list.h) holds the address of
     `USB0_IRQHandler` and not of `Default_Handler`. startup.cpp gives every interrupt a weak `<name>_IRQHandler` alias of
     `Default_Handler` (the crash reporter): a build that drops usb0.cpp links without a word and the first USB interrupt is a crash
     record. The alias has the same address as `Default_Handler`, so the symbol existing proves nothing; the entry must differ from it.
     The entry needs its Thumb bit, and the symbol must be a definition;
  2. the symbol `g_bdt` (the buffer descriptor table, 512 bytes: the controller's BDT page register takes address bits 31..9) sits at
     a multiple of 512. Mangled names (`_ZN...N_1L5g_bdtE`, as track3r-os's image has it, `_ZL5g_bdt`) are found too; two different `g_bdt` addresses are an error;
  3. the string "Track3r" is in the loadable part of the image (the product string the host tools find the unit by:
     16D0:0E9F "Track3r"), not only in a symbol or debug section.

The vector table is read from the ELF's program headers (the load segment that holds address 0), the symbols from `nm`. Exit status:
0 every check passed, 1 a check failed, 2 the ELF cannot be read or nm cannot be run.
"""
import argparse
import contextlib
import io
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile

USB_IRQ = 53
USB_VECTOR = 16 + USB_IRQ
PT_LOAD = 1
EM_ARM = 40
BDT_ALIGN = 512
MARKER = b"Track3r"


class ImageError(Exception):
    """Something that is not a check result: the input cannot be read."""


def read_elf(path):
    """(load segments, as [(paddr, bytes)]) of a 32-bit little-endian ARM ELF."""
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as error:
        raise ImageError("cannot read %s: %s" % (path, error))
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ImageError("%s is not an ELF file" % path)
    if data[4] != 1 or data[5] != 1:
        raise ImageError("%s is not a 32-bit little-endian ELF" % path)
    machine, = struct.unpack_from("<H", data, 18)
    if machine != EM_ARM:
        raise ImageError("%s is for machine %d, not ARM (%d)" % (path, machine, EM_ARM))
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    if phnum == 0 or phentsize < 32 or phoff + phnum * phentsize > len(data):
        raise ImageError("%s has no usable program headers" % path)
    segments = []
    for i in range(phnum):
        kind, offset, _vaddr, paddr, filesz, _memsz, _flags, _align = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if kind == PT_LOAD and filesz > 0:
            if offset + filesz > len(data):
                raise ImageError("%s: a load segment runs past the end of the file" % path)
            segments.append((paddr, data[offset:offset + filesz]))
    if not segments:
        raise ImageError("%s has no load segment" % path)
    return segments


def read_word(segments, address):
    for paddr, content in segments:
        if paddr <= address and address + 4 <= paddr + len(content):
            return struct.unpack_from("<I", content, address - paddr)[0]
    return None


NM_LINE = re.compile(r"^([0-9a-fA-F]{8,16})\s+([A-Za-z])\s+(\S+)\s*$")


def parse_nm(text):
    """[(address, type, name)] of the lines of `nm` that name a symbol with an address (undefined symbols have none)."""
    out = []
    for line in text.splitlines():
        m = NM_LINE.match(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2), m.group(3)))
    return out


def find_symbols(symbols, name):
    """Every (address, type, name) whose name is `name`, or its mangling as a file-local (`_ZL<n>name`) or nested (`_ZN...<n>nameE`) variable."""
    pattern = re.compile(r"^(?:%s|_ZL%d%s|_ZN\w*?%d%sE)$" % (re.escape(name), len(name), re.escape(name), len(name), re.escape(name)))
    return [s for s in symbols if pattern.match(s[2])]


def problems(segments, symbols):
    """The failed checks, as strings (empty: the image passes)."""
    found = []
    # 1. the USB vector
    entry = read_word(segments, USB_VECTOR * 4)
    handlers = [s for s in find_symbols(symbols, "USB0_IRQHandler") if s[1] in "TtWwVv"]
    defaults = [s for s in find_symbols(symbols, "Default_Handler") if s[1] in "TtWw"]
    if entry is None:
        found.append("the vector table has no entry %d (offset 0x%X): no load segment holds it" % (USB_VECTOR, USB_VECTOR * 4))
    elif not handlers:
        found.append("no USB0_IRQHandler definition in the symbol table (usb0.cpp is not in the image)")
    elif not defaults:
        found.append("no Default_Handler in the symbol table: the vector table cannot be told from the fallback")
    else:
        target = entry & ~1
        usb = {s[0] & ~1 for s in handlers}
        fallback = {s[0] & ~1 for s in defaults}
        if entry & 1 == 0:
            found.append("vector %d (USB0, IRQ %d) is 0x%08X: no Thumb bit, the first interrupt would be a UsageFault" % (USB_VECTOR, USB_IRQ, entry))
        elif target in fallback:
            found.append("vector %d (USB0, IRQ %d) is 0x%08X, Default_Handler: USB0_IRQHandler is only the weak alias (usb0.cpp is not linked in)" % (USB_VECTOR, USB_IRQ, entry))
        elif target not in usb:
            found.append("vector %d (USB0, IRQ %d) is 0x%08X, which is neither USB0_IRQHandler (%s) nor Default_Handler" %
                         (USB_VECTOR, USB_IRQ, entry, ", ".join("0x%08X" % a for a in sorted(usb))))
    # 2. the buffer descriptor table
    bdts = find_symbols(symbols, "g_bdt")
    addresses = sorted({s[0] for s in bdts})
    if not bdts:
        found.append("no g_bdt in the symbol table (the USB driver's buffer descriptor table is not in the image)")
    elif len(addresses) > 1:
        found.append("g_bdt is defined %d times, at %s" % (len(addresses), ", ".join("0x%08X" % a for a in addresses)))
    elif addresses[0] % BDT_ALIGN != 0:
        found.append("g_bdt is at 0x%08X, not a multiple of %d: the controller takes the table's address bits 31..9" % (addresses[0], BDT_ALIGN))
    # 3. the identity string
    if not any(MARKER in content for _paddr, content in segments):
        found.append('the string "Track3r" is not in the loadable image (the host tools find the unit by it)')
    return found


def run_nm(nm, elf):
    try:
        done = subprocess.run([nm, elf], capture_output=True, text=True)
    except OSError as error:
        raise ImageError("cannot run %s: %s" % (nm, error))
    if done.returncode != 0:
        raise ImageError("%s failed (%d): %s" % (nm, done.returncode, done.stderr.strip()[:300]))
    return done.stdout


def check(elf, nm):
    try:
        segments = read_elf(elf)
        symbols = parse_nm(run_nm(nm, elf))
    except ImageError as error:
        print("FAIL usb_image: %s" % error)
        return 2
    found = problems(segments, symbols)
    if found:
        print("FAIL usb_image: %s (%d problem(s)):" % (elf, len(found)))
        for item in found:
            print("  " + item)
        return 1
    entry = read_word(segments, USB_VECTOR * 4)
    bdt = find_symbols(symbols, "g_bdt")[0][0]
    print("PASS usb_image: vector %d (USB0) is USB0_IRQHandler at 0x%08X, g_bdt at 0x%08X (%d-aligned), \"Track3r\" in the image" % (USB_VECTOR, entry & ~1, bdt, BDT_ALIGN))
    return 0


# ---- self-test: synthetic ELF files and a stand-in nm -------------------------------------------------------------------------

def make_elf(vectors, flash_extra=b"", extra_segments=(), strtab=b"", machine=EM_ARM, base=0):
    """A minimal ELF32 LE executable: one PT_LOAD at `base` (0) holding `vectors` (a dict {index: word}, 128 words) and flash_extra, more
    segments as [(paddr, bytes)], and `strtab` as file content that no segment loads (a symbol table's strings)."""
    words = [0] * 128
    for index, word in vectors.items():
        words[index] = word
    flash = struct.pack("<128I", *words) + flash_extra
    segments = [(base, flash)] + list(extra_segments)
    phoff = 52
    offset = phoff + 32 * len(segments)
    headers = b""
    body = b""
    for paddr, content in segments:
        headers += struct.pack("<8I", PT_LOAD, offset + len(body), paddr, paddr, len(content), len(content), 5, 4)
        body += content
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    header = ident + struct.pack("<HHIIIIIHHHHHH", 2, machine, 1, 0x1D, phoff, 0, 0x05000000, 52, 32, len(segments), 0, 0, 0)
    return header + headers + body + strtab


def make_nm_text(symbols):
    return "".join("%08x %s %s\n" % s for s in symbols)


# (the g_bdt mangling is the one track3r-os's release image has for usb0.cpp's anonymous-namespace variable)
GOOD_SYMBOLS = [(0x00000101, "T", "Reset_Handler"), (0x00000201, "T", "Default_Handler"), (0x00000201, "W", "SysTick_IRQHandler"),
                (0x00000201, "W", "USB1_IRQHandler"), (0x00000301, "T", "USB0_IRQHandler"), (0x1FFF0200, "b", "_ZN4o3sdk8platform4mk664usb012_GLOBAL__N_1L5g_bdtE")]
GOOD_FLASH = b"padding\0Track3r\0padding"


def self_test():
    failures = 0
    cases = [0]
    with tempfile.TemporaryDirectory() as tmp:
        fake_nm = os.path.join(tmp, "fake-nm")
        with open(fake_nm, "w") as handle:  # prints the text beside the ELF: <elf>.nm
            handle.write('#!/bin/sh\ncat "$1.nm"\n')
        os.chmod(fake_nm, os.stat(fake_nm).st_mode | stat.S_IXUSR)
        count = [0]

        def build(name, symbols, vectors=None, flash=GOOD_FLASH, **kw):
            count[0] += 1
            path = os.path.join(tmp, "%02d-%s.elf" % (count[0], name.replace(" ", "_").replace("/", "_")))
            with open(path, "wb") as handle:
                handle.write(make_elf({0: 0x20030000, 1: 0x101, USB_VECTOR: 0x301} if vectors is None else vectors, flash, **kw))
            with open(path + ".nm", "w") as handle:
                handle.write(make_nm_text(symbols) if not isinstance(symbols, str) else symbols)
            return path

        def expect(label, path, status, text=None):
            nonlocal failures
            cases[0] += 1
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                got = check(path, fake_nm)
            if got != status or (text is not None and text not in out.getvalue()):
                failures += 1
                print("self-test FAILED: %s: status %d, %d expected%s\n    %s" % (label, got, status, (", wanting %r" % text) if text else "", out.getvalue().strip().replace("\n", "\n    ")))

        without = lambda name: [s for s in GOOD_SYMBOLS if name not in s[2]]
        expect("a good image", build("good", GOOD_SYMBOLS), 0, "PASS usb_image: vector 69 (USB0) is USB0_IRQHandler at 0x00000300")
        expect("the vector is the alias of Default_Handler", build("alias", [s for s in GOOD_SYMBOLS if s[2] != "USB0_IRQHandler"] + [(0x201, "W", "USB0_IRQHandler")],
                                                                  vectors={0: 0x20030000, USB_VECTOR: 0x201}), 1, "only the weak alias")
        expect("no USB0_IRQHandler at all", build("nousb", without("USB0_IRQHandler")), 1, "no USB0_IRQHandler definition")
        expect("an undefined USB0_IRQHandler", build("undef", without("USB0_IRQHandler") + [(0x300, "U", "USB0_IRQHandler")]), 1, "no USB0_IRQHandler definition")
        expect("the entry is another handler", build("other", GOOD_SYMBOLS, vectors={USB_VECTOR: 0x101}), 1, "neither USB0_IRQHandler")
        expect("the entry is a neighbour's slot (IRQ 54 filled, 53 empty)", build("empty", GOOD_SYMBOLS, vectors={USB_VECTOR + 1: 0x301}), 1, "vector 69 (USB0, IRQ 53) is 0x00000000")
        expect("no Thumb bit", build("arm", GOOD_SYMBOLS, vectors={USB_VECTOR: 0x300}), 1, "no Thumb bit")
        expect("no Default_Handler symbol", build("nodefault", [s for s in GOOD_SYMBOLS if "Default" not in s[2]]), 1, "no Default_Handler")
        expect("USB0_IRQHandler as a weak definition replaced by the strong one", build("weak", [(0x301, "W", "USB0_IRQHandler") if s[2] == "USB0_IRQHandler" else s for s in GOOD_SYMBOLS]), 0)
        expect("g_bdt misaligned (256)", build("bdt256", [(0x1FFF0100, "b", "g_bdt") if "g_bdt" in s[2] else s for s in GOOD_SYMBOLS]), 1, "not a multiple of 512")
        expect("g_bdt off by four", build("bdt4", [(0x1FFF0204, "B", "g_bdt") if "g_bdt" in s[2] else s for s in GOOD_SYMBOLS]), 1, "0x1FFF0204")
        expect("g_bdt plain name, 1024 aligned", build("bdt1k", [(0x20000400, "B", "g_bdt") if "g_bdt" in s[2] else s for s in GOOD_SYMBOLS]), 0)
        expect("g_bdt file-local mangling", build("bdtl", [(0x20000200, "b", "_ZL5g_bdt") if "g_bdt" in s[2] else s for s in GOOD_SYMBOLS]), 0)
        expect("no g_bdt", build("nobdt", without("g_bdt")), 1, "no g_bdt in the symbol table")
        expect("a symbol that only ends like g_bdt", build("lookalike", without("g_bdt") + [(0x20000200, "b", "my_g_bdt"), (0x20000204, "b", "g_bdt_size"), (0x20000208, "b", "_ZN3foo7my_g_bdtE")]),
               1, "no g_bdt in the symbol table")
        expect("g_bdt twice", build("bdt2", GOOD_SYMBOLS + [(0x20000400, "b", "g_bdt")]), 1, "defined 2 times")
        expect("the same g_bdt under two names is one", build("bdtsame", GOOD_SYMBOLS + [(0x1FFF0200, "b", "g_bdt")]), 0)
        expect("no Track3r", build("nostring", GOOD_SYMBOLS, flash=b"padding\0Track4r\0trackr\0"), 1, '"Track3r" is not in the loadable image')
        expect("Track3r only in a section nothing loads", build("unloaded", GOOD_SYMBOLS, flash=b"padding", strtab=b"\0Track3r\0"), 1, '"Track3r" is not in the loadable image')
        expect("Track3r in a second load segment", build("segment2", GOOD_SYMBOLS, flash=b"padding", extra_segments=[(0x1FFF0000, b"\0Track3r diag\0")]), 0)
        expect("every problem is listed", build("all", without("g_bdt") + [(0x201, "W", "USB0_IRQHandler")], vectors={USB_VECTOR: 0x201}, flash=b""), 1, "3 problem(s)")
        # inputs that are not images
        garbage = os.path.join(tmp, "garbage.elf")
        with open(garbage, "wb") as handle:
            handle.write(b"not an elf" * 20)
        expect("a file that is not an ELF", garbage, 2, "not an ELF")
        expect("a missing file", os.path.join(tmp, "missing.elf"), 2, "cannot read")
        path = os.path.join(tmp, "x86.elf")
        with open(path, "wb") as handle:
            handle.write(make_elf({}, GOOD_FLASH, machine=62))
        expect("an x86 ELF", path, 2, "not ARM")
        path = os.path.join(tmp, "short.elf")
        with open(path, "wb") as handle:
            handle.write(make_elf({}, GOOD_FLASH)[:60])
        expect("a truncated ELF", path, 2, "program headers")
        path = os.path.join(tmp, "high.elf")
        with open(path, "wb") as handle:  # the only load segment is nowhere near address 0: the table is not there
            handle.write(make_elf({}, GOOD_FLASH, base=0x1000))
        with open(path + ".nm", "w") as handle:
            handle.write(make_nm_text(GOOD_SYMBOLS))
        expect("no segment at address 0", path, 1, "no load segment holds it")
        good = build("good2", GOOD_SYMBOLS)
        cases[0] += 3
        with contextlib.redirect_stdout(io.StringIO()):
            status = check(good, os.path.join(tmp, "no-such-nm"))
        if status != 2:
            failures += 1
            print("self-test FAILED: a missing nm gave status %d, 2 expected" % status)
        os.remove(good + ".nm")
        with contextlib.redirect_stdout(io.StringIO()):
            status = check(good, fake_nm)  # nm fails (cat of a missing file)
        if status != 2:
            failures += 1
            print("self-test FAILED: a failing nm gave status %d, 2 expected" % status)
        # the nm parser: the lines the real tool prints
        parsed = parse_nm("         U __aeabi_dadd\n00000a41 T main\n1fff0200 b _ZL5g_bdt\n\nnot a line\n0000000000000300 t local\n")
        if parsed != [(0xA41, "T", "main"), (0x1FFF0200, "b", "_ZL5g_bdt"), (0x300, "t", "local")]:
            failures += 1
            print("self-test FAILED: parse_nm: %r" % (parsed,))
    print("check_usb_image self-test: %s (%d cases)" % ("ok" if not failures else "%d failure(s)" % failures, cases[0]))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", "--selftest", dest="self_test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    c = sub.add_parser("check")
    c.add_argument("elf")
    c.add_argument("--nm", default="arm-none-eabi-nm", help="the toolchain's nm (default: arm-none-eabi-nm on the PATH)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "check":
        return check(args.elf, args.nm)
    parser.error("a command or --self-test is required")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
