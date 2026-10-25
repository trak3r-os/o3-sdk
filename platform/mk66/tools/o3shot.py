#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3shot: the screen of the unit as a PNG, and a screen compare with tolerances (OS-HIL; stdlib only: zlib + struct).

The device side is `diag shot` (usb/shot_stream.h): framed base64 lines with a CRC each. This module

  * decodes those lines (`parse_shot`): every line's CRC and offset checked, the whole image's CRC and the line count checked against the END line, the missing
    ranges reported; RGB565 and ARGB8 pictures to RGB888 (`shot_rgb`);
  * writes and reads PNG files (`write_png`, `read_png`: 8-bit gray, gray+alpha, RGB, RGBA, not interlaced, all five filters), so the simulator's goldens
    (o3sim --scenario ... --out DIR writes DIR/<snap>.png, 800 x 480 RGBA) can be read;
  * compares two pictures (`compare_images`) the way a screen of a real FT812 has to be compared with the simulator's: a pixel matches when every channel is within
    `tol` (RGB565 loses up to 8 levels in red and blue and 4 in green, and the FT812 dithers), OR (the anti-aliased edge of a line or a glyph falls on a neighbour pixel)
    each of the two has a like pixel among the other's neighbours within `aa` pixels; the picture passes when no more than `max_bad` of the compared pixels are left. Regions pick what is compared
    (a picture with a loaded project differs from the demo's in its data, not in its chrome), ignores take parts out.

    o3shot.py decode CAPTURE.txt OUT.png         the text a terminal saved of `shot` -> PNG
    o3shot.py compare A.png B.png [--tol 24] [--aa 1] [--max-bad 0.005] [--region x,y,w,h]... [--ignore x,y,w,h]... [--diff OUT.png]
    o3shot.py --self-test

`compare` exits 0 when the pictures agree, 1 when they differ more than the allowed share, 2 on a usage or file error.
"""
import argparse
import base64
import binascii
import os
import re
import struct
import sys
import tempfile
import zlib

WIDTH = 800
HEIGHT = 480


class ShotError(Exception):
    pass


# ---- the frames ------------------------------------------------------------------------------------------------------------------

class Shot:
    def __init__(self, fmt, width, height, data, moved, lines, bad_lines, missing):
        self.fmt = fmt
        self.width = width
        self.height = height
        self.data = data
        self.moved = moved
        self.lines = lines
        self.bad_lines = bad_lines
        self.missing = missing  # [(first, last_exclusive)] byte ranges no good line covered

    @property
    def complete(self):
        return not self.missing and not self.bad_lines


_BEGIN = re.compile(r"^SHOT BEGIN fmt=(565|8888) w=(\d+) h=(\d+) bytes=(\d+) line=(\d+)$")
_DATA = re.compile(r"^SHOT D ([0-9A-F]{6}) ([A-Za-z0-9+/=]+) ([0-9A-F]{8})$")
_END = re.compile(r"^SHOT END lines=(\d+) crc=([0-9A-F]{8}) moved=([01])$")
_ERR = re.compile(r"^SHOT ERR (.*)$")


def parse_shot(text):
    """The text of one `shot` (any other lines, the echo and the prompt among them, are ignored) as a Shot. ShotError for an ERR line, a missing BEGIN or END, a
    header that makes no sense, or an END that does not match the data; lines whose CRC or length is wrong are counted in bad_lines and leave a hole in `missing`."""
    header = None
    pieces = {}
    bad = 0
    end = None
    for raw in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        line = raw.strip()
        if not line.startswith("SHOT "):
            continue
        m = _ERR.match(line)
        if m:
            raise ShotError("the unit answered: %s" % m.group(1))
        m = _BEGIN.match(line)
        if m:
            header = (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
            pieces = {}
            bad = 0
            end = None
            continue
        if header is None:
            continue
        m = _DATA.match(line)
        if m:
            offset = int(m.group(1), 16)
            try:
                raw_bytes = base64.b64decode(m.group(2), validate=True)
            except (binascii.Error, ValueError):
                bad += 1
                continue
            if zlib.crc32(raw_bytes) & 0xFFFFFFFF != int(m.group(3), 16) or not raw_bytes or offset + len(raw_bytes) > header[3]:
                bad += 1
                continue
            pieces[offset] = raw_bytes
            continue
        m = _END.match(line)
        if m:
            end = (int(m.group(1)), int(m.group(2), 16), m.group(3) == "1")
            continue
        bad += 1  # a SHOT line that is none of the four: damaged in transit
    if header is None:
        raise ShotError("no SHOT BEGIN line in the text")
    if end is None:
        raise ShotError("no SHOT END line: the transfer was cut")
    fmt, width, height, total, _line = header
    bpp = 2 if fmt == "565" else 4
    if total != width * height * bpp or width <= 0 or height <= 0:
        raise ShotError("the header does not describe a picture: %s %dx%d %d bytes" % (fmt, width, height, total))
    data = bytearray(total)
    covered = []
    for offset in sorted(pieces):
        piece = pieces[offset]
        if covered and offset < covered[-1][1]:
            bad += 1  # overlaps what came before: a line repeated or an offset damaged
            continue
        data[offset:offset + len(piece)] = piece
        if covered and covered[-1][1] == offset:
            covered[-1] = (covered[-1][0], offset + len(piece))
        else:
            covered.append((offset, offset + len(piece)))
    missing = []
    at = 0
    for first, last in covered:
        if first > at:
            missing.append((at, first))
        at = last
    if at < total:
        missing.append((at, total))
    shot = Shot(fmt, width, height, bytes(data), end[2], end[0], bad, missing)
    if shot.complete:
        if end[0] != len(pieces):
            raise ShotError("the END line counts %d lines, %d arrived" % (end[0], len(pieces)))
        if zlib.crc32(shot.data) & 0xFFFFFFFF != end[1]:
            raise ShotError("the picture's CRC does not match the END line")
    return shot


def shot_rgb(shot):
    """The picture as RGB888 bytes, row by row."""
    n = shot.width * shot.height
    if shot.fmt == "8888":
        out = bytearray(n * 3)
        out[0::3] = shot.data[2::4]  # the chip's ARGB8 is B, G, R, A in memory
        out[1::3] = shot.data[1::4]
        out[2::3] = shot.data[0::4]
        return bytes(out)
    lut = _rgb565_table()
    return b"".join(lut[v] for v in struct.unpack("<%dH" % n, shot.data))


_LUT = []


def _rgb565_table():
    if not _LUT:
        for v in range(65536):
            r = (v >> 11) & 31
            g = (v >> 5) & 63
            b = v & 31
            _LUT.append(bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))))
    return _LUT


# ---- PNG --------------------------------------------------------------------------------------------------------------------------

def _chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path, width, height, rgb):
    """8-bit RGB (3 bytes a pixel, rows top first)."""
    if len(rgb) != width * height * 3:
        raise ValueError("%d bytes for a %dx%d RGB picture" % (len(rgb), width, height))
    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(_chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(_chunk(b"IEND", b""))


def read_png(path):
    """(width, height, rgb bytes): 8-bit gray, gray+alpha, RGB or RGBA, not interlaced; alpha is dropped."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ShotError("%s is not a PNG" % path)
    at = 8
    idat = []
    header = None
    while at + 12 <= len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != struct.unpack(">I", data[at + 8 + length:at + 12 + length])[0]:
            raise ShotError("%s: a %s chunk has a bad CRC" % (path, kind.decode("latin-1")))
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
        at += 12 + length
    if header is None:
        raise ShotError("%s has no IHDR" % path)
    width, height, depth, color, _comp, _filt, interlace = header
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if depth != 8 or channels is None or interlace != 0:
        raise ShotError("%s: only 8-bit gray, gray+alpha, RGB and RGBA without interlacing are read (depth %d, colour type %d, interlace %d)" % (path, depth, color, interlace))
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        raise ShotError("%s: the image data is %d bytes, %d expected" % (path, len(raw), (stride + 1) * height))
    out = bytearray(stride * height)
    prev = bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 255
        elif kind == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif kind == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        elif kind != 0:
            raise ShotError("%s: unknown PNG filter %d on row %d" % (path, kind, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if channels == 3:
        return width, height, bytes(out)
    rgb = bytearray(width * height * 3)
    if channels == 4:
        rgb[0::3], rgb[1::3], rgb[2::3] = out[0::4], out[1::4], out[2::4]
    elif channels == 1:
        rgb[0::3] = rgb[1::3] = rgb[2::3] = out
    else:
        rgb[0::3] = rgb[1::3] = rgb[2::3] = out[0::2]
    return width, height, bytes(rgb)


# ---- compare ----------------------------------------------------------------------------------------------------------------------

class Comparison:
    def __init__(self, compared, bad, bbox, diff):
        self.compared = compared
        self.bad = bad
        self.bbox = bbox  # (x0, y0, x1, y1) of the pixels that differ, or None
        self.diff = diff  # RGB bytes: the picture dimmed, the pixels that differ in red

    @property
    def fraction(self):
        return self.bad / self.compared if self.compared else 0.0


def parse_region(text):
    m = re.fullmatch(r"(\d+),(\d+),(\d+),(\d+)", text)
    if not m:
        raise ValueError("a region is x,y,w,h: %r" % text)
    x, y, w, h = (int(g) for g in m.groups())
    return (x, y, w, h)


def compare_images(a, b, width, height, tol=24, aa=1, regions=None, ignores=None, want_diff=False):
    """Compares two RGB888 pictures of the same size (see the module text). `regions`: [(x, y, w, h)] to compare, default the whole picture; `ignores` likewise."""
    if len(a) != width * height * 3 or len(b) != width * height * 3:
        raise ValueError("the pictures are not %dx%d RGB" % (width, height))
    rects = [(0, 0, width, height)] if not regions else [(max(0, x), max(0, y), min(w, width - x), min(h, height - y)) for x, y, w, h in regions]
    skip = [(max(0, x), max(0, y), x + w, y + h) for x, y, w, h in (ignores or [])]
    stride = width * 3
    mask = bytearray(width * height)  # 1 = compared
    for x, y, w, h in rects:
        for yy in range(y, y + h):
            mask[yy * width + x:yy * width + x + w] = b"\x01" * w
    for x0, y0, x1, y1 in skip:
        for yy in range(max(0, y0), min(height, y1)):
            mask[yy * width + max(0, x0):yy * width + min(width, x1)] = b"\x00" * (min(width, x1) - max(0, x0))
    compared = sum(mask)
    bad = 0
    x_min, y_min, x_max, y_max = width, height, -1, -1
    diff = None
    if want_diff:
        diff = bytearray(len(a))
        for i in range(len(a)):
            diff[i] = (a[i] >> 2) + 64  # the first picture, dimmed
    for y in range(height):
        row_a = a[y * stride:(y + 1) * stride]
        row_b = b[y * stride:(y + 1) * stride]
        if row_a == row_b:
            continue
        for x in range(width):
            if not mask[y * width + x]:
                continue
            i = x * 3
            pa = row_a[i:i + 3]
            pb = row_b[i:i + 3]
            if pa == pb or (abs(pa[0] - pb[0]) <= tol and abs(pa[1] - pb[1]) <= tol and abs(pa[2] - pb[2]) <= tol):
                continue
            # Both ways, or a thin line missing from one picture would pass (every pixel of the other has a background neighbour): the pixel of A needs a like
            # neighbour in B and the pixel of B a like neighbour in A.
            a_in_b = False
            b_in_a = False
            for dy in range(-aa, aa + 1):
                yy = y + dy
                if yy < 0 or yy >= height:
                    continue
                for dx in range(-aa, aa + 1):
                    xx = x + dx
                    if (dx == 0 and dy == 0) or xx < 0 or xx >= width:
                        continue
                    j = (yy * width + xx) * 3
                    a_in_b = a_in_b or (abs(pa[0] - b[j]) <= tol and abs(pa[1] - b[j + 1]) <= tol and abs(pa[2] - b[j + 2]) <= tol)
                    b_in_a = b_in_a or (abs(pb[0] - a[j]) <= tol and abs(pb[1] - a[j + 1]) <= tol and abs(pb[2] - a[j + 2]) <= tol)
            found = a_in_b and b_in_a
            if found:
                continue
            bad += 1
            x_min, y_min, x_max, y_max = min(x_min, x), min(y_min, y), max(x_max, x), max(y_max, y)
            if want_diff:
                diff[(y * width + x) * 3:(y * width + x) * 3 + 3] = b"\xff\x00\x00"
    return Comparison(compared, bad, (x_min, y_min, x_max, y_max) if bad else None, bytes(diff) if want_diff else None)


# ---- command line -------------------------------------------------------------------------------------------------------------

def _load_rgb(path, width=WIDTH, height=HEIGHT):
    w, h, rgb = read_png(path)
    if (w, h) != (width, height):
        raise ShotError("%s is %dx%d, not %dx%d" % (path, w, h, width, height))
    return rgb


def compare_main(args):
    try:
        a = _load_rgb(args.a)
        b = _load_rgb(args.b)
        regions = [parse_region(r) for r in args.region]
        ignores = [parse_region(r) for r in args.ignore]
    except (ShotError, ValueError, OSError, zlib.error) as error:
        print("o3shot: %s" % error, file=sys.stderr)
        return 2
    result = compare_images(a, b, WIDTH, HEIGHT, args.tol, args.aa, regions, ignores, want_diff=bool(args.diff))
    if args.diff:
        write_png(args.diff, WIDTH, HEIGHT, result.diff)
    print("compared %d pixels, %d differ beyond tol %d (aa %d): %.3f %%%s" %
          (result.compared, result.bad, args.tol, args.aa, 100.0 * result.fraction, "" if result.bbox is None else ", in x %d..%d y %d..%d" % (result.bbox[0], result.bbox[2], result.bbox[1], result.bbox[3])))
    if result.fraction > args.max_bad:
        print("o3shot: FAIL: more than %.3f %% of the compared pixels differ" % (100.0 * args.max_bad))
        return 1
    print("o3shot: pass")
    return 0


def decode_main(args):
    try:
        with open(args.capture, "r", errors="replace") as f:
            shot = parse_shot(f.read())
        if not shot.complete:
            print("o3shot: the capture is incomplete: %d bad lines, %d missing ranges" % (shot.bad_lines, len(shot.missing)), file=sys.stderr)
            return 1
        write_png(args.out, shot.width, shot.height, shot_rgb(shot))
    except (ShotError, OSError) as error:
        print("o3shot: %s" % error, file=sys.stderr)
        return 2
    print("o3shot: wrote %s (%dx%d, %s, moved=%d)" % (args.out, shot.width, shot.height, shot.fmt, int(shot.moved)))
    return 0


# ---- self-test --------------------------------------------------------------------------------------------------------------------

def make_capture(data, fmt, width=WIDTH, height=HEIGHT, line=96, moved=0, damage=None, drop=None, cut=False):
    """The text the firmware prints for `data` (the device's format, written independently here), for the tests. `damage`: line index whose payload is changed after its CRC;
    `drop`: line index left out."""
    lines = ["SHOT BEGIN fmt=%s w=%d h=%d bytes=%d line=%d" % (fmt, width, height, len(data), line)]
    n = 0
    for offset in range(0, len(data), line):
        piece = data[offset:offset + line]
        crc = zlib.crc32(piece) & 0xFFFFFFFF
        if damage == n:
            piece = bytes([piece[0] ^ 1]) + piece[1:]
        if drop != n:
            lines.append("SHOT D %06X %s %08X" % (offset, base64.b64encode(piece).decode(), crc))
        n += 1
    if not cut:
        lines.append("SHOT END lines=%d crc=%08X moved=%d" % (n, zlib.crc32(data) & 0xFFFFFFFF, moved))
    return "\r\n".join(lines) + "\r\n"


def _gradient565():
    out = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = (x * 31) // WIDTH, (y * 63) // HEIGHT, ((x + y) * 31) // (WIDTH + HEIGHT)
            out += struct.pack("<H", (r << 11) | (g << 5) | b)
    return bytes(out)


def self_test():
    problems = []
    data = _gradient565()
    # a clean capture round trips; the colours are the 565 expansion of what was sent
    shot = parse_shot("o3> shot\r\n" + make_capture(data, "565") + "o3> ")
    if not shot.complete or shot.moved or shot.data != data or shot.lines != 8000:
        problems.append("a clean 565 capture did not round trip")
    rgb = shot_rgb(shot)
    first = struct.unpack("<H", data[0:2])[0]
    if rgb[0:3] != bytes((((first >> 11) & 31) << 3 | ((first >> 11) & 31) >> 2, ((first >> 5) & 63) << 2 | ((first >> 5) & 63) >> 4, (first & 31) << 3 | (first & 31) >> 2)):
        problems.append("RGB565 expansion is wrong")
    if shot_rgb(Shot("565", 1, 2, b"\xff\xff\x00\xf8", False, 1, 0, []))[:6] != b"\xff\xff\xff\xff\x00\x00":
        problems.append("white and red in 565")
    # 8888: B G R A
    argb = bytes([10, 20, 30, 255] * (WIDTH * HEIGHT))
    s8 = parse_shot(make_capture(argb, "8888", line=96))
    if not s8.complete or shot_rgb(s8)[:3] != bytes([30, 20, 10]):
        problems.append("ARGB8 conversion is wrong")
    # 8888 pixel by pixel (OS-SIMKEY): every pixel its own B, G, R and an alpha that is dropped, so a swapped channel, a kept alpha, a shifted row or a half-width read shows
    pixels8 = bytearray()
    expect8 = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            b8, g8, r8, a8 = (x * 3 + y) & 255, (x + y * 5) & 255, (x ^ y) & 255, (x * 7 + y * 11) & 255
            pixels8 += bytes((b8, g8, r8, a8))
            expect8 += bytes((r8, g8, b8))
    s8 = parse_shot(make_capture(bytes(pixels8), "8888"))
    if not s8.complete or (s8.width, s8.height, s8.fmt) != (WIDTH, HEIGHT, "8888") or s8.lines != (len(pixels8) + 95) // 96:
        problems.append("a clean 8888 capture did not round trip")
    elif shot_rgb(s8) != bytes(expect8):
        problems.append("ARGB8 pixels are wrong (channel order, alpha or row length)")
    if shot_rgb(Shot("8888", 2, 1, bytes((1, 2, 3, 4, 5, 6, 7, 0)), False, 1, 0, [])) != bytes((3, 2, 1, 7, 6, 5)):
        problems.append("two 8888 pixels were converted wrong")
    try:  # a capture that stops short of its header's size (a chip that wrote half the rows) is refused, not shown
        parse_shot(make_capture(bytes(pixels8[:len(pixels8) // 2]), "8888"))
        problems.append("a half-size 8888 capture was accepted")
    except ShotError:
        pass
    # damaged, dropped, cut and refused transfers are told apart
    bad = parse_shot(make_capture(data, "565", damage=7))
    if bad.complete or bad.bad_lines != 1 or bad.missing != [(7 * 96, 8 * 96)]:
        problems.append("a damaged line was not found: %r %r" % (bad.bad_lines, bad.missing))
    gone = parse_shot(make_capture(data, "565", drop=0))
    if gone.complete or gone.missing != [(0, 96)]:
        problems.append("a dropped first line was not found")
    for label, text, wanted in (("cut", make_capture(data, "565", cut=True), "no SHOT END"), ("err", "SHOT BEGIN fmt=565 w=800 h=480 bytes=768000 line=96\r\nSHOT ERR snapshot timeout\r\n", "snapshot timeout"),
                                ("none", "o3> ", "no SHOT BEGIN")):
        try:
            parse_shot(text)
            problems.append("%s: no error" % label)
        except ShotError as error:
            if wanted not in str(error):
                problems.append("%s: %s" % (label, error))
    try:  # an END line whose CRC is not the picture's
        parse_shot(make_capture(data, "565").replace("crc=%08X" % (zlib.crc32(data) & 0xFFFFFFFF), "crc=00000000"))
        problems.append("a wrong END CRC passed")
    except ShotError:
        pass
    if not parse_shot(make_capture(data, "565", moved=1)).moved:
        problems.append("moved=1 was lost")
    # PNG round trip (and a hand-made RGBA file with every filter, as the simulator's encoder may write)
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "a.png")
        write_png(path, WIDTH, HEIGHT, rgb)
        w, h, back = read_png(path)
        if (w, h) != (WIDTH, HEIGHT) or back != rgb:
            problems.append("the PNG did not round trip")
        w2, h2 = 7, 5
        pixels = bytes((x * 31 + y * 17 + c * 5) & 255 for y in range(h2) for x in range(w2) for c in range(4))
        stride = w2 * 4
        for filt in range(5):
            raw = bytearray()
            prev = bytes(stride)
            for y in range(h2):
                line = pixels[y * stride:(y + 1) * stride]
                filtered = bytearray()
                for i in range(stride):
                    a = line[i - 4] if i >= 4 else 0
                    b = prev[i]
                    c = prev[i - 4] if i >= 4 else 0
                    if filt == 0:
                        pred = 0
                    elif filt == 1:
                        pred = a
                    elif filt == 2:
                        pred = b
                    elif filt == 3:
                        pred = (a + b) >> 1
                    else:
                        p = a + b - c
                        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                        pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    filtered.append((line[i] - pred) & 255)
                raw += bytes([filt]) + filtered
                prev = line
            fpath = os.path.join(tmp, "f%d.png" % filt)
            with open(fpath, "wb") as f:
                f.write(b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", w2, h2, 8, 6, 0, 0, 0)) + _chunk(b"IDAT", zlib.compress(bytes(raw))) + _chunk(b"IEND", b""))
            rw, rh, rrgb = read_png(fpath)
            expect = bytearray()
            for i in range(0, len(pixels), 4):
                expect += pixels[i:i + 3]
            if (rw, rh) != (w2, h2) or rrgb != bytes(expect):
                problems.append("PNG filter %d was read wrong" % filt)
        with open(os.path.join(tmp, "bad.png"), "wb") as f:
            f.write(b"not a png")
        try:
            read_png(os.path.join(tmp, "bad.png"))
            problems.append("a non-PNG was read")
        except ShotError:
            pass
    # the compare: identical, within tolerance, an edge shifted by a pixel (anti-aliasing), a real difference, regions and ignores
    w, h = 40, 30
    base = bytearray(b"\x10\x20\x30" * (w * h))
    for y in range(h):
        for x in range(20, w):
            base[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\xe0\xe0\xe0"
    same = compare_images(bytes(base), bytes(base), w, h)
    if same.bad != 0 or same.compared != w * h:
        problems.append("identical pictures differ")
    near = bytearray(base)
    for i in range(len(near)):
        near[i] = min(255, near[i] + 8)
    if compare_images(bytes(base), bytes(near), w, h, tol=24).bad != 0:
        problems.append("a difference of 8 levels was not within tol 24")
    if compare_images(bytes(base), bytes(near), w, h, tol=4, aa=0).bad != w * h:
        problems.append("a difference of 8 levels passed tol 4")
    shifted = bytearray(base)
    for y in range(h):  # the edge moves one pixel right
        shifted[(y * w + 20) * 3:(y * w + 20) * 3 + 3] = b"\x10\x20\x30"
    if compare_images(bytes(base), bytes(shifted), w, h, tol=8, aa=1).bad != 0:
        problems.append("an edge shifted by one pixel was not forgiven by aa 1")
    if compare_images(bytes(base), bytes(shifted), w, h, tol=8, aa=0).bad != h:
        problems.append("an edge shifted by one pixel passed aa 0")
    moved = bytearray(base)
    for y in range(h):  # two pixels is no anti-aliasing
        for x in (20, 21):
            moved[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\x10\x20\x30"
    if compare_images(bytes(base), bytes(moved), w, h, tol=8, aa=1).bad == 0:
        problems.append("an edge shifted by two pixels was forgiven by aa 1")
    blob = bytearray(base)
    for y in range(5, 10):
        for x in range(5, 10):
            blob[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\x00\xff\x00"
    r = compare_images(bytes(base), bytes(blob), w, h, tol=24, aa=1, want_diff=True)
    if r.bad != 25 or r.bbox != (5, 5, 9, 9) or r.diff is None or r.diff[(5 * w + 5) * 3:(5 * w + 5) * 3 + 3] != b"\xff\x00\x00":
        problems.append("a real difference was not found: %r %r" % (r.bad, r.bbox))
    if compare_images(bytes(base), bytes(blob), w, h, regions=[(20, 0, 20, 30)]).bad != 0:
        problems.append("a region that does not hold the difference found it")
    if compare_images(bytes(base), bytes(blob), w, h, regions=[(0, 0, 20, 30)], ignores=[(5, 5, 5, 5)]).bad != 0:
        problems.append("an ignored part was compared")
    if compare_images(bytes(base), bytes(blob), w, h, regions=[(0, 0, 20, 30)]).compared != 20 * 30:
        problems.append("the compared count is not the region's")
    try:
        parse_region("1,2,3")
        problems.append("a bad region was accepted")
    except ValueError:
        pass
    if problems:
        print("o3shot self-test FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print("o3shot self-test passed")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Decode `diag shot` captures and compare screen pictures.")
    parser.add_argument("--self-test", action="store_true", help="run the self-test and exit")
    sub = parser.add_subparsers(dest="command")
    d = sub.add_parser("decode", help="a saved `shot` text to a PNG")
    d.add_argument("capture")
    d.add_argument("out")
    c = sub.add_parser("compare", help="two PNGs of 800x480")
    c.add_argument("a")
    c.add_argument("b")
    c.add_argument("--tol", type=int, default=24, help="largest difference of one colour channel that is still the same colour (default 24)")
    c.add_argument("--aa", type=int, default=1, help="a pixel also matches a neighbour of the other picture within this many pixels (default 1; 0 turns it off)")
    c.add_argument("--max-bad", type=float, default=0.005, help="largest share of the compared pixels that may differ, 0..1 (default 0.005)")
    c.add_argument("--region", action="append", default=[], metavar="x,y,w,h", help="compare only this rectangle (repeatable)")
    c.add_argument("--ignore", action="append", default=[], metavar="x,y,w,h", help="leave this rectangle out (repeatable)")
    c.add_argument("--diff", metavar="OUT.png", help="write the first picture dimmed with the differing pixels in red")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.command == "decode":
        return decode_main(args)
    if args.command == "compare":
        return compare_main(args)
    parser.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
