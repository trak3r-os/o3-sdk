#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Who may write the SD card and who may touch the bootloader request pin (stdlib only).

    check_writers.py check [--root <o3sdk dir>]
    check_writers.py --self-test            (also --selftest)

Adapted from track3r-os tools/check_layers.py, rules 6 to 8 (OS-006c, OS-007n; docs/M2C-PLAN.md, section 3): the update path and the app save layer
are the card's two writers. Their paths do not overlap: Firmware/ is the update path's, with its own guard (update/path_guard.h) and nothing in common with the other; the save layer's writer answers through hal/app_guard.h, which is a superset of track3r-os's guard (it admits Projects/, Settings/, instruments/ and AppExport/ as well), and the kit's files service narrows it to Apps/<id>/ and Samples/ with its own ScopedWriter (kit/app/files_work.h, tests/app_files_test.cpp). This check keeps the list of who may name what; the bench's rules
(9 and 10 of track3r-os) are not here, o3sdk has no bench. It reads the first-party C and C++ sources under the root (comments and string literals
removed, so a name in a comment is not a use) and fails when:

  1. FatFs's write-class calls (f_write f_sync f_unlink f_rename f_truncate f_mkdir f_expand f_setlabel f_mkfs f_chmod f_utime
     f_forward) or the open flags that write (FA_WRITE FA_CREATE_NEW FA_CREATE_ALWAYS FA_OPEN_APPEND FA_OPEN_ALWAYS: an `f_open`
     that writes names one of them) are named in any source but the two writers of the card, each of which applies its own path guard before
     every one of them: platform/mk66/update/src/fat_firmware_store.cpp (the update path, `fw`, under Firmware/) and
     platform/mk66/save/src/fat_storage_writer.cpp (the app save layer, through hal/app_guard.h; the files service uses it under Apps/<id>/ and Samples/ only);
  2. `disk_write` or `disk_ioctl`, the FatFs glue to the block device, are named in any source but
     platform/mk66/storage/src/fat_storage.cpp, which defines them;
  3. `kBootloaderRequest`, the bootloader request pin (PTA1), is named in any source but platform/mk66/include/o3sdk/platform/mk66/
     board.h, which defines it, and platform/mk66/update/src/fw_host_mk66.cpp, the one place that drives it (the `fw update` hand-over);
  4. a source includes <o3sdk/hal/storage_write.h> (the card's write contract), <o3sdk/hal/save_guard.h> or <o3sdk/hal/app_guard.h> (the guards)
     and is not one of SAVE_WRITE_USERS: the headers themselves, the FatFs writer, the save job (app/atomic_save), the kit's files service
     (app/files), the host cards (platform/host: DirCard, FatFsStorage), the image's main.cpp and the screenshot service. Nothing else can
     ask for a change of the card: not a screen, not an app, not a unit. (The match is by the include's file name, comments removed.)

Not scanned: tests (any path with a `tests` directory), third_party (FatFs itself), build directories, hidden directories. The
files named above need not exist (the files service and the screenshot service are later slices): the check passes while a file is absent and
fails the day a writer, or an include of the write contract, appears anywhere else. The root is o3sdk's own tree (the default); an app project's sources may be checked with --root.
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
WRITE_API_USERS = {"platform/mk66/update/src/fat_firmware_store.cpp", "platform/mk66/save/src/fat_storage_writer.cpp"}
DISK_WRITE_API = re.compile(r"\b(?:disk_write|disk_ioctl)\b")
DISK_WRITE_USERS = {"platform/mk66/storage/src/fat_storage.cpp"}
BOOTLOADER_REQUEST = re.compile(r"\bkBootloaderRequest\b")
BOOTLOADER_REQUEST_USERS = {"platform/mk66/include/o3sdk/platform/mk66/board.h", "platform/mk66/update/src/fw_host_mk66.cpp"}

# Rule 4: who may include the write contract and the guards (by the include's file name).
SAVE_WRITE_HEADERS = {"storage_write.h", "save_guard.h", "app_guard.h"}
SAVE_WRITE_USERS = {
    "kit/hal/include/o3sdk/hal/storage_write.h", "kit/hal/include/o3sdk/hal/save_guard.h", "kit/hal/include/o3sdk/hal/app_guard.h",
    "platform/mk66/save/include/o3sdk/platform/mk66/save/fat_storage_writer.h", "platform/mk66/save/src/fat_storage_writer.cpp",
    "kit/app/include/o3sdk/app/atomic_save.h", "kit/app/src/atomic_save.cpp",
    "kit/app/include/o3sdk/app/files.h", "kit/app/src/files.cpp",  # the kit's files service (slice FA)
    "platform/host/include/o3sdk/platform/host/dir_card.h", "platform/host/src/dir_card.cpp",
    "platform/host/include/o3sdk/platform/host/host_card.h", "platform/host/include/o3sdk/platform/host/protect_card.h",
    "platform/host/fat/include/o3sdk/platform/host/fat_fs_storage.h", "platform/host/fat/src/fat_fs_storage.cpp",
    "platform/mk66/usb/include/o3sdk/platform/mk66/usb/screenshot_service.h", "platform/mk66/usb/src/screenshot_service.cpp",  # a later slice
    "platform/mk66/src/main.cpp",
}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]', re.MULTILINE)

RULES = (
    (WRITE_API, WRITE_API_USERS,
     "FatFs write-class call `%s` outside the two writers, platform/mk66/update/src/fat_firmware_store.cpp and platform/mk66/save/src/fat_storage_writer.cpp (rule 1)"),
    (DISK_WRITE_API, DISK_WRITE_USERS, "`%s` outside platform/mk66/storage/src/fat_storage.cpp (rule 2)"),
    (BOOTLOADER_REQUEST, BOOTLOADER_REQUEST_USERS,
     "`%s`, the bootloader request pin, outside board.h and platform/mk66/update/src/fw_host_mk66.cpp (rule 3)"),
)

# Comments are blanked; string and character literals are kept while scanning so that a `//` inside one is not a comment, then blanked too.
COMMENT_OR_LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)


# Comments only: string and character literals are walked over unchanged (an #include names its file in one).
COMMENT_ONLY = re.compile(r'("(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\')|//[^\n]*|/\*.*?\*/', re.DOTALL)


def comments_blanked(text):
    """The text with comments replaced by spaces (newlines kept), literals untouched."""
    return COMMENT_ONLY.sub(lambda m: m.group(0) if m.group(1) is not None else re.sub(r"[^\n]", " ", m.group(0)), text)


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
        if rel not in SAVE_WRITE_USERS:
            with_includes = comments_blanked(text)
            for m in INCLUDE.finditer(with_includes):
                name = m.group(1).rsplit("/", 1)[-1]
                if name in SAVE_WRITE_HEADERS:
                    found.append((rel, with_includes.count("\n", 0, m.start()) + 1,
                                  "%s (the card's write contract and guards) is for the save layer only (rule 4)" % name))
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
    saver = "platform/mk66/save/src/fat_storage_writer.cpp"
    contracts = "#include <o3sdk/hal/storage_write.h>\n#include <o3sdk/hal/save_guard.h>\n#include \"o3sdk/hal/app_guard.h\"\n"
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
        ("a namesake of the store in another directory", {"platform/mk66/update/src/fat_storage_writer.cpp": writes}, 9, None),
        ("the app save layer's writer names every write call (rule 1's second user)", {saver: writes}, 0, None),
        ("the save writer's header and a namesake of the writer are not the writer", {"platform/mk66/save/include/o3sdk/platform/mk66/save/fat_storage_writer.h": "int f_write;\n", "platform/mk66/save/src/fat_storage_writer2.cpp": "f_unlink(p);\n"}, 2, None),
        ("each writer names its own calls, a third one is refused", {store: writes, saver: writes, "platform/mk66/save/src/bench.cpp": "f_sync(&w);\n"}, 1, "save/src/bench.cpp:1: FatFs write-class call `f_sync` outside the two writers"),
        ("the store's header is not the store", {"platform/mk66/update/include/o3sdk/platform/mk66/update/fat_firmware_store.h": "int f_write;\n"}, 1, None),
        ("the store naming the glue's functions", {store: "disk_write(0, 0, 0, 0);\n"}, 1, "`disk_write` outside"),
        ("disk_write in the kit", {"kit/hal/src/x.cpp": "disk_write(0, p, 0, 1);\n"}, 1, "rule 2"),
        ("disk_ioctl in the update path", {host: "disk_ioctl(0, 0, nullptr);\n"}, 1, "`disk_ioctl`"),
        ("the pin in a boot source", {"platform/mk66/src/boot.cpp": "gpio::set(board::kBootloaderRequest);\n"}, 1, "rule 3"),
        ("the pin in the console", {"platform/mk66/console/src/console.cpp": "auto p = kBootloaderRequest;\n"}, 1, "bootloader request pin"),
        ("the pin in the store", {store: "kBootloaderRequest\n"}, 1, "rule 3"),
        ("the pin next to a valid use, on its own line", {host: "gpio::drive(kBootloaderRequest);\n", "platform/mk66/update/src/other.cpp": "\n\nkBootloaderRequest;\n"}, 1, "other.cpp:3"),
        # rule 4: who may include the write contract and the guards
        ("the users of the write contract and the guards include all of them", {
            "kit/hal/include/o3sdk/hal/storage_write.h": contracts, "kit/hal/include/o3sdk/hal/save_guard.h": contracts, "kit/hal/include/o3sdk/hal/app_guard.h": contracts,
            "platform/mk66/save/include/o3sdk/platform/mk66/save/fat_storage_writer.h": contracts, saver: contracts,
            "kit/app/include/o3sdk/app/atomic_save.h": contracts, "kit/app/src/atomic_save.cpp": contracts,
            "kit/app/include/o3sdk/app/files.h": contracts, "kit/app/src/files.cpp": contracts,
            "platform/host/include/o3sdk/platform/host/dir_card.h": contracts, "platform/host/src/dir_card.cpp": contracts,
            "platform/host/include/o3sdk/platform/host/host_card.h": contracts,
            "platform/host/fat/include/o3sdk/platform/host/fat_fs_storage.h": contracts, "platform/host/fat/src/fat_fs_storage.cpp": contracts,
            "platform/mk66/usb/src/screenshot_service.cpp": contracts, "platform/mk66/usb/include/o3sdk/platform/mk66/usb/screenshot_service.h": contracts,
            "platform/mk66/src/main.cpp": contracts}, 0, None),
        ("the three headers in a screen", {"kit/app/src/screen.cpp": contracts}, 3, "kit/app/src/screen.cpp:1: storage_write.h (the card's write contract and guards) is for the save layer only (rule 4)"),
        ("app_guard.h in an app", {"apps/hello/hello.cpp": "#include <o3sdk/hal/app_guard.h>\n"}, 1, "app_guard.h"),
        ("save_guard.h in the update path", {"platform/mk66/update/src/fw_service.cpp": "int a;\n#include <o3sdk/hal/save_guard.h>\n"}, 1, "fw_service.cpp:2: save_guard.h"),
        ("a quoted or a relative include is the same include", {"kit/ui/src/x.cpp": '  # include "../../hal/include/o3sdk/hal/storage_write.h"\n'}, 1, "storage_write.h"),
        ("the sim and the console are not users", {"sim/src/x.cpp": contracts, "platform/mk66/console/src/appimg.cpp": "#include <o3sdk/hal/storage_write.h>\n"}, 4, None),
        ("an include in a comment is not an include", {"kit/ui/src/x.cpp": '// #include <o3sdk/hal/save_guard.h>\n/* #include <o3sdk/hal/app_guard.h>\n#include <o3sdk/hal/storage_write.h> */\nint x;\n'}, 0, None),
        ("a name in a string, or a header of another name, is not the include", {"kit/ui/src/x.cpp": 'const char* s = "o3sdk/hal/save_guard.h";\n#include <o3sdk/hal/my_save_guard.h>\n#include <o3sdk/hal/save_guard.hpp>\n#include <o3sdk/hal/storage.h>\n'}, 0, None),
        ("tests are not scanned for the contract", {"tests/support/mem_card.h": contracts, "platform/mk66/save/tests/t.cpp": contracts}, 0, None),
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
        put(saver, contracts + writes)
        put("kit/app/src/atomic_save.cpp", contracts)
        if check(tmp, quiet=True) != 0:
            failures += 1
            print("self-test FAILED: the directory walk flagged a clean tree")
        put("kit/usb/src/sneaky.cpp", "void g() { f_write(&w, d, n, &bw); }\n")
        if check(tmp, quiet=True) != 1:
            failures += 1
            print("self-test FAILED: the directory walk missed an f_write in kit/usb/src/sneaky.cpp")
        os.remove(os.path.join(tmp, "kit", "usb", "src", "sneaky.cpp"))
        put("kit/ui/src/sneaky_save.cpp", "#include <o3sdk/hal/app_guard.h>\n")
        if check(tmp, quiet=True) != 1:
            failures += 1
            print("self-test FAILED: the directory walk missed the app_guard.h include in kit/ui/src/sneaky_save.cpp")
        put("kit/usb/src/sneaky.cpp", "void g() { f_write(&w, d, n, &bw); }\n")
        if sorted(collect(tmp)) != sorted([store, saver, glue, board, "kit/app/src/atomic_save.cpp", "kit/ui/src/sneaky_save.cpp", "kit/usb/src/sneaky.cpp"]):
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
