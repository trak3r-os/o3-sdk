#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert BDF bitmap fonts into FT81x L1 bitmap data plus a metric record (stdlib only).

The UI draws text as glyph bitmaps from RAM_G, so each face becomes one L1 bitmap whose cells are the glyphs of one
character range (docs/specs/OS-003-ui-toolkit.md, ADR 0010 section 3).
  * L1 = 1 bit per pixel, most significant bit = leftmost pixel, `stride` bytes per row, rows top to bottom.
    Cell n of a bitmap starts at BITMAP_SOURCE + n * stride * height (FT81x Programmer's Guide, section
    4.20 CELL), so cell = char - first_char and the whole face is one contiguous block.
  * The metric record (cell size, stride, first char, glyph count, ascent, cap height, handle, RAM_G address) is
    everything the renderer needs. The faces are monospace, so there is no per-glyph advance table.
  * The four faces are fixed by name, one per cell size (ui::Font): f16x32, f10x20, f8x16, f6x12. Bitmap handle =
    1 + position in that order; RAM_G addresses are packed from 0, each 4-byte aligned. ui/assets.h owns the rest of
    the RAM_G map (tiles and icons start at kRamGTiles) and the generated file asserts that the faces fit below it.
  * Every bitmap array is declared word aligned on its type and asserted, never left to where the linker puts a byte
    array (ADR 0011: it is uploaded with hal::Display::upload(), whose source must be word aligned).
  * Output is a C++ source file defining ui::font_face(); it is written into the build tree and not committed.

Usage:
  font_convert.py --out font_data.cpp --face f16x32=<ter-u32?.bdf> --face f10x20=<ter-u20?.bdf> \\
      --face f8x16=<spleen-8x16.bdf> --face f6x12=<spleen-6x12.bdf>
  font_convert.py --self-test [--spleen-dir <dir>] [--terminus-dir <dir>]

Exit 0 on success, 1 on any error (missing glyph, bad BDF, a BDF whose cell size is not the face's name).
"""
import argparse
import sys
from pathlib import Path

FIRST_CHAR = 32     # space
LAST_CHAR = 126     # '~'
FONT_NAMES = ("f16x32", "f10x20", "f8x16", "f6x12")  # order of ui::Font
RAM_G_SIZE = 1 << 20  # FT81x RAM_G (FT81X Series Programmer's Guide, section 2.2)


class BdfError(Exception):
    pass


class Bdf:
    """The parts of a BDF file the converter needs: bounding box, ascent, glyph bitmaps by encoding."""

    def __init__(self, text, name="<bdf>"):
        self.name = name
        self.bbox = None            # (w, h, xoff, yoff)
        self.ascent = None
        self.glyphs = {}            # encoding -> (bbx_w, bbx_h, xoff, yoff, [row ints])
        self._parse(text)

    def _parse(self, text):
        lines = text.splitlines()
        i = 0
        while i < len(lines):
            parts = lines[i].split()
            key = parts[0] if parts else ""
            if key == "FONTBOUNDINGBOX" and len(parts) == 5:
                self.bbox = tuple(int(p) for p in parts[1:])
            elif key == "FONT_ASCENT" and len(parts) == 2:
                self.ascent = int(parts[1])
            elif key == "STARTCHAR":
                i = self._parse_char(lines, i)
                continue
            i += 1
        if self.bbox is None:
            raise BdfError("%s: no FONTBOUNDINGBOX" % self.name)
        if self.ascent is None:
            # No FONT_ASCENT: the bounding box top is the ascent (yoff is the descent, negative or zero).
            self.ascent = self.bbox[1] + self.bbox[3]

    def _parse_char(self, lines, i):
        encoding = bbx = None
        i += 1
        while i < len(lines) and lines[i].split()[:1] != ["BITMAP"]:
            parts = lines[i].split()
            if parts[:1] == ["ENCODING"]:
                encoding = int(parts[1])
            elif parts[:1] == ["BBX"]:
                bbx = tuple(int(p) for p in parts[1:5])
            i += 1
        if encoding is None or bbx is None:
            raise BdfError("%s: glyph without ENCODING or BBX near line %d" % (self.name, i + 1))
        i += 1  # BITMAP
        rows = []
        while i < len(lines) and lines[i].strip() != "ENDCHAR":
            rows.append(int(lines[i].strip(), 16))
            i += 1
        if len(rows) != bbx[1]:
            raise BdfError("%s: glyph %d has %d rows, BBX says %d" % (self.name, encoding, len(rows), bbx[1]))
        if encoding >= 0:
            self.glyphs[encoding] = bbx + (rows,)
        return i + 1  # past ENDCHAR


def glyph_cell(font, code, cell_w, cell_h, stride):
    """The L1 bytes (stride * cell_h) of one glyph placed into the cell, baseline at row `ascent`."""
    if code not in font.glyphs:
        raise BdfError("%s: no glyph for U+%04X" % (font.name, code))
    bw, bh, xoff, yoff, rows = font.glyphs[code]
    row_bytes = (bw + 7) // 8
    top = font.ascent - (yoff + bh)     # cell row of the glyph's first bitmap row
    left = xoff - font.bbox[2]          # cell column of the glyph's leftmost bitmap column
    cell = [0] * (stride * cell_h)
    for r, bits in enumerate(rows):
        y = top + r
        for x in range(bw):
            if not (bits >> (row_bytes * 8 - 1 - x)) & 1:
                continue
            cx = left + x
            if not (0 <= y < cell_h and 0 <= cx < cell_w):
                raise BdfError("%s: glyph U+%04X does not fit the %dx%d cell" % (font.name, code, cell_w, cell_h))
            cell[y * stride + cx // 8] |= 0x80 >> (cx % 8)
    return cell


def cap_height(cell, cell_h, stride):
    """Rows from the first to the last inked row of one cell (the capital H): the face's cap height."""
    inked = [y for y in range(cell_h) if any(cell[y * stride:(y + 1) * stride])]
    return inked[-1] - inked[0] + 1 if inked else 0


def convert_face(font):
    """(cell_w, cell_h, stride, ascent, cap_h, bytes) for the ASCII printable range of one BDF."""
    cell_w, cell_h = font.bbox[0], font.bbox[1]
    stride = (cell_w + 7) // 8
    data = []
    for code in range(FIRST_CHAR, LAST_CHAR + 1):
        data.extend(glyph_cell(font, code, cell_w, cell_h, stride))
    h = glyph_cell(font, ord("H"), cell_w, cell_h, stride)
    return cell_w, cell_h, stride, font.ascent, cap_height(h, cell_h, stride), data


def parse_face_arg(arg):
    """'f10x20=path.bdf' -> (name, path)."""
    name, _, path = arg.partition("=")
    if name not in FONT_NAMES or not path:
        raise SystemExit("font_convert: bad --face '%s' (want name=file.bdf, name in %s)" % (arg, FONT_NAMES))
    return name, Path(path)


def name_size(name):
    w, _, h = name[1:].partition("x")
    return int(w), int(h)


def align4(n):
    return (n + 3) & ~3


def layout(faces):
    """RAM_G address of each face, packed from 0 in ui::Font order, each 4-byte aligned."""
    addrs, at = {}, 0
    for name in FONT_NAMES:
        addrs[name] = at
        at = align4(at + len(faces[name][5]))
    if at > RAM_G_SIZE:
        raise BdfError("the faces do not fit RAM_G (%d bytes)" % at)
    return addrs, at


def emit_cpp(faces, sources):
    addrs, end = layout(faces)
    out = ["// Generated by tools/font_convert.py -- do not edit, do not commit.",
           "// Glyph data: " + "; ".join(sources),
           "// Spleen is (c) 2018-2026 Frederic Cambus, BSD-2-Clause; see third_party/fonts/spleen/LICENSE.",
           "// Terminus is (c) 2020 Dimitar Toshkov Zhekov, SIL Open Font License 1.1, with Reserved Font Name \"Terminus",
           "// Font\"; see third_party/fonts/terminus/OFL.TXT. The bitmaps below are a Modified Version in the sense of",
           "// the licence (a format conversion): they are named by cell size (f10x20, f16x32), never \"Terminus\".",
           "// SPDX-License-Identifier: BSD-2-Clause AND OFL-1.1",
           "#include <o3sdk/ui/assets.h>",
           "#include <o3sdk/ui/font.h>",
           "",
           "namespace o3sdk::ui {",
           "namespace {",
           "",
           "// A bitmap is sent to RAM_G with hal::Display::upload(), which wants a word-aligned source (the device may",
           "// read it with LDM/LDRD or DMA; a misaligned LDM/LDRD is a UsageFault, ADR 0011). So the alignment is",
           "// declared on the type, never left to where the linker puts a byte array.",
           "template <unsigned N>",
           "struct alignas(4) Bitmap {",
           "  std::uint8_t bytes[N];",
           "};",
           ""]
    for name in FONT_NAMES:
        cw, ch, stride, ascent, cap, data = faces[name]
        out.append("// %s: %dx%d cells, %d glyphs from U+%04X, %d bytes of L1 data, RAM_G %u, handle %d."
                   % (name, cw, ch, LAST_CHAR - FIRST_CHAR + 1, FIRST_CHAR, len(data), addrs[name],
                      FONT_NAMES.index(name) + 1))
        out.append("constexpr Bitmap<%d> k_%s_bitmap = {{" % (len(data), name))
        for j in range(0, len(data), 16):
            out.append("    " + ", ".join("0x%02X" % b for b in data[j:j + 16]) + ",")
        out.append("}};")
        out.append("static_assert(alignof(Bitmap<%d>) >= 4 && sizeof(Bitmap<%d>) >= %d, \"%s bitmap: word aligned, whole\");"
                   % (len(data), len(data), len(data), name))
        out.append("")
    out.append("constexpr FontFace kFaces[%d] = {" % len(FONT_NAMES))
    for i, name in enumerate(FONT_NAMES):
        cw, ch, stride, ascent, cap, data = faces[name]
        out.append("    {k_%s_bitmap.bytes, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d},  // %s" % (
            name, len(data), addrs[name], i + 1, cw, ch, stride, FIRST_CHAR, LAST_CHAR - FIRST_CHAR + 1, ascent, cap,
            name))
    out.append("};")
    out.append("")
    out.append("// The geometry the layouts use at compile time (font.h font_geometry) is the geometry of these bitmaps.")
    for i, name in enumerate(FONT_NAMES):
        cw, ch, stride, ascent, cap, data = faces[name]
        out.append("static_assert(font_geometry(Font::%s).cell_w == %d && font_geometry(Font::%s).cell_h == %d &&"
                   % ("F" + name[1:], cw, "F" + name[1:], ch))
        out.append("                  font_geometry(Font::%s).ascent == %d && font_geometry(Font::%s).cap_h == %d,"
                   % ("F" + name[1:], ascent, "F" + name[1:], cap))
        out.append("              \"%s: font.h geometry differs from the BDF\");" % name)
    out.append("static_assert(%d <= kRamGTiles, \"the faces overflow the RAM_G region ui/assets.h reserves for them\");"
               % end)
    out += ["", "}  // namespace", "",
            "const FontFace& font_face(Font font) noexcept { return kFaces[static_cast<unsigned>(font)]; }", "",
            "}  // namespace o3sdk::ui", ""]
    return "\n".join(out)


def generate(args):
    faces, sources = {}, []
    for arg in args.face:
        name, path = parse_face_arg(arg)
        try:
            font = Bdf(path.read_text(encoding="utf-8"), path.name)
            face = convert_face(font)
        except (OSError, BdfError, ValueError) as e:
            print("font_convert: %s: %s" % (path, e), file=sys.stderr)
            return 1
        if (face[0], face[1]) != name_size(name):
            print("font_convert: %s is a %dx%d font, not the %s face" % (path, face[0], face[1], name), file=sys.stderr)
            return 1
        if name in faces:
            print("font_convert: duplicate face %s" % name, file=sys.stderr)
            return 1
        faces[name] = face
        sources.append("%s <- %s" % (name, path.name))
    if sorted(faces) != sorted(FONT_NAMES):
        print("font_convert: need exactly the faces %s" % ", ".join(FONT_NAMES), file=sys.stderr)
        return 1
    try:
        text = emit_cpp(faces, sources)
    except BdfError as e:
        print("font_convert: %s" % e, file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if not args.out.exists() or args.out.read_text() != text:  # keep the mtime when nothing changed
        args.out.write_text(text)
    return 0


MINI_BDF = """STARTFONT 2.1
FONTBOUNDINGBOX 8 4 0 -1
STARTPROPERTIES 1
FONT_ASCENT 3
ENDPROPERTIES
CHARS 2
STARTCHAR a
ENCODING 97
BBX 3 2 1 0
BITMAP
E0
A0
ENDCHAR
STARTCHAR b
ENCODING 98
BBX 8 4 0 -1
BITMAP
80
01
FF
00
ENDCHAR
ENDFONT
"""


def self_test(spleen_dir, terminus_dir):
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    font = Bdf(MINI_BDF, "mini")
    check(font.bbox == (8, 4, 0, -1) and font.ascent == 3, "mini: bbox/ascent")
    # 'a': 3x2 bitmap, xoff 1, yoff 0 -> bottom row of glyph on baseline row 3, i.e. rows 1 and 2 of 4, from column 1.
    check(glyph_cell(font, 97, 8, 4, 1) == [0x00, 0x70, 0x50, 0x00], "mini: glyph placement %r" % glyph_cell(font, 97, 8, 4, 1))
    check(glyph_cell(font, 98, 8, 4, 1) == [0x80, 0x01, 0xFF, 0x00], "mini: full-cell glyph")
    check(cap_height([0, 0x70, 0x50, 0], 4, 1) == 2 and cap_height([0, 0, 0, 0], 4, 1) == 0, "cap height")
    try:
        glyph_cell(font, 99, 8, 4, 1)
        failures.append("mini: missing glyph accepted")
    except BdfError:
        pass
    check(parse_face_arg("f10x20=x.bdf") == ("f10x20", Path("x.bdf")), "face arg")
    check(name_size("f16x32") == (16, 32) and name_size("f6x12") == (6, 12), "name size")

    # Every generated bitmap is declared word aligned on its type and asserted (ADR 0011), never left to placement;
    # the faces are packed from RAM_G 0, 4-byte aligned, in ui::Font order, with handles 1..4.
    sizes = [(16, 32, 2), (10, 20, 2), (8, 16, 1), (6, 12, 1)]
    faces = {n: (w, h, s, h * 3 // 4, h // 2, [0x80] * (95 * s * h)) for n, (w, h, s) in zip(FONT_NAMES, sizes)}
    addrs, end = layout(faces)
    check([addrs[n] for n in FONT_NAMES] == [0, 6080, 9880, 11400] and end == 12540, "layout %r end %d" % (addrs, end))
    check(all(a % 4 == 0 for a in addrs.values()), "layout aligned")
    text = emit_cpp(faces, ["mini"])
    check(text.count("struct alignas(4) Bitmap") == 1, "emit: aligned bitmap type")
    check(text.count("constexpr Bitmap<") == len(FONT_NAMES), "emit: one aligned array per face")
    check(text.count("static_assert(alignof(Bitmap<") == len(FONT_NAMES), "emit: alignment asserted per face")
    check(".bytes," in text and "[8] = {" not in text, "emit: faces point at the aligned bytes, no bare byte array")
    check("static_assert(12540 <= kRamGTiles" in text, "emit: the faces are asserted to fit below the tiles")
    check("font.h geometry differs from the BDF" in text, "emit: geometry asserted")

    if spleen_dir:  # the real Spleen 8x16: 'A' is rows 00 00 7C C6 C6 C6 FE C6 C6 C6 C6 C6 00 00 00 00
        path = Path(spleen_dir) / "spleen-8x16.bdf"
        real = Bdf(path.read_text(encoding="utf-8"), path.name)
        cw, ch, stride, ascent, cap, data = convert_face(real)
        check((cw, ch, stride, ascent, cap) == (8, 16, 1, 12, 10), "spleen 8x16 metrics %r" % ((cw, ch, stride, ascent, cap),))
        check(len(data) == (LAST_CHAR - FIRST_CHAR + 1) * 16, "spleen 8x16 size")
        a = data[(ord("A") - FIRST_CHAR) * 16:][:16]
        check(a == [0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0], "spleen 'A' %r" % a)
        check(not any(data[:16]), "spleen space is blank")
        small = Bdf((Path(spleen_dir) / "spleen-6x12.bdf").read_text(encoding="utf-8"), "spleen-6x12.bdf")
        m = convert_face(small)
        check((m[0], m[1], m[2]) == (6, 12, 1), "spleen 6x12 metrics")
    if terminus_dir:  # the real Terminus: the cell sizes, cap heights, and that every ASCII glyph is there
        for name, ter in (("f10x20", "ter-u20"), ("f16x32", "ter-u32")):
            for weight in "nb":
                path = Path(terminus_dir) / ("%s%s.bdf" % (ter, weight))
                if not path.exists():
                    continue
                real = Bdf(path.read_text(encoding="utf-8"), path.name)
                m = convert_face(real)
                check((m[0], m[1]) == name_size(name), "%s metrics %r" % (path.name, m[:5]))
                check(len(m[5]) == (LAST_CHAR - FIRST_CHAR + 1) * m[2] * m[1], "%s size" % path.name)
                check(not any(m[5][:m[2] * m[1]]), "%s space is blank" % path.name)
                check(all(any(m[5][i * m[2] * m[1]:(i + 1) * m[2] * m[1]]) for i in range(1, 95)),
                      "%s every printable glyph has ink" % path.name)
    print("font_convert self-test: %s" % ("FAILED" if failures else "ok"))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--face", action="append", default=[])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--spleen-dir", help="with --self-test: also check the real Spleen BDFs in this directory")
    parser.add_argument("--terminus-dir", help="with --self-test: also check the real Terminus BDFs in this directory")
    args = parser.parse_args()
    if args.self_test:
        return self_test(args.spleen_dir, args.terminus_dir)
    if not args.out or not args.face:
        parser.error("--out and --face are required")
    return generate(args)


if __name__ == "__main__":
    sys.exit(main())
