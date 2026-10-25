#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Who may write the SD card and who may touch the bootloader request pin (stdlib only).

    check_writers.py check [--root <o3sdk dir>]
    check_writers.py --self-test            (also --selftest)

Adapted from track3r-os tools/check_layers.py, rules 6 and 7 (OS-006c); the rules that belong to the tracker's save layer and bench
(8 to 10) are not here, o3sdk has neither. It reads the first-party C and C++ sources under the root (comments and string literals
removed, so a name in a comment is not a use) and fails when:

  1. FatFs's write-class calls (f_write f_sync f_unlink f_rename f_truncate f_mkdir f_expand f_setlabel f_mkfs f_chmod f_utime
     f_forward) or the open flags that write (FA_WRITE FA_CREATE_NEW FA_CREATE_ALWAYS FA_OPEN_APPEND FA_OPEN_ALWAYS: an `f_open`
     that writes names one of them) are named in any source but platform/mk66/update/src/fat_firmware_store.cpp, which applies the
     path guard before each of them: the update path (`fw`) is the only writer of the card;
  2. `disk_write` or `disk_ioctl`, the FatFs glue to the block device, are named in any source but
     platform/mk66/storage/src/fat_storage.cpp, which defines them;
  3. `kBootloaderRequest`, the bootloader request pin (PTA1), is named in any source but platform/mk66/include/o3sdk/platform/mk66/
     board.h, which defines it, and platform/mk66/update/src/fw_host_mk66.cpp, the one place that drives it (the `fw update` hand-over).

Not scanned: tests (any path with a `tests` directory), third_party (FatFs itself), build directories, hidden directories. The
files named above need not exist: until the storage and update slices land the check is vacuous and passes; it fails the day a
writer appears anywhere else. The root is o3sdk's own tree (the default); an app project's sources may be checked with --root.
Exit status: 0 clean, 1 a violation, 2 a usage error.
"""
import argparse
import os
import re
import sys
import tempfile

SOURCE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp")
SKIPPED_DIR = re.compile(r"^(?:build(?:-.*)?|\..*|__pycache__|third_party|tests)$")

WRITE_API = re.compile(r"\b(?:f_write|f_sync|f_unlink|f_rename|f_truncate|f_mkdir|f_expand|f_setlabel|f_mkfs|f_chmod|f_utime|f_forward"
                       r"|FA_WRITE|FA_CREATE_NEW|FA_CREATE_ALWAYS|FA_OPEN_APPEND|FA_OPEN_ALWAYS)\b")
WRITE_API_USERS = {"platform/mk66/update/src/fat_firmware_store.cpp"}
DISK_WRITE_API = re.compile(r"\b(?:disk_write|disk_ioctl)\b")
DISK_WRITE_USERS = {"platform/mk66/storage/src/fat_storage.cpp"}
BOOTLOADER_REQUEST = re.compile(r"\bkBootloaderRequest\b")
BOOTLOADER_REQUEST_USERS = {"platform/mk66/include/o3sdk/platform/mk66/board.h", "platform/mk66/update/src/fw_host_mk66.cpp"}

RULES = (
    (WRITE_API, WRITE_API_USERS,
     "FatFs write-class call `%s` outside platform/mk66/update/src/fat_firmware_store.cpp (rule 1: the update path is the only writer)"),
    (DISK_WRITE_API, DISK_WRITE_USERS, "`%s` outside platform/mk66/storage/src/fat_storage.cpp (rule 2)"),
    (BOOTLOADER_REQUEST, BOOTLOADER_REQUEST_USERS,
     "`%s`, the bootloader request pin, outside board.h and platform/mk66/update/src/fw_host_mk66.cpp (rule 3)"),
)

# Comments are blanked; string and character literals are kept while scanning so that a `//` inside one is not a comment, then blanked too.
COMMENT_OR_LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)


def code_only(text):
    """The text with comments and the inside of every string / character literal replaced by spaces (newlines kept: line numbers hold)."""
    return COMMENT_OR_LITERAL.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def violations(files):
    """files: {path relative to the root, with '/': text}. A sorted list of (path, line, message)."""
    found = []
    for rel, text in files.items():
        parts = rel.split("/")
        if any(SKIPPED_DIR.match(part) for part in parts[:-1]) or not rel.endswith(SOURCE_SUFFIXES):
            continue
        code = code_only(text)
        for pattern, users, message in RULES:
            if rel in users:
                continue
            for m in pattern.finditer(code):
                found.append((rel, code.count("\n", 0, m.start()) + 1, message % m.group(0)))
    return sorted(found)


def collect(root):
    """The scanned files under root as {relative path: text}. Skipped directories are not entered."""
    files = {}
    for here, dirs, names in os.walk(root):
        dirs[:] = sorted(d for d in dirs if not SKIPPED_DIR.match(d))
        for name in sorted(names):
            if name.endswith(SOURCE_SUFFIXES):
                path = os.path.join(here, name)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    files[os.path.relpath(path, root).replace(os.sep, "/")] = handle.read()
    return files


def check(root, quiet=False):
    if not os.path.isdir(root):
        if not quiet:
            print("check_writers: %s is not a directory" % root, file=sys.stderr)
        return 2
    found = violations(collect(root))
    if not quiet:
        for rel, line, message in found:
            print("%s:%d: %s" % (rel, line, message))
        print("check_writers: %d violation(s)" % len(found) if found else "check_writers: ok")
    return 1 if found else 0


def self_test():
    store = "platform/mk66/update/src/fat_firmware_store.cpp"
    glue = "platform/mk66/storage/src/fat_storage.cpp"
    board = "platform/mk66/include/o3sdk/platform/mk66/board.h"
    host = "platform/mk66/update/src/fw_host_mk66.cpp"
    writes = "void f() { f_open(&w, p, FA_CREATE_ALWAYS | FA_WRITE); f_write(&w, d, n, &bw); f_sync(&w); f_unlink(p); f_rename(a, b); f_mkdir(d); f_truncate(&w); f_expand(&w, 1, 1); }\n"
    # (label, files, the number of violations expected, a text every message set must contain or None)
    cases = [
        ("an empty tree is clean", {}, 0, None),
        ("the storage and update slices not landed yet: vacuous", {"kit/app/src/runtime.cpp": "int x;\n"}, 0, None),
        ("the update path's store names every write call", {store: writes}, 0, None),
        ("the glue defines disk_write and disk_ioctl", {glue: "int disk_write(int, const unsigned char*, unsigned, unsigned) { return 0; }\nint disk_ioctl(int, int, void*) { return 0; }\n"}, 0, None),
        ("board.h defines the pin and fw_host drives it", {board: "inline constexpr Pin kBootloaderRequest{};\n", host: "gpio::drive(board::kBootloaderRequest);\n"}, 0, None),
        ("a read-only f_open is not a writer", {glue: "f_open(&f, p, FA_READ); f_read(&f, b, n, &br); f_lseek(&f, 0); f_close(&f);\n", "platform/mk66/storage/src/x.cpp": "f_opendir(&d, p); f_readdir(&d, &i);\n"}, 0, None),
        ("a name in a comment or a string is not a use",
         {"kit/app/src/a.cpp": '// f_write and kBootloaderRequest are named here only\n/* disk_write\n disk_ioctl */ const char* s = "f_unlink FA_WRITE kBootloaderRequest";\nchar c = \'x\'; const char* t = "// f_mkdir";\n'}, 0, None),
        ("lookalike identifiers are not the call", {"kit/app/src/a.cpp": "int my_f_write; int f_writeable; int xFA_WRITE; int kBootloaderRequestPin_; int disk_write_count;\n"}, 0, None),
        ("tests, third_party, build and hidden directories are not scanned",
         {"platform/mk66/tests/t.cpp": writes, "tests/usb/t.cpp": writes, "third_party/fatfs/ff.c": writes, "build-host/x/y.cpp": writes, ".claude/z.cpp": writes,
          "third_party/fatfs/diskio.h": "int disk_write(); int disk_ioctl();\n", "platform/mk66/tests/b.cpp": "kBootloaderRequest\n"}, 0, None),
        ("a source that is not C or C++ is not scanned", {"platform/mk66/tools/x.py": "f_write\n", "docs/x.md": "kBootloaderRequest\n"}, 0, None),
        ("f_write in the kit", {"kit/app/src/save.cpp": "void g() {\n  f_write(&w, d, n, &bw);\n}\n"}, 1, "kit/app/src/save.cpp:2: FatFs write-class call `f_write`"),
        ("f_unlink in an app", {"apps/hello/src/x.cpp": "void g() { f_unlink(\"a\"); }\n"}, 1, "`f_unlink`"),
        ("f_rename in the storage glue (read only)", {glue: "f_rename(a, b);\n"}, 1, "`f_rename`"),
        ("f_mkdir, f_truncate, f_sync, f_expand elsewhere", {"platform/mk66/src/x.cpp": "f_mkdir(d);\nf_truncate(&f);\nf_sync(&f);\nf_expand(&f, 1, 1);\n"}, 4, None),
        ("f_setlabel, f_mkfs, f_chmod, f_utime, f_forward elsewhere", {"platform/mk66/usb/src/x.cpp": "f_setlabel(l);\nf_mkfs(p, 0, 0, 0);\nf_chmod(p, 0, 0);\nf_utime(p, &i);\nf_forward(&f, 0, 0, &n);\n"}, 5, None),
        ("an f_open that writes, elsewhere", {"platform/mk66/storage/src/other.cpp": "f_open(&f, p, FA_READ | FA_WRITE);\n"}, 1, "`FA_WRITE`"),
        ("every write flag named in a header", {"platform/mk66/storage/include/o3sdk/x.h": "FA_WRITE FA_CREATE_NEW FA_CREATE_ALWAYS FA_OPEN_APPEND FA_OPEN_ALWAYS\n"}, 5, None),
        ("a namesake of the store in another directory", {"platform/mk66/save/src/fat_firmware_store.cpp": writes}, 9, None),
        ("the store's header is not the store", {"platform/mk66/update/include/o3sdk/platform/mk66/update/fat_firmware_store.h": "int f_write;\n"}, 1, None),
        ("the store naming the glue's functions", {store: "disk_write(0, 0, 0, 0);\n"}, 1, "`disk_write` outside"),
        ("disk_write in the kit", {"kit/hal/src/x.cpp": "disk_write(0, p, 0, 1);\n"}, 1, "rule 2"),
        ("disk_ioctl in the update path", {host: "disk_ioctl(0, 0, nullptr);\n"}, 1, "`disk_ioctl`"),
        ("the pin in a boot source", {"platform/mk66/src/boot.cpp": "gpio::set(board::kBootloaderRequest);\n"}, 1, "rule 3"),
        ("the pin in the console", {"platform/mk66/console/src/console.cpp": "auto p = kBootloaderRequest;\n"}, 1, "bootloader request pin"),
        ("the pin in the store", {store: "kBootloaderRequest\n"}, 1, "rule 3"),
        ("the pin next to a valid use, on its own line", {host: "gpio::drive(kBootloaderRequest);\n", "platform/mk66/update/src/other.cpp": "\n\nkBootloaderRequest;\n"}, 1, "other.cpp:3"),
        ("a comment opener inside a string does not hide the next line", {"kit/app/src/a.cpp": 'const char* s = "/*";\nf_write(&w, d, n, &bw);\nconst char* t = "*/";\n'}, 1, "a.cpp:2"),
    ]
    failures = 0
    for label, files, expected, text in cases:
        got = violations(files)
        shown = "\n    ".join("%s:%d: %s" % g for g in got)
        if len(got) != expected or (text is not None and text not in shown):
            failures += 1
            print("self-test FAILED: %s: %d violation(s), %d expected%s\n    %s" % (label, len(got), expected, (", wanting %r" % text) if text else "", shown))
    # The same through the file walker: a clean tree, the skipped directories, the relative paths, and a violation found in a nested directory.
    with tempfile.TemporaryDirectory() as tmp:
        def put(rel, text):
            path = os.path.join(tmp, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(text)

        put(store, writes)
        put(glue, "int disk_write() { return 0; }\n")
        put(board, "kBootloaderRequest\n")
        put("third_party/fatfs/ff.c", writes)
        put("build-host/gen/g.cpp", writes)
        put("tests/t.cpp", writes)
        put("platform/mk66/tests/t.cpp", writes)
        if check(tmp, quiet=True) != 0:
            failures += 1
            print("self-test FAILED: the directory walk flagged a clean tree")
        put("kit/usb/src/sneaky.cpp", "void g() { f_write(&w, d, n, &bw); }\n")
        if check(tmp, quiet=True) != 1:
            failures += 1
            print("self-test FAILED: the directory walk missed an f_write in kit/usb/src/sneaky.cpp")
        if sorted(collect(tmp)) != sorted([store, glue, board, "kit/usb/src/sneaky.cpp"]):
            failures += 1
            print("self-test FAILED: the walk collected %r" % sorted(collect(tmp)))
        if check(os.path.join(tmp, "nope"), quiet=True) != 2:
            failures += 1
            print("self-test FAILED: a missing root is not a usage error")
    print("check_writers self-test: %s (%d cases)" % ("ok" if not failures else "%d failure(s)" % failures, len(cases) + 1))
    return 1 if failures else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", "--selftest", dest="self_test", action="store_true")
    sub = parser.add_subparsers(dest="command")
    c = sub.add_parser("check")
    c.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
                   help="o3sdk's source directory (default: the one this file is in)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "check":
        return check(args.root)
    parser.error("a command or --self-test is required")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
