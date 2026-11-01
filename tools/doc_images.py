#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""doc_images.py: the pictures of o3-sdk's documents (docs/TOUR.md, README.md), made from the simulator's scenarios.

    python3 -I tools/doc_images.py --build build-host     # runs the scenarios, writes docs/img/*.png
    python3 -I tools/doc_images.py --check                # the ctest doc_images: the documents and the table below agree

--build takes a host build directory of o3-sdk (the top-level project, with its tests: the default). HELLO's simulator is <build>/apps/hello/hello_sim;
the template app's is <build>/templates/work-host/build-host/app/demo_app_sim, which `ctest --test-dir <build> -R template_host_build` makes (the
instance of templates/app that templates/check_template.py builds, named DEMO-APP). --hello-sim and --template-sim name either one elsewhere.

IMAGES is the one table: the picture's name in docs/img, the scenario that takes it (a path from the repository's root), the snap's name in that
scenario, and the crop ("screen" keeps the display, the top 800 x 480; "full" keeps the pad grid the simulator draws under it). RUNS says how each
scenario runs: the simulator and the options it needs, as tests/CMakeLists.txt runs it for the pinned digests ({card} is a fresh empty directory).
Each scenario runs once, into a temporary directory. A template scenario is run from a copy with @APP_ID@ replaced by the instance's id.

The pictures are written again without loss and small: an 8-bit palette when the picture has 256 colours or fewer, else RGB with the PNG filter
that packs best. Nothing is compared byte for byte: --check asks that every picture a document shows (README.md and docs/*.md, a path with img/)
is in docs/img and named in the table, that every row is shown by a document, that docs/img holds nothing else, that every row names a scenario
that exists, runs in RUNS and really takes that snap, and that the pictures together stay under MAX_TOTAL. Python 3 standard library only.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "docs", "img")
DOCS = [os.path.join(ROOT, "README.md")]  # and every docs/*.md, found at check time

SCREEN_W = 800
SCREEN_H = 480
MAX_TOTAL = 2 * 1024 * 1024  # the pictures together stay under 2 MB
TEMPLATE_ID = "demo_app"     # the id templates/check_template.py instantiates the template with

HELLO = "apps/hello/scenarios/"
TEMPLATE = "templates/app/app/scenarios/"

# How each scenario runs: (simulator, options). The options mirror tests/CMakeLists.txt (digest_hello) and templates/check_template.py.
RUNS = {
    HELLO + "audio_in.txt": ("hello", []),
    HELLO + "boot.txt": ("hello", []),
    HELLO + "config.txt": ("hello", ["--firmware-dir", HELLO + "firmware"]),
    HELLO + "config_pages.txt": ("hello", ["--card", "{card}", "--audio-in"]),
    HELLO + "files.txt": ("hello", ["--card", "{card}"]),
    HELLO + "help.txt": ("hello", []),
    HELLO + "midi.txt": ("hello", []),
    HELLO + "picker.txt": ("hello", ["--card", "{card}"]),
    HELLO + "record.txt": ("hello", ["--card", "{card}"]),
    HELLO + "tour.txt": ("hello", []),
    HELLO + "usb.txt": ("hello", ["--card", "{card}"]),
    TEMPLATE + "boot.txt": ("template", []),
}

# (picture, scenario, snap, crop), in the order docs/TOUR.md shows them.
IMAGES = [
    ("boot-logo", HELLO + "boot.txt", "01_boot", "screen"),
    ("template-play", TEMPLATE + "boot.txt", "02_chord", "full"),
    ("frame", HELLO + "tour.txt", "04_reso_held", "screen"),
    ("help-buttons", HELLO + "help.txt", "01_buttons", "screen"),
    ("help-pads", HELLO + "help.txt", "02_pads", "screen"),
    ("config-firmware", HELLO + "config.txt", "01_firmware_list", "screen"),
    ("config-install", HELLO + "config.txt", "05_ask_install", "screen"),
    ("config-audio", HELLO + "audio_in.txt", "05_radio_ready", "screen"),
    ("config-midi", HELLO + "midi.txt", "02_midi_section", "screen"),
    ("config-card", HELLO + "usb.txt", "07_disk_out", "screen"),
    ("config-display", HELLO + "config_pages.txt", "02_display", "screen"),
    ("config-info", HELLO + "config_pages.txt", "05_info", "screen"),
    ("config-app-page", HELLO + "config_pages.txt", "06_hello_page", "screen"),
    ("name-keyboard", HELLO + "picker.txt", "01_typed", "screen"),
    ("file-picker", HELLO + "picker.txt", "04_picker", "screen"),
    ("files-saved", HELLO + "files.txt", "01_saved", "screen"),
    ("record", HELLO + "record.txt", "01_recording", "screen"),
    ("midi-follow", HELLO + "midi.txt", "05_master_playing", "screen"),
    ("usb-profile", HELLO + "usb.txt", "03_profile_p10", "screen"),
    ("theme-amiga", HELLO + "help.txt", "00_play", "screen"),
    ("theme-black", HELLO + "tour.txt", "08_theme", "screen"),
    ("theme-white", HELLO + "help.txt", "12_closed_white", "screen"),
]

CROPS = ("screen", "full")
SIMS = ("hello", "template")


# ---- The scenarios --------------------------------------------------------------------------------------------------------------------------


def snaps_of(path):
    """The names after `snap` and `boot` (the boot screen's picture) on the lines of a scenario, as sim/src/scenario.cpp reads them."""
    names = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            words = line.split("#", 1)[0].split()
            if len(words) >= 2 and words[0] in ("snap", "boot"):
                names.append(words[1])
    return names


# ---- PNG in and out -------------------------------------------------------------------------------------------------------------------------


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def read_png(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGB or RGBA PNG, as the simulator writes them (any filter, no interlace)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(path + ": not a PNG")
    pos = 8
    idat = []
    width = height = depth = colour = interlace = None
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if depth != 8 or colour not in (2, 6) or interlace != 0:
        raise ValueError("%s: an 8-bit RGB or RGBA PNG without interlace is expected (depth %s, colour type %s)" % (path, depth, colour))
    raw = zlib.decompress(b"".join(idat))
    bpp = 3 if colour == 2 else 4
    stride = width * bpp
    rows = []
    prev = bytearray(stride)
    i = 0
    for _ in range(height):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                c = prev[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + _paeth(a, prev[x], c)) & 255
        elif f != 0:
            raise ValueError("%s: unknown PNG filter %d" % (path, f))
        rows.append(line)
        prev = line
    if bpp == 4:  # the simulator's alpha is always opaque: keep the colour
        rgb = []
        for line in rows:
            out = bytearray(width * 3)
            out[0::3] = line[0::4]
            out[1::3] = line[1::4]
            out[2::3] = line[2::4]
            rgb.append(out)
        rows = rgb
    return width, height, [bytes(r) for r in rows]


def _chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def _filtered(rows, bpp, f):
    """The rows with one PNG filter for the whole picture (0 none, 1 sub, 2 up), compressed."""
    out = bytearray()
    prev = bytes(len(rows[0]))
    for r in rows:
        if f == 0:
            out += b"\x00" + r
        elif f == 1:
            out += b"\x01" + r[:bpp] + bytes((r[i] - r[i - bpp]) & 255 for i in range(bpp, len(r)))
        else:
            out += b"\x02" + bytes((a - b) & 255 for a, b in zip(r, prev))
        prev = r
    return zlib.compress(bytes(out), 9)


def write_png(path, width, rows):
    """Writes the RGB rows without loss: an 8-bit palette when there are 256 colours or fewer, else RGB with the filter that packs best."""
    colours = set()
    for r in rows:
        for x in range(0, len(r), 3):
            colours.add(r[x:x + 3])
        if len(colours) > 256:
            break
    if len(colours) <= 256:
        palette = sorted(colours)
        index = {c: i for i, c in enumerate(palette)}
        idx_rows = [bytes(index[r[x:x + 3]] for x in range(0, len(r), 3)) for r in rows]
        packed = min((_filtered(idx_rows, 1, f) for f in (0, 2)), key=len)
        header = struct.pack(">IIBBBBB", width, len(rows), 8, 3, 0, 0, 0)
        body = _chunk(b"IHDR", header) + _chunk(b"PLTE", b"".join(palette)) + _chunk(b"IDAT", packed)
    else:
        packed = min((_filtered(rows, 3, f) for f in (0, 1, 2)), key=len)
        header = struct.pack(">IIBBBBB", width, len(rows), 8, 2, 0, 0, 0)
        body = _chunk(b"IHDR", header) + _chunk(b"IDAT", packed)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + body + _chunk(b"IEND", b""))


# ---- --check --------------------------------------------------------------------------------------------------------------------------------


def documents():
    docs = list(DOCS)
    docs_dir = os.path.join(ROOT, "docs")
    for name in sorted(os.listdir(docs_dir)):
        if name.endswith(".md"):
            docs.append(os.path.join(docs_dir, name))
    return docs


def shown_pictures():
    """{picture: [documents that show it]}: every `img/<name>.png` in README.md and docs/*.md (a Markdown image or an <img> tag)."""
    shown = {}
    for path in documents():
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for name in re.findall(r"(?<![A-Za-z0-9_/.-])(?:docs/)?img/([A-Za-z0-9_.-]+)\.png", text):
            shown.setdefault(name, []).append(os.path.relpath(path, ROOT))
    return shown


def check():
    problems = []
    names = [row[0] for row in IMAGES]
    for name in sorted({n for n in names if names.count(n) > 1}):
        problems.append("IMAGES names %s more than once" % name)
    snaps = {}
    for name, scenario, snap, crop in IMAGES:
        path = os.path.join(ROOT, scenario)
        if not os.path.isfile(path):
            problems.append("%s: the scenario %s does not exist" % (name, scenario))
            continue
        if scenario not in RUNS:
            problems.append("%s: RUNS does not say how %s runs" % (name, scenario))
        if scenario not in snaps:
            snaps[scenario] = snaps_of(path)
        if snap not in snaps[scenario]:
            problems.append("%s: %s takes no snap %s" % (name, scenario, snap))
        if crop not in CROPS:
            problems.append("%s: the crop %r is not one of %s" % (name, crop, ", ".join(CROPS)))
    for scenario, (sim, _) in sorted(RUNS.items()):
        if sim not in SIMS:
            problems.append("RUNS: %s runs on %r, which is not one of %s" % (scenario, sim, ", ".join(SIMS)))
        if not os.path.isfile(os.path.join(ROOT, scenario)):
            problems.append("RUNS names %s, which does not exist" % scenario)
    shown = shown_pictures()
    for name in sorted(shown):
        where = ", ".join(sorted(set(shown[name])))
        if not os.path.isfile(os.path.join(IMG, name + ".png")):
            problems.append("%s shows img/%s.png, and docs/img has no such file" % (where, name))
        if name not in names:
            problems.append("%s shows img/%s.png, and IMAGES has no row for it (it could not be made again)" % (where, name))
    for name in names:
        if name not in shown:
            problems.append("IMAGES makes %s, and no document shows it" % name)
    present = sorted(f[:-4] for f in os.listdir(IMG) if f.endswith(".png")) if os.path.isdir(IMG) else []
    for name in present:
        if name not in names:
            problems.append("docs/img/%s.png is not made by IMAGES (remove it, or add its row)" % name)
    total = sum(os.path.getsize(os.path.join(IMG, n + ".png")) for n in present)
    if total > MAX_TOTAL:
        problems.append("the pictures take %d bytes, over the %d allowed" % (total, MAX_TOTAL))
    for p in problems:
        print("doc_images: " + p, file=sys.stderr)
    print("doc_images: %d pictures in the table, %d shown by the documents, %d bytes in docs/img" % (len(IMAGES), len(shown), total))
    return 0 if not problems else 1


# ---- --build --------------------------------------------------------------------------------------------------------------------------------


def make(sims):
    for key in SIMS:
        needed = any(RUNS[row[1]][0] == key for row in IMAGES)
        if needed and not (sims.get(key) and os.access(sims[key], os.X_OK)):
            hint = " (ctest --test-dir <build> -R template_host_build makes it)" if key == "template" else ""
            print("doc_images: no %s simulator at %s%s" % (key, sims.get(key), hint), file=sys.stderr)
            return 1
    os.makedirs(IMG, exist_ok=True)
    order = []
    for row in IMAGES:
        if row[1] not in order:
            order.append(row[1])
    total = 0
    with tempfile.TemporaryDirectory(prefix="o3sdk_doc_images_") as tmp:
        for n, scenario in enumerate(order):
            sim, options = RUNS[scenario]
            base = os.path.join(tmp, "%02d" % n)
            out = os.path.join(base, "out")
            card = os.path.join(base, "card")
            os.makedirs(out)
            os.makedirs(card)
            script = os.path.join(ROOT, scenario)
            if sim == "template":  # the template's files are instantiated by tools/new_app.py: run a copy with the instance's id
                with open(script, encoding="utf-8") as f:
                    text = f.read().replace("@APP_ID@", TEMPLATE_ID)
                script = os.path.join(base, os.path.basename(scenario))
                with open(script, "w", encoding="utf-8") as f:
                    f.write(text)
            args = [os.path.join(ROOT, a) if a.startswith("apps/") else a.format(card=card) for a in options]
            run = subprocess.run([sims[sim], "--scenario", script, "--out", out] + args, cwd=ROOT,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600, check=False)
            if run.returncode != 0:
                sys.stderr.write(run.stdout.decode("utf-8", "replace"))
                print("doc_images: %s failed (exit %d)" % (scenario, run.returncode), file=sys.stderr)
                return 1
            for name, row_scenario, snap, crop in IMAGES:
                if row_scenario != scenario:
                    continue
                width, height, rows = read_png(os.path.join(out, snap + ".png"))
                if crop == "screen":
                    if width != SCREEN_W or height < SCREEN_H:
                        print("doc_images: %s/%s is %d x %d, smaller than the screen" % (scenario, snap, width, height), file=sys.stderr)
                        return 1
                    rows = rows[:SCREEN_H]
                dest = os.path.join(IMG, name + ".png")
                write_png(dest, width, rows)
                size = os.path.getsize(dest)
                total += size
                print("%-16s %-48s %-18s %-6s %7d bytes" % (name, scenario, snap, crop, size))
    print("doc_images: %d pictures, %d bytes" % (len(IMAGES), total))
    return 0


def main():
    parser = argparse.ArgumentParser(description="The pictures of o3-sdk's documents, from the simulator's scenarios.")
    job = parser.add_mutually_exclusive_group(required=True)
    job.add_argument("--build", help="a host build directory of o3-sdk: run the scenarios and write docs/img")
    job.add_argument("--check", action="store_true", help="check that the documents, docs/img and the table agree (the ctest)")
    parser.add_argument("--hello-sim", help="HELLO's simulator (default <build>/apps/hello/hello_sim)")
    parser.add_argument("--template-sim", help="the template app's simulator (default <build>/templates/work-host/build-host/app/demo_app_sim)")
    args = parser.parse_args()
    if args.check:
        return check()
    build = os.path.abspath(args.build)
    sims = {
        "hello": os.path.abspath(args.hello_sim) if args.hello_sim else os.path.join(build, "apps", "hello", "hello_sim"),
        "template": os.path.abspath(args.template_sim) if args.template_sim
        else os.path.join(build, "templates", "work-host", "build-host", "app", TEMPLATE_ID + "_sim"),
    }
    return make(sims)


if __name__ == "__main__":
    sys.exit(main())
