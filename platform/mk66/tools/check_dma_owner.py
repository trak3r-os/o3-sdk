#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Only boot::dma_init() writes the eDMA's global configuration (stdlib only).

    check_dma_owner.py check [--root platform/mk66]
    check_dma_owner.py --self-test

ADR 0013 D1 and hazard H11: DMA_CR and the sixteen channel arbitration priorities (DCHPRIn) are written once, from the table in
resources.h, by `boot::dma_init()` in the reset sequence, before the display and the audio engine start. A driver that writes
either of them on its own (OS-007c's display did, temporarily, before the merge) would reorder the channels under a running audio
stream. This check reads the platform's sources (comments and string literals removed) and fails if:

  1. anything but `src/boot.cpp` (the writer) and `src/regs.h` (the accessors' definitions) names `dma_dchpri`, `DMA_CR`,
     `DMA_DCHPRI` or `DCHPRI`;
  2. anything else uses the accessor `dma_cr()` other than to read it in the forms `dma_cr() & mask`, `dma_cr() == v`,
     `dma_cr() != v` (audio `start()` verifies that `dma_init()` ran: it never assigns);
  3. any file reaches the registers through a raw offset from the eDMA base (`kDma`, `kDmaBase`, `dma::kBase`) that is DMA_CR
     (+0x000) or a DCHPRI (+0x100..+0x10F), or spells the register's absolute address (the base address 0x40008000 may be
     defined or compared: `= 0x40008000u`, `== 0x40008000u`). A bare `kBase` is not matched (the I2S block has one too); a driver
     that invented a base constant under another name would still be caught by rules 1 and 2 if it used the accessors, and is a
     review item if it does not;
  4. `dma_init()` is called anywhere but once, in `src/startup.cpp`, before `o3sdk_target_main()`.

It is a source check, so it also runs in the host build; the registers' real addresses are the tests' business (mk66 ctest).
"""
import argparse
import os
import re
import sys
import tempfile

EXTENSIONS = (".h", ".hpp", ".c", ".cc", ".cpp", ".S", ".s", ".inc")
SKIP_DIRS = {"tools", ".git"}
WRITER = "src/boot.cpp"
ACCESSORS = "src/regs.h"
STARTUP = "src/startup.cpp"

NAME_RULE = re.compile(r"\b(dma_dchpri|DMA_CR|DMA_DCHPRI\w*|DCHPRI\w*)\b")
CR_USE = re.compile(r"\bdma_cr\b")
CR_READ = re.compile(r"\bdma_cr\s*\(\s*\)\s*(?:&(?![&=])|==|!=)")
RAW_OFFSET = re.compile(r"\b(?:kDma|kDmaBase|dma::kBase)\s*\+\s*(0[xX][0-9a-fA-F]+|\d+)[uUlL]*")
RAW_ADDRESS = re.compile(r"(?<!\w)0[xX]4000(8000|810[0-9a-fA-F])[uUlL]*\b")
INIT_CALL = re.compile(r"\bdma_init\s*\(\s*\)\s*;")
TARGET_MAIN_CALL = re.compile(r"\bo3sdk_target_main\s*\(\s*\)\s*;")


def strip(text):
    """Comments and the inside of string / character literals replaced by spaces (newlines kept, so line numbers hold)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.extend("\n" if ch == "\n" else " " for ch in text[i:end])
            i = end
        elif c in "\"'":
            quote = c
            out.append(c)
            i += 1
            while i < n and text[i] != quote and text[i] != "\n":
                if text[i] == "\\" and i + 1 < n:
                    out.append(" ")
                    i += 1
                out.append(" ")
                i += 1
            if i < n and text[i] == quote:
                out.append(quote)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def offset_is_forbidden(value):
    return value == 0 or 0x100 <= value <= 0x10F


def scan_file(rel, text):
    """The violations of rules 1-3 in one file."""
    found = []
    code = strip(text)
    if rel not in (WRITER, ACCESSORS):
        for m in NAME_RULE.finditer(code):
            found.append("%s:%d: names `%s`: DMA_CR and DCHPRI belong to boot::dma_init() (src/boot.cpp)" % (rel, line_of(code, m.start()), m.group(1)))
        for m in CR_USE.finditer(code):
            if not CR_READ.match(code, m.start()):
                found.append("%s:%d: `dma_cr` used other than as `dma_cr() & mask`, `== v`, `!= v` (a read): only boot::dma_init() writes it" % (rel, line_of(code, m.start())))
    if rel != ACCESSORS:
        for m in RAW_OFFSET.finditer(code):
            value = int(m.group(1), 0)
            if offset_is_forbidden(value):
                found.append("%s:%d: raw eDMA offset +0x%X is DMA_CR or a DCHPRI: only boot::dma_init() writes them" % (rel, line_of(code, m.start()), value))
        for m in RAW_ADDRESS.finditer(code):
            defined = m.group(1) == "8000" and code[:m.start()].rstrip().endswith("=")  # `kBase = 0x40008000u`, `kBase == 0x40008000u`
            if not defined:
                found.append("%s:%d: the absolute address of DMA_CR or a DCHPRI" % (rel, line_of(code, m.start())))
    return found


def collect(root):
    files = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name.endswith(EXTENSIONS):
                path = os.path.join(dirpath, name)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    files[os.path.relpath(path, root).replace(os.sep, "/")] = handle.read()
    return files


def violations(files):
    found = []
    for rel in sorted(files):
        found.extend(scan_file(rel, files[rel]))
    # Rule 4: one call, in the reset sequence, before main.
    calls = []
    for rel in sorted(files):
        for m in INIT_CALL.finditer(strip(files[rel])):
            calls.append((rel, line_of(strip(files[rel]), m.start()), m.start()))
    if len(calls) != 1:
        found.append("dma_init() must be called exactly once, in %s; found %d call(s): %s" %
                     (STARTUP, len(calls), ", ".join("%s:%d" % (c[0], c[1]) for c in calls) or "none"))
    elif calls[0][0] != STARTUP:
        found.append("%s:%d: dma_init() is called here, it belongs in %s (the reset sequence, before any driver)" % (calls[0][0], calls[0][1], STARTUP))
    else:
        code = strip(files[STARTUP])
        main_call = TARGET_MAIN_CALL.search(code)
        if main_call is None or main_call.start() < calls[0][2]:
            found.append("%s: dma_init() must come before o3sdk_target_main()" % STARTUP)
    if WRITER not in files:
        found.append("%s is missing: the writer of DMA_CR and the DCHPRIs" % WRITER)
    return found


def check(root):
    files = collect(root)
    found = violations(files)
    if found:
        print("FAIL dma_owner: %d violation(s):" % len(found))
        for item in found:
            print("  " + item)
        return 1
    print("PASS dma_owner: %d source files; DMA_CR and every DCHPRI are written only by boot::dma_init() (called once, in the reset sequence)" % len(files))
    return 0


def self_test():
    ok_boot = ("void dma_init() noexcept {\n  dma_cr() = 1u;\n  dma_dchpri(3) = 4;\n}\n")
    ok_regs = ("inline volatile uint32_t& dma_cr() noexcept { return at<uint32_t>(kBase + 0x00u); }\n"
               "inline volatile uint8_t& dma_dchpri(unsigned c) noexcept { return at<uint8_t>(kBase + 0x100u + (c ^ 3u)); }\n")
    ok_startup = "void reset() {\n  boot::dma_init();\n  o3sdk_target_main();\n}\n"
    base = {WRITER: ok_boot, ACCESSORS: ok_regs, STARTUP: ok_startup}

    def with_file(rel, text):
        files = dict(base)
        files[rel] = text
        return files

    cases = [
        # (label, files, expected number of violations is nonzero)
        ("the real shape", dict(base), False),
        ("audio reads dma_cr with a mask", with_file("src/audio.cpp", "if ((regs::dma_cr() & k) != k) { panic(); }\n"), False),
        ("a read with ==", with_file("src/audio.cpp", "bool a = regs::dma_cr() == 3u;\n"), False),
        ("other eDMA registers", with_file("display/src/dev_regs.h",
                                           "inline constexpr uintptr_t kDma = 0x40008000u;\n"
                                           "inline volatile uint8_t& dma_cdne() noexcept { return at<uint8_t>(kDma + 0x1Cu); }\n"
                                           "inline volatile uint32_t& dma_es() noexcept { return at<uint32_t>(kDma + 0x04u); }\n"), False),
        ("a comment names them", with_file("display/src/device.cpp", "// DMA_CR and DCHPRI are written by dma_init(), not here\n"), False),
        ("a string names them", with_file("src/x.cpp", 'const char* s = "DCHPRI and dma_cr() = 1";\n'), False),
        ("a block comment names them", with_file("src/x.cpp", "/* dma_dchpri(1) = 2;\n   DMA_CR */\nint x;\n"), False),
        ("the base address is defined and compared", with_file("src/x.cpp", "constexpr uint32_t kBase = 0x40008000u;\nCHECK(kBase == 0x40008000u);\n"), False),
        ("an I2S offset from a bare kBase", with_file("src/x.cpp", "constexpr uint32_t kMcr = kBase + 0x100;\n"), False),
        ("a TCD offset is not a DCHPRI", with_file("src/x.cpp", "auto& r = at<uint32_t>(kBase + 0x1000u);\n"), False),
        ("display writes a DCHPRI by accessor", with_file("display/src/device.cpp", "dev::dma_dchpri(14) = 1;\n"), True),
        ("display writes DMA_CR by accessor", with_file("display/src/device.cpp", "regs::dma_cr() = 0;\n"), True),
        ("display ORs into DMA_CR", with_file("display/src/device.cpp", "regs::dma_cr() |= 2u;\n"), True),
        ("display binds a reference to DMA_CR", with_file("display/src/device.cpp", "auto& cr = regs::dma_cr();\ncr = 5;\n"), True),
        ("display takes the address of DMA_CR", with_file("display/src/device.cpp", "volatile uint32_t* p = &regs::dma_cr();\n"), True),
        ("a re-declared accessor for DCHPRI", with_file("display/src/dev_regs.h", "inline volatile uint8_t& dma_dchpri(unsigned c) noexcept;\n"), True),
        ("raw DCHPRI offset", with_file("display/src/dev_regs.h", "at<uint8_t>(kDma + 0x10Eu) = 1;\n"), True),
        ("raw DCHPRI offset 0x100", with_file("src/x.cpp", "at<uint8_t>(audio_cfg::dma::kBase + 0x100u + (c ^ 3u)) = 1;\n"), True),
        ("raw DMA_CR offset", with_file("display/src/dev_regs.h", "at<uint32_t>(kDma + 0x00u) = 1;\n"), True),
        ("raw DMA_CR offset, decimal", with_file("display/src/dev_regs.h", "at<uint32_t>(kDma + 0) = 1;\n"), True),
        ("absolute DMA_CR address", with_file("src/x.cpp", "*(volatile uint32_t*)0x40008000u = 1;\n"), True),
        ("absolute DCHPRI address", with_file("src/x.cpp", "*(volatile uint8_t*)0x4000810Cu = 1;\n"), True),
        ("macro name", with_file("src/x.cpp", "DMA_CR = DMA_CR_EMLM;\n"), True),
        ("dma_init called from main", with_file("src/main.cpp", "int main() { boot::dma_init(); }\n"), True),
        ("dma_init never called", {WRITER: ok_boot, ACCESSORS: ok_regs, STARTUP: "void reset() { o3sdk_target_main(); }\n"}, True),
        ("dma_init called twice", {WRITER: ok_boot, ACCESSORS: ok_regs, STARTUP: "void reset() {\n boot::dma_init();\n boot::dma_init();\n o3sdk_target_main();\n}\n"}, True),
        ("dma_init after main", {WRITER: ok_boot, ACCESSORS: ok_regs, STARTUP: "void reset() {\n o3sdk_target_main();\n boot::dma_init();\n}\n"}, True),
        ("no writer", {ACCESSORS: ok_regs, STARTUP: ok_startup}, True),
    ]
    failures = 0
    for label, files, expect in cases:
        got = violations(files)
        if bool(got) != expect:
            failures += 1
            print("self-test FAILED: %s: %s" % (label, got if got else "no violation, expected some" if expect else got))
    # The same cases through the file walker, once: the directory scan finds what the in-memory scan finds.
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in with_file("display/src/device.cpp", "dev::dma_dchpri(15) = 0;\n").items():
            path = os.path.join(tmp, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)
        if check_quiet(tmp) == 0:
            failures += 1
            print("self-test FAILED: the directory walk missed a DCHPRI write in display/src/device.cpp")
    print("check_dma_owner self-test: %s (%d cases)" % ("ok" if not failures else "%d failure(s)" % failures, len(cases) + 1))
    return 1 if failures else 0


def check_quiet(root):
    return 1 if violations(collect(root)) else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    c = sub.add_parser("check")
    c.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "check":
        return check(args.root)
    parser.error("a command or --self-test is required")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
