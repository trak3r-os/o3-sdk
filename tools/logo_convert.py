#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The boot logo: a PNG turned into an FT81x bitmap that lives in flash (stdlib only).

OS-003f, docs/specs/OS-003-ui-toolkit.md "003f - Boot logo". The kit's default source is logo/o3.png (the maintainer's O3 logo, the one
source of truth); an app has its own with o3sdk_app(... LOGO <png>), which runs this tool. The pipeline, every step deterministic
(integer arithmetic only):

  1. decode      a PNG decoder on zlib: non-interlaced 8-bit gray, gray + alpha, RGB and RGBA, every filter type, CRCs
                 checked; alpha is composited on black (the boot screen is black); then luminance (ITU-R BT.601, integer)
  2. colour      a picture with a pixel of real chroma (max - min of its channels above CHROMA_THRESHOLD) is a colour logo, and goes
                 to PALETTED565 (step 6); every other one is grey: L1, L2 or L4, drawn through COLOR_RGB (the tint, white by default)
  3. crop        the bounding box of everything brighter than CROP_THRESHOLD: the black margin costs flash and size, and
                 cropping centres the picture itself, not its margin
  4. grid        detect the pixel-art grid (an integer block size and its phase, per axis) from where the edges of the
                 picture lie, and take the centre of every block. The fit is accepted only if at least GRID_MIN_PERMILLE
                 of all the edge energy lies on block boundaries. A picture whose "pixels" are not a regular grid (an
                 AI-drawn one, a halftone, a photograph) is not forced onto one; its scale is the largest that fits the panel
  5. resample    no grid: the crop is resampled by box filtering (BOX_NUM / BOX_DEN of each cell) to the FINEST native size whose
                 bitmap fits MAX_BYTES: scale 1 (native size = the shown size) first, then 2, 3 ... The shown size is at most
                 MAX_DRAW_W x MAX_DRAW_H (the panel, less the line under the logo). A picture smaller than that keeps its own
                 pixels and gets the largest scale that fits
  6. format      grey: a native picture that is nearly black and white (BILEVEL_PERMILLE of its pixels are within BILEVEL_LOW of
                 black or BILEVEL_HIGH of white: a halftone, line art) is L1, because its information is in where the dots are,
                 not in grey levels, and one bit buys the finest native size; any other the smallest of L1, L2, L4 whose
                 quantisation error (RMS in grey levels, against the native image) is at most FORMAT_MAX_RMS, L4 if none is.
                 colour: PALETTED565, an 8-bit index a pixel and a palette of at most 256 RGB565 colours after it in the blob
                 (index 0 is black, the background; median cut for the rest, no dithering). Also reported: the errors against the
                 source at the size it is shown
  7. output      a C++ source with the pixels in a word-aligned const array, rows MSB first, a BootLogo that describes them, and
                 static_asserts that tie it to ui/boot_logo.h (the limits); a ctest regenerates the default's and fails when the
                 committed one differs

Usage:
  logo_convert.py --png o3.png --weak --out boot_logo_data.cpp     (rewritten only when it differs; --weak: the kit's default)
  logo_convert.py --png o3.png --weak --check boot_logo_data.cpp   (exit 1 when the committed file is stale)
  logo_convert.py --png o3.png --report                            (what it decided, no output file)
  logo_convert.py --png o3.png --preview native.png                (the native bitmap at screen size, to look at)
  logo_convert.py --png logo.png --tint 0xFF8000 --out data.cpp    (a grey logo drawn in orange; refused for a colour one)
  logo_convert.py --png logo.png --scale 2 --out data.cpp          (a native size chosen by hand: the shown size / 2)
  logo_convert.py --make-test-png out.png --kind colour            (a small made-up logo, grey or colour, for the build's tests)
  logo_convert.py --self-test
"""
import argparse
import collections
import hashlib
import itertools
import math
import operator
import struct
import sys
import tempfile
import zlib
from pathlib import Path

# ---- Tunables (each is a decision, written down here and echoed in the generated file) ---------------------------------

CROP_THRESHOLD = 24          # luminance above this is picture; the logo's own margin is black (0..1)
CHROMA_THRESHOLD = 32        # max - min of a pixel's channels above this is colour; a grey picture saved once as a JPEG (logo/o3.png) reaches 13
GRID_MIN_PITCH = 2           # the block sizes tried, in source pixels
GRID_MAX_PITCH = 64
GRID_MIN_PERMILLE = 850      # share of the edge energy that must lie on block boundaries for a grid to be accepted
GRID_TIE_PERMILLE = 20       # a larger block size within this of the best score wins (a grid of 7 is also one of 14 if it can be)
BOX_NUM, BOX_DEN = 1, 1      # the whole cell is averaged (a pixel is the coverage of its cell). It was the central half for a checkerboard
                             # dither, which a full box turns to flat grey; a halftone is dots whose AREA is the picture, and a half
                             # window would lose a small dot that falls between two samples and make the others vary with their phase
BILEVEL_LOW, BILEVEL_HIGH = 32, 224  # luminance this dark or this light is "black" or "white"
BILEVEL_PERMILLE = 850       # share of the NATIVE picture that is black or white for it to count as bilevel (logo/o3.png at 684x420: 929; a
                             # finely dithered drawing, which averages to greys when it is shrunk, is 779): the dots' positions are the picture
BILEVEL_CUT = 96             # a bilevel picture's pixel is lit from this grey (of 255) up, not from half: a dot of one or two pixels that
                             # straddles pixel boundaries averages under 128 in each and would vanish, and the lattice of small edge dots
                             # breaks; a big dot grows by well under a pixel
FORMAT_MAX_RMS = 6           # grey levels (of 255): above this a format does not keep the dither
TONE_BLOCK = 4               # the tone error compares the mean of every 4x4 patch of native pixels

# The limits of ui/boot_logo.h (the generated file static_asserts that they are the same).
MAX_DRAW_W, MAX_DRAW_H = 800, 420   # the panel's width; 420 of its 480 rows, so that the line under the logo fits
MAX_SCALE = 16
MAX_BYTES = 64 * 1024               # bitmap, padding to a word and palette: the RAM_G region, and the most flash a logo costs
PALETTE_ENTRIES = 256
PALETTE_BYTES = PALETTE_ENTRIES * 2

# FT81x BITMAP_LAYOUT formats (Programmers Guide v1.2 Table 7, p.97) in the order they are tried: fewest bits first. PALETTED565 (14)
# is the colour format: an 8-bit index into a 256-entry RGB565 palette (PALETTE_SOURCE).
FORMATS = (("L1", 1, 1), ("L2", 17, 2), ("L4", 2, 4))  # name, BITMAP_LAYOUT value, bits per pixel
PALETTED = ("Paletted565", 14, 8)

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}   # colour type -> samples per pixel (8-bit only)
MAX_SIDE = 4096                        # a side: with MAX_DECODED_BYTES, a decode never needs more than about 64 MiB (4096 x 4096 RGBA)
MAX_DECODED_BYTES = 64 * 1024 * 1024   # width x height x samples a pixel


class LogoError(Exception):
    pass


# ---- 1. PNG ------------------------------------------------------------------------------------------------------------

def read_chunks(data):
    """The chunks of a PNG as (type, body), CRC checked, up to and including IEND."""
    if data[:8] != PNG_SIGNATURE:
        raise LogoError("not a PNG (bad signature)")
    pos = 8
    chunks = []
    while True:
        if pos + 8 > len(data):
            raise LogoError("PNG truncated: no IEND")
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 8 + length
        if end + 4 > len(data):
            raise LogoError("PNG truncated inside a %s chunk" % ctype.decode("latin-1"))
        body = data[pos + 8:end]
        (crc,) = struct.unpack(">I", data[end:end + 4])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise LogoError("bad CRC in a %s chunk" % ctype.decode("latin-1"))
        chunks.append((ctype, body))
        pos = end + 4
        if ctype == b"IEND":
            return chunks


def unfilter(raw, width, height, bpp):
    """The scanlines of a decompressed non-interlaced image: filter types 0..4 undone (PNG spec section 9)."""
    stride = width * bpp
    if len(raw) != height * (1 + stride):
        raise LogoError("image data is %d bytes, %d expected" % (len(raw), height * (1 + stride)))
    rows = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub: the byte bpp to the left, per channel
            for c in range(bpp):
                line[c::bpp] = bytearray(itertools.accumulate(line[c::bpp], lambda a, b: (a + b) & 255))
        elif ftype == 2:  # Up
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif ftype == 3:  # Average of left and up
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 255
        else:
            raise LogoError("unknown PNG filter type %d" % ftype)
        rows.append(line)
        prev = line
    return rows


def decode_png(data):
    """-> (width, height, colour type, rows): each row a bytearray of width * channels samples."""
    chunks = read_chunks(data)
    if not chunks or chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise LogoError("PNG: the first chunk must be a 13-byte IHDR")
    width, height, depth, ctype, comp, filt, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    if width < 1 or height < 1 or width > MAX_SIDE or height > MAX_SIDE:
        raise LogoError("PNG: %dx%d is outside 1..%d" % (width, height, MAX_SIDE))
    if depth != 8:
        raise LogoError("PNG: bit depth %d is not supported (8 only)" % depth)
    if ctype not in CHANNELS:
        raise LogoError("PNG: colour type %d is not supported (gray, gray + alpha, RGB, RGBA; not palette)" % ctype)
    if comp != 0 or filt != 0:
        raise LogoError("PNG: unknown compression or filter method")
    if interlace != 0:
        raise LogoError("PNG: interlaced images are not supported")
    bpp = CHANNELS[ctype]
    if width * height * bpp > MAX_DECODED_BYTES:
        raise LogoError("PNG: %dx%d at %d samples a pixel is more than %d MiB decoded" % (width, height, bpp, MAX_DECODED_BYTES >> 20))
    idat = b"".join(body for t, body in chunks if t == b"IDAT")
    if not idat:
        raise LogoError("PNG: no image data")
    expected = height * (1 + width * bpp)
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(idat, expected + 1)  # bounded: a bomb stops one byte past the size it claims
    except zlib.error as e:
        raise LogoError("PNG: image data does not inflate (%s)" % e)
    if len(raw) != expected or not inflater.eof:
        raise LogoError("PNG: image data is not %d bytes" % expected)
    return width, height, ctype, unfilter(raw, width, height, bpp)


def luminance(width, ctype, rows):
    """Rows of 0..255 luminance, integer BT.601 (299 R + 587 G + 114 B), alpha composited on black."""
    out = []
    for row in rows:
        if ctype == 0:
            y = list(row)
        elif ctype == 4:
            y = [(v * a + 127) // 255 for v, a in zip(row[0::2], row[1::2])]
        elif ctype == 2:
            y = [(299 * r + 587 * g + 114 * b + 500) // 1000 for r, g, b in zip(row[0::3], row[1::3], row[2::3])]
        else:
            y = [((299 * r + 587 * g + 114 * b + 500) // 1000 * a + 127) // 255
                 for r, g, b, a in zip(row[0::4], row[1::4], row[2::4], row[3::4])]
        out.append(y)
    return out


def channels(ctype, rows):
    """-> (r, g, b): three planes of rows of 0..255, alpha composited on black. A grey picture's planes are one and the same list."""
    if ctype in (0, 4):
        grey = luminance(0, ctype, rows)
        return grey, grey, grey
    r, g, b = [], [], []
    for row in rows:
        if ctype == 2:
            pr, pg, pb = list(row[0::3]), list(row[1::3]), list(row[2::3])
        else:
            a = row[3::4]
            pr, pg, pb = ([(v * al + 127) // 255 for v, al in zip(row[c::4], a)] for c in range(3))
        r.append(pr)
        g.append(pg)
        b.append(pb)
    return r, g, b


def max_chroma(planes):
    """The largest max - min of the channels of any pixel."""
    r, g, b = planes
    if r is g and g is b:
        return 0
    best = 0
    for pr, pg, pb in zip(r, g, b):
        best = max(best, max(map(operator.sub, map(max, pr, pg, pb), map(min, pr, pg, pb))))
    return best


def bilevel_permille(lum):
    """Share (permille) of the pixels that are black (at most BILEVEL_LOW) or white (at least BILEVEL_HIGH)."""
    total = sum(len(row) for row in lum)
    extreme = sum(sum(1 for v in row if v <= BILEVEL_LOW or v >= BILEVEL_HIGH) for row in lum)
    return extreme * 1000 // total


# ---- 2. Crop and the summed-area table -----------------------------------------------------------------------------------

def crop_box(rows, threshold=CROP_THRESHOLD):
    """(x0, y0, x1, y1), exclusive ends, of the pixels brighter than `threshold`."""
    ys = [y for y, row in enumerate(rows) if max(row) > threshold]
    if not ys:
        raise LogoError("the picture is blank (nothing brighter than %d)" % threshold)
    col_max = list(map(max, zip(*rows)))
    xs = [x for x, v in enumerate(col_max) if v > threshold]
    return xs[0], ys[0], xs[-1] + 1, ys[-1] + 1


def cropped(rows, box):
    x0, y0, x1, y1 = box
    return [row[x0:x1] for row in rows[y0:y1]]


def integral(rows):
    """Summed-area table: sat[y][x] is the sum of rows[:y][:x]."""
    sat = [[0] * (len(rows[0]) + 1)]
    for row in rows:
        acc = list(itertools.accumulate(row, initial=0))
        sat.append(list(map(operator.add, acc, sat[-1])))
    return sat


def window_mean(sat, x0, y0, x1, y1):
    area = (x1 - x0) * (y1 - y0)
    total = sat[y1][x1] - sat[y0][x1] - sat[y1][x0] + sat[y0][x0]
    return (2 * total + area) // (2 * area)


# ---- 3. Grid ---------------------------------------------------------------------------------------------------------------

def edge_profiles(rows):
    """gx[x]: the summed |difference| between column x - 1 and column x over all rows (gx[0] = 0); gy likewise for rows."""
    cols = list(zip(*rows))
    gx = [0] + [sum(map(abs, map(operator.sub, cols[x], cols[x - 1]))) for x in range(1, len(cols))]
    gy = [0] + [sum(map(abs, map(operator.sub, rows[y], rows[y - 1]))) for y in range(1, len(rows))]
    return gx, gy


def detect_axis(profile):
    """-> (pitch, phase, permille, best_permille): the block size and phase whose boundaries carry the largest share of the edge
    energy, accepted only if that share reaches GRID_MIN_PERMILLE (else pitch is None, and the best share seen is reported).
    A cell starts at phase + k * pitch."""
    total = sum(profile)
    if total == 0:
        return None, 0, 0, 0
    scored = []
    for pitch in range(GRID_MIN_PITCH, GRID_MAX_PITCH + 1):
        if len(profile) < 4 * pitch:  # a grid of fewer than four cells says nothing
            break
        for phase in range(pitch):
            scored.append((sum(profile[phase::pitch]) * 1000 // total, pitch, phase))
    if not scored:
        return None, 0, 0, 0
    best = max(s for s, _, _ in scored)
    if best < GRID_MIN_PERMILLE:
        return None, 0, 0, best
    candidates = [(p, -ph, s) for s, p, ph in scored if s >= best - GRID_TIE_PERMILLE]
    p, neg_phase, s = max(candidates)
    return p, -neg_phase, s, best


def sample_grid(rows, px, py, phx, phy):
    """The centre pixel of every whole block: rows of luminance, one per block row."""
    h, w = len(rows), len(rows[0])
    nx, ny = (w - phx) // px, (h - phy) // py
    if nx < 1 or ny < 1:
        raise LogoError("the grid leaves no whole block")
    return [[rows[phy + j * py + py // 2][phx + i * px + px // 2] for i in range(nx)] for j in range(ny)]


# ---- 4. Fallback: box filter over the centre of each cell ------------------------------------------------------------------

def resample_box(sat, w, h, out_w, out_h, num=BOX_NUM, den=BOX_DEN):
    """Mean over the central num/den of each of out_w x out_h cells that tile the w x h picture (integer boundaries)."""
    def span(i, n, size):
        lo = ((2 * den * i + (den - num)) * size) // (2 * den * n)
        hi = ((2 * den * i + (den + num)) * size) // (2 * den * n)
        lo = min(lo, size - 1)
        return lo, max(hi, lo + 1)
    xs = [span(i, out_w, w) for i in range(out_w)]
    ys = [span(j, out_h, h) for j in range(out_h)]
    return [[window_mean(sat, x0, y0, x1, y1) for x0, x1 in xs] for y0, y1 in ys]


# ---- 5. Format ----------------------------------------------------------------------------------------------------------------

def quantise(rows, bits):
    """(codes, reconstruction): the nearest of the format's levels (0, 255 / (2^bits - 1), ...) for each pixel."""
    top = (1 << bits) - 1
    codes = [[(v * top + 127) // 255 for v in row] for row in rows]
    return codes, [[c * 255 // top for c in row] for row in codes]


def sum_sq(a, b):
    return sum(sum((x - y) * (x - y) for x, y in zip(ra, rb)) for ra, rb in zip(a, b))


def rms_x100(sumsq, n):
    """sqrt(sumsq / n) times 100, as an integer (floats are never printed: the generated file must not vary by machine)."""
    return math.isqrt(sumsq * 10000 // n)


def pick_format(native):
    """-> (name, id, bits, codes, rms table): the smallest format that keeps the dither, else L4."""
    n = len(native) * len(native[0])
    table = []
    chosen = None
    for name, fid, bits in FORMATS:
        codes, recon = quantise(native, bits)
        ss = sum_sq(native, recon)
        table.append((name, rms_x100(ss, n)))
        if chosen is None and ss <= FORMAT_MAX_RMS * FORMAT_MAX_RMS * n:
            chosen = (name, fid, bits, codes)
    if chosen is None:
        name, fid, bits = FORMATS[-1]
        chosen = (name, fid, bits, quantise(native, bits)[0])
    return chosen + (table,)


def pick_scale(width, height):
    """The largest integer scale (at most MAX_SCALE) at which a native picture is no larger than MAX_DRAW_W x MAX_DRAW_H shown."""
    scale = min(MAX_SCALE, MAX_DRAW_W // width, MAX_DRAW_H // height)
    if scale < 1:
        raise LogoError("a native picture of %dx%d is over the shown size limit %dx%d" % (width, height, MAX_DRAW_W, MAX_DRAW_H))
    return scale


def fit_native(cw, ch, scale):
    """-> (width, height, capped): the crop fitted into the shown-size box divided by `scale`, aspect kept. capped: the crop has no
    more pixels than that, so it keeps its own and the scale is the caller's to raise (a picture is never enlarged into its native)."""
    bw, bh = MAX_DRAW_W // scale, MAX_DRAW_H // scale
    if cw * bh <= bw * ch:
        h, w = bh, max(1, (cw * bh + ch // 2) // ch)
    else:
        w, h = bw, max(1, (ch * bw + cw // 2) // cw)
    if w >= cw and h >= ch:
        return cw, ch, True
    return w, h, False


def bitmap_bytes(width, height, bits, paletted):
    """Bytes of the blob: the rows padded to a whole word, then the palette of a colour logo."""
    stride = (width * bits + 7) // 8
    return (stride * height + 3) // 4 * 4 + (PALETTE_BYTES if paletted else 0)


def pack(codes, bits):
    """Rows of pixel codes into bytes: pixel 0 in the most significant bits, rows padded to whole bytes. -> (bytes, stride)."""
    per_byte = 8 // bits
    stride = (len(codes[0]) + per_byte - 1) // per_byte
    out = bytearray()
    for row in codes:
        for b in range(stride):
            v = 0
            for k in range(per_byte):
                i = b * per_byte + k
                v = (v << bits) | (row[i] if i < len(row) else 0)
            out.append(v)
    return bytes(out), stride


def screen_error(sat, w, h, native_recon, scale):
    """RMS (x100) of the shown picture against the source: the native bitmap at `scale`, nearest, against the crop
    box-averaged to the same size. What the dither and the resampling cost, whatever the format."""
    out_w, out_h = len(native_recon[0]) * scale, len(native_recon) * scale
    ref = resample_box(sat, w, h, out_w, out_h, 1, 1)
    ss = 0
    for y in range(out_h):
        r, q = ref[y], native_recon[y // scale]
        ss += sum((r[x] - q[x // scale]) ** 2 for x in range(out_w))
    return rms_x100(ss, out_w * out_h)


def tone_error(sat, w, h, recon, block=TONE_BLOCK):
    """RMS (x100) of the mean grey of every block x block patch of the native picture against the same patch of the source:
    the tone, which a dither is there to carry, without asking for its pixels."""
    n_w, n_h = len(recon[0]) // block, len(recon) // block
    if n_w == 0 or n_h == 0:
        raise LogoError("a native picture of %dx%d is smaller than one %dx%d patch" % (len(recon[0]), len(recon), block, block))
    ss = 0
    for j in range(n_h):
        for i in range(n_w):
            mine = sum(sum(recon[j * block + dy][i * block:(i + 1) * block]) for dy in range(block)) // (block * block)
            x0, x1 = i * block * w // len(recon[0]), (i + 1) * block * w // len(recon[0])
            y0, y1 = j * block * h // len(recon), (j + 1) * block * h // len(recon)
            ss += (mine - window_mean(sat, x0, y0, x1, y1)) ** 2
    return rms_x100(ss, n_w * n_h)


# ---- Colour: PALETTED565 -----------------------------------------------------------------------------------------------------

def rgb565(r, g, b):
    """The RGB565 value nearest to an 8-bit colour."""
    return (((r * 31 + 127) // 255) << 11) | (((g * 63 + 127) // 255) << 5) | ((b * 31 + 127) // 255)


def expand565(v):
    """An RGB565 value as 8-bit channels, the top bits repeated (31 -> 255): what the chip is taken to show, and what the preview shows."""
    r5, g6, b5 = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


def spread_of(box):
    """(n, sums, per-channel n * sum of squares - sum squared) of a box of (colour, key, count) elements, in integers."""
    n = sum(c for _, _, c in box)
    sums = [sum(c * v[i] for v, _, c in box) for i in range(3)]
    spread = [n * sum(c * v[i] * v[i] for v, _, c in box) - sums[i] * sums[i] for i in range(3)]
    return n, sums, spread


def median_cut(boxes_wanted, elements):
    """Median cut of (colour, key, count) elements into at most `boxes_wanted` boxes: the box with the largest squared error is cut,
    at the weighted median of the channel that varies most. Ties are broken by position, so the result never varies."""
    boxes = [(spread_of(elements), elements)]
    while len(boxes) < boxes_wanted:
        best = None
        for i, ((n, _, spread), box) in enumerate(boxes):
            score = sum(spread) // n
            if len(box) >= 2 and score > 0 and (best is None or score > best[0]):
                best = (score, i)
        if best is None:
            break
        (n, _, spread), box = boxes.pop(best[1])
        ch = spread.index(max(spread))
        box = sorted(box, key=lambda e: (e[0][ch], e[1]))
        run, cut = 0, 1
        for k, (_, _, c) in enumerate(box):
            run += c
            if 2 * run >= n:
                cut = k + 1
                break
        cut = min(max(cut, 1), len(box) - 1)
        for part in (box[:cut], box[cut:]):
            boxes.append((spread_of(part), part))
    return [box for _, box in boxes]


def build_palette(native):
    """Rows of (r, g, b) -> (entries, index rows, rms x100): at most PALETTE_ENTRIES RGB565 entries, index 0 black (the background),
    and the index of every pixel. A picture of at most 255 colours (after the 565 rounding) keeps all of them, exactly."""
    counts = collections.Counter(rgb565(*px) for row in native for px in row)
    counts.pop(0, None)
    elements = [(expand565(k), k, c) for k, c in sorted(counts.items())]
    entries = [0]
    index_of = {0: 0}
    if len(elements) <= PALETTE_ENTRIES - 1:
        for _, k, _ in elements:
            index_of[k] = len(entries)
            entries.append(k)
    else:
        for box in median_cut(PALETTE_ENTRIES - 1, elements):
            n, sums, _ = spread_of(box)
            entries.append(rgb565(*[(2 * s + n) // (2 * n) for s in sums]))
            for _, k, _ in box:
                index_of[k] = len(entries) - 1
    codes = [[index_of[rgb565(*px)] for px in row] for row in native]
    shown = [expand565(e) for e in entries]
    ss = sum(sum((a - b) ** 2 for a, b in zip(px, shown[i])) for row, crow in zip(native, codes) for px, i in zip(row, crow))
    n = len(native) * len(native[0]) * 3
    return entries, codes, rms_x100(ss, n)


def pack_palette(entries):
    """The palette as the chip reads it: PALETTE_ENTRIES RGB565 values, little endian, the unused ones black."""
    return b"".join(struct.pack("<H", e) for e in entries) + bytes(PALETTE_BYTES - 2 * len(entries))


# ---- The whole conversion ---------------------------------------------------------------------------------------------------

class Result:
    pass


def check_native(width, height):
    """The native picture must hold at least one TONE_BLOCK patch each way (it is what the error measure, and any useful logo, needs)."""
    if width < TONE_BLOCK or height < TONE_BLOCK:
        raise LogoError("the native picture would be %dx%d: under %d pixels a side is no logo" % (width, height, TONE_BLOCK))


def convert(png_bytes, scale=None, tint=None):
    """The decisions and the bytes. `scale`: the integer scale by hand (the native size is the shown size / scale) instead of the finest
    native size that fits MAX_BYTES; used when there is no pixel grid. `tint`: 0xRRGGBB of a grey logo, white by default."""
    res = Result()
    res.sha256 = hashlib.sha256(png_bytes).hexdigest()
    width, height, ctype, raw_rows = decode_png(png_bytes)
    res.source = (width, height, ctype)
    planes = channels(ctype, raw_rows)
    res.max_chroma = max_chroma(planes)
    res.colour = res.max_chroma > CHROMA_THRESHOLD
    if tint is not None and not 0 <= tint <= 0xFFFFFF:
        raise LogoError("the tint 0x%X is not a colour 0xRRGGBB" % tint)
    if res.colour and tint is not None:
        raise LogoError("a tint is for a grey logo (L1, L2, L4); this picture has colour (a pixel with chroma %d, over %d): it is drawn "
                        "in its own colours" % (res.max_chroma, CHROMA_THRESHOLD))
    res.tint = 0xFFFFFF if tint is None else tint
    if res.colour:
        lum = value = [list(map(max, pr, pg, pb)) for pr, pg, pb in zip(*planes)]  # the brightest channel: what the crop and the grid see
    else:
        lum = value = planes[0]
    res.box = crop_box(value)
    crop = cropped(value, res.box)
    cplanes = [crop] * 3 if not res.colour else [cropped(p, res.box) for p in planes]
    cw, ch = len(crop[0]), len(crop)
    res.crop_size = (cw, ch)
    gx, gy = edge_profiles(crop)
    px, phx, sx, bx = detect_axis(gx)
    py, phy, sy, by = detect_axis(gy)
    res.grid_best = (bx, by)
    res.bilevel = 0
    sats = [integral(crop)] if not res.colour else [integral(p) for p in cplanes]

    def finish(native_planes, sc):
        """Format and bytes for one candidate native picture; None when it does not fit MAX_BYTES."""
        nw, nh = len(native_planes[0][0]), len(native_planes[0])
        if res.colour:
            if bitmap_bytes(nw, nh, 8, True) > MAX_BYTES:
                return None
            rgb = [[tuple(px) for px in zip(*rows)] for rows in zip(*native_planes)]
            entries, codes, rms = build_palette(rgb)
            fmt = PALETTED
            table = [("Paletted565", rms)]
            res.recon = [[expand565(entries[i]) for i in row] for row in codes]
            res.palette_used = len(entries)
        else:
            native = native_planes[0]
            res.bilevel = bilevel_permille(native)
            if res.bilevel >= BILEVEL_PERMILLE:
                codes = [[1 if v >= BILEVEL_CUT else 0 for v in row] for row in native]
                recon = [[255 * c for c in row] for row in codes]
                n = nw * nh
                table = [("L1", rms_x100(sum_sq(native, recon), n))]
                fmt = FORMATS[0]
            else:
                name, fid, bits, codes, table = pick_format(native)
                fmt = (name, fid, bits)
            if bitmap_bytes(nw, nh, fmt[2], False) > MAX_BYTES:
                return None
            res.recon = [[c * 255 // ((1 << fmt[2]) - 1) for c in row] for row in codes]
            entries = None
        res.format, res.format_id, res.bits = fmt
        res.formats = table
        res.data, res.stride = pack(codes, fmt[2])
        res.data += bytes((-len(res.data)) % 4)
        res.palette_offset = 0
        if entries is not None:
            res.palette_offset = len(res.data)
            res.data += pack_palette(entries)
        res.width, res.height, res.scale = nw, nh, sc
        res.native = native_planes
        return res

    if px is not None and py is not None:
        res.mode = "grid"
        res.grid = (px, py, phx, phy, sx, sy)
        native_planes = [sample_grid(p, px, py, phx, phy) for p in (cplanes[:1] if not res.colour else cplanes)]
        check_native(len(native_planes[0][0]), len(native_planes[0]))
        if finish(native_planes, pick_scale(len(native_planes[0][0]), len(native_planes[0]))) is None:
            raise LogoError("the native picture of the grid (%dx%d) is over %d bytes" % (len(native_planes[0][0]), len(native_planes[0]), MAX_BYTES))
    else:
        res.mode = "box"
        res.grid = None
        done = None
        for sc in ([scale] if scale is not None else range(1, MAX_SCALE + 1)):
            if sc < 1 or sc > MAX_SCALE:
                raise LogoError("the scale %d is outside 1..%d" % (sc, MAX_SCALE))
            nw, nh, capped = fit_native(cw, ch, sc)
            check_native(nw, nh)
            if capped:
                sc = pick_scale(nw, nh)  # the picture's own pixels, as large as they fit
            planes_n = [resample_box(sat, cw, ch, nw, nh) for sat in sats]
            done = finish(planes_n, sc)
            if done is not None:
                break
        if done is None:
            raise LogoError("no native size of this %dx%d picture fits %d bytes (try a smaller picture or fewer colours)" % (cw, ch, MAX_BYTES))
    if res.colour:
        res.screen_rms = res.tone_rms = None
    else:
        res.screen_rms = screen_error(sats[0], cw, ch, res.recon, res.scale)
        res.tone_rms = tone_error(sats[0], cw, ch, res.recon)
    return res


def describe(res):
    """The decisions, as lines of text (the report, and the comment block of the generated file)."""
    w, h, ctype = res.source
    x0, y0, x1, y1 = res.box
    lines = ["source: %dx%d, colour type %d, sha256 %s" % (w, h, ctype, res.sha256),
             "crop: %d,%d %dx%d (everything above luminance %d)" % (x0, y0, res.crop_size[0], res.crop_size[1], CROP_THRESHOLD)]
    if res.colour:
        lines.append("colour: yes, a pixel of chroma %d (over %d): a 256-entry RGB565 palette, drawn in its own colours" % (res.max_chroma, CHROMA_THRESHOLD))
    else:
        lines.append("colour: no (chroma at most %d, over %d is colour): grey, drawn through COLOR_RGB 0x%06X; bilevel share of the native picture %d permille (%d for L1)"
                     % (res.max_chroma, CHROMA_THRESHOLD, res.tint, res.bilevel, BILEVEL_PERMILLE))
    if res.mode == "grid":
        px, py, phx, phy, sx, sy = res.grid
        lines.append("grid: found, block %dx%d phase %d,%d (%d and %d permille of the edge energy on the boundaries, at least %d)"
                     % (px, py, phx, phy, sx, sy, GRID_MIN_PERMILLE))
        lines.append("native: %dx%d blocks, the centre pixel of each, scale %d" % (res.width, res.height, res.scale))
    else:
        lines.append("grid: none (the best fit holds %d and %d permille of the edge energy, %d needed): not a regular pixel grid"
                     % (res.grid_best[0], res.grid_best[1], GRID_MIN_PERMILLE))
        lines.append("native: %dx%d, box filter over %d/%d of each cell, the finest size that fits %d bytes; scale %d (shown %dx%d px)"
                     % (res.width, res.height, BOX_NUM, BOX_DEN, MAX_BYTES, res.scale, res.width * res.scale, res.height * res.scale))
    lines.append("format: %s (bits %d, BITMAP_LAYOUT %d), stride %d, %d bytes (%d with the padding to a word%s)"
                 % (res.format, res.bits, res.format_id, res.stride, res.stride * res.height, len(res.data),
                    " and the %d-byte palette of %d entries" % (PALETTE_BYTES, res.palette_used) if res.colour else ""))
    if res.colour:
        lines.append("palette: rms %d.%02d (of 255, per channel) against the native image" % (res.formats[0][1] // 100, res.formats[0][1] % 100))
    else:
        lines.append("quantisation rms against the native image: " + ", ".join("%s %d.%02d" % (n, r // 100, r % 100) for n, r in res.formats)
                     + (" (L1 for a bilevel picture, whatever its error)" if res.format == "L1" and res.bilevel >= BILEVEL_PERMILLE
                        else " (a format is kept at %d.00 or less)" % FORMAT_MAX_RMS))
        lines.append("against the source: tone (mean of each %dx%d patch) rms %d.%02d; pixel by pixel at the shown size x%d (%dx%d px) rms %d.%02d,"
                     " which includes the dither's own contrast"
                     % (TONE_BLOCK, TONE_BLOCK, res.tone_rms // 100, res.tone_rms % 100, res.scale, res.width * res.scale,
                        res.height * res.scale, res.screen_rms // 100, res.screen_rms % 100))
    return lines


def emit_cpp(res, source_name, weak=False):
    """The C++ source. `weak`: the kit's default logo, whose boot_logo() any app's own definition replaces."""
    out = ["// Generated by tools/logo_convert.py from %s -- do not edit; %s" % (source_name,
               "a ctest (logo_convert_fresh) regenerates this file and fails when it differs." if weak else "the build regenerates it when the picture or the tool changes."),
           "// %s" % ("The kit's default logo (docs/specs/OS-003-ui-toolkit.md, 003f), the maintainer's O3 logo; GPL-3.0-or-later with the rest of the tree." if weak
                     else "The picture is the app's; this file is only its conversion."),
           "// SPDX-License-Identifier: GPL-3.0-or-later",
           "//"]
    out += ["// " + line for line in describe(res)]
    n = len(res.data)
    fmt = "Paletted565" if res.colour else res.format
    out += ["#include <o3sdk/ui/boot_logo.h>",
            "",
            "namespace o3sdk::ui {",
            "namespace {",
            "",
            "// Uploaded with hal::Display::upload(), whose source must be word aligned: declared on the type (ADR 0011).",
            "template <unsigned N>",
            "struct alignas(4) Bitmap {",
            "  std::uint8_t bytes[N];",
            "};",
            "",
            "// %dx%d %s, stride %d, rows top to bottom, the leftmost pixel in the most significant bits%s." % (res.width, res.height, res.format, res.stride,
                 "; the %d-byte RGB565 palette (little endian) from byte %d" % (PALETTE_BYTES, res.palette_offset) if res.colour else ""),
            "constexpr Bitmap<%d> k_boot_logo = {{" % n]
    for j in range(0, n, 16):
        out.append("    " + ", ".join("0x%02X" % b for b in res.data[j:j + 16]) + ",")
    out += ["}};",
            "static_assert(alignof(Bitmap<%d>) >= 4 && sizeof(Bitmap<%d>) == %d, \"boot logo: word aligned, whole words\");" % (n, n, n),
            "constexpr BootLogo k_logo = {k_boot_logo.bytes, %d, %d, %d, %d, eve::BitmapFormat::%s, %d, %d, 0x%06X};"
            % (n, res.width, res.height, res.stride, fmt, res.scale, res.palette_offset, 0xFFFFFF if res.colour else res.tint),
            "static_assert(boot_logo_valid(k_logo), \"boot logo: over a limit of ui/boot_logo.h (kBootLogoMaxBytes, kBootLogoMaxDrawW, kBootLogoMaxDrawH) or not consistent\");",
            "static_assert(kBootLogoMaxDrawW == %d && kBootLogoMaxDrawH == %d && kBootLogoMaxScale == %d && kBootLogoMaxBytes == %d &&" % (MAX_DRAW_W, MAX_DRAW_H, MAX_SCALE, MAX_BYTES),
            "                  kBootLogoPaletteBytes == %d, \"ui/boot_logo.h and tools/logo_convert.py disagree on the limits: update the header or the tool\");" % PALETTE_BYTES,
            "",
            "}  // namespace",
            "",
            "%sBootLogo boot_logo() noexcept { return k_logo; }" % ("__attribute__((weak)) " if weak else ""),
            "",
            "}  // namespace o3sdk::ui",
            ""]
    return "\n".join(out)


# ---- A PNG encoder for the preview and the self-test ---------------------------------------------------------------------------

def filter_line(ftype, line, prev, bpp):
    """The forward filter of one scanline (the inverse of unfilter), for the self-test."""
    out = bytearray(len(line))
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if ftype == 0:
            pred = 0
        elif ftype == 1:
            pred = a
        elif ftype == 2:
            pred = b
        elif ftype == 3:
            pred = (a + b) >> 1
        else:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
        out[i] = (line[i] - pred) & 255
    return out


def chunk(ctype, body):
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF)


def encode_png(width, height, colour_type, rows, filters=(0,), depth=8, interlace=0, bpp=None):
    """A PNG from rows of samples. `filters` is cycled over the scanlines; `bpp` (bytes a pixel) is the colour type's unless given."""
    bpp = bpp or CHANNELS.get(colour_type, 1)
    raw = bytearray()
    prev = bytearray(width * bpp)
    for y, row in enumerate(rows):
        ftype = filters[y % len(filters)]
        raw.append(ftype)
        raw += filter_line(ftype, bytearray(row), prev, bpp)
        prev = bytearray(row)
    ihdr = struct.pack(">IIBBBBB", width, height, depth, colour_type, 0, 0, interlace)
    return PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")


def write_preview(path, res):
    """The native bitmap at the size it is shown (nearest, as the chip does): grey as a gray PNG, colour as RGB."""
    rows = []
    for row in res.recon:
        if res.colour:
            big = bytearray(v for px in row for _ in range(res.scale) for v in px)
        else:
            big = bytearray(v for v in row for _ in range(res.scale))
        rows += [big] * res.scale
    Path(path).write_bytes(encode_png(res.width * res.scale, res.height * res.scale, 2 if res.colour else 0, rows, filters=(1, 2, 4)))


def test_picture(colour):
    """A small made-up logo as a PNG (for the build's tests and the self-test): a ramp in a box and a disc, on black, 200 x 120.
    Grey: the ramp and the disc are greys. Colour: red to yellow, and a cyan disc: 122 colours in all."""
    rows = []
    for y in range(120):
        row = bytearray()
        for x in range(200):
            px = (0, 0, 0)
            if 20 <= x < 120 and 20 <= y < 100:
                v = 40 + (x - 20) * 2
                px = (v, v, v) if not colour else (255, min(255, 60 + (x - 20) * 2), 0)
            elif (x - 160) ** 2 + (y - 60) ** 2 <= 35 * 35:
                px = (200, 200, 200) if not colour else (0, 220, 220)
            row += bytes(px)
        rows.append(bytes(row))
    return encode_png(200, 120, 2, rows, filters=(1, 2, 4))


# ---- Self-test ---------------------------------------------------------------------------------------------------------------------

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    def expect_error(what, fn):
        try:
            fn()
        except LogoError:
            return
        except Exception as e:  # anything but a clean LogoError is a bug
            failures.append("%s: raised %r, not LogoError" % (what, e))
            return
        failures.append("%s: accepted" % what)

    # -- The decoder: every colour type with every filter, round-trip exact. A small picture with gradients and edges, so a
    # wrong predictor does not cancel out.
    for ctype, chans in sorted(CHANNELS.items()):
        w, h = 13, 9
        rows = [bytes((x * 19 + y * 7 + c * 61 + (x * y) % 11) & 255 for x in range(w) for c in range(chans)) for y in range(h)]
        for ftype in range(5):
            png = encode_png(w, h, ctype, rows, filters=(ftype,))
            dw, dh, dct, drows = decode_png(png)
            check((dw, dh, dct) == (w, h, ctype), "decode type %d filter %d: header" % (ctype, ftype))
            check([bytes(r) for r in drows] == rows, "decode type %d filter %d: pixels" % (ctype, ftype))
        mixed = encode_png(w, h, ctype, rows, filters=(0, 1, 2, 3, 4, 4, 3, 2, 1))
        check([bytes(r) for r in decode_png(mixed)[3]] == rows, "decode type %d: every filter in one image" % ctype)
    # -- The decoder refuses what it cannot or must not take, cleanly.
    good = encode_png(4, 4, 0, [bytes(range(4))] * 4)
    expect_error("bad signature", lambda: decode_png(b"GIF89a" + good[6:]))
    expect_error("truncated", lambda: decode_png(good[:-5]))
    expect_error("empty", lambda: decode_png(b""))
    crc_broken = bytearray(good)
    crc_broken[20] ^= 0xFF  # inside IHDR
    expect_error("bad crc", lambda: decode_png(bytes(crc_broken)))
    expect_error("16-bit", lambda: decode_png(encode_png(4, 4, 0, [bytes(8)] * 4, depth=16, bpp=2)))
    expect_error("palette", lambda: decode_png(encode_png(4, 4, 3, [bytes(4)] * 4)))
    expect_error("interlaced", lambda: decode_png(encode_png(4, 4, 0, [bytes(4)] * 4, interlace=1)))
    expect_error("filter 9", lambda: decode_png(encode_png(4, 4, 0, [bytes(4)] * 4, filters=(9,))))
    short = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 7)) + chunk(b"IEND", b"")
    expect_error("short image data", lambda: decode_png(short))
    bomb = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 10_000_000)) + chunk(b"IEND", b"")
    expect_error("image data bomb", lambda: decode_png(bomb))
    # A picture that would take more than about 64 MiB to decode is refused from its header, before anything is allocated.
    check(MAX_SIDE * MAX_SIDE * 4 <= 64 * 1024 * 1024 and MAX_DECODED_BYTES == 64 * 1024 * 1024, "the decode bound is 64 MiB")
    huge_side = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE + 1, 1, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0")) + chunk(b"IEND", b"")
    expect_error("a side past MAX_SIDE", lambda: decode_png(huge_side))
    huge_area = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE, MAX_SIDE, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 100)) + chunk(b"IEND", b"")
    expect_error("4096 x 4096 RGBA with no data behind it (it is allowed by size, and not a PNG)", lambda: decode_png(huge_area))
    over = PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", MAX_SIDE, MAX_SIDE, 8, 6, 0, 0, 0)) + chunk(b"IEND", b"")
    expect_error("no image data", lambda: decode_png(over))
    check(MAX_SIDE * MAX_SIDE * 4 == MAX_DECODED_BYTES, "4096 x 4096 RGBA is exactly at the bound, and so allowed by size")
    # Ancillary chunks are skipped and alpha is composited on black.
    extra = good[:33] + chunk(b"tEXt", b"Comment\0hello") + good[33:]
    check(decode_png(extra)[0] == 4, "an ancillary chunk is skipped")
    lum = luminance(2, 6, [bytearray([255, 255, 255, 255, 255, 255, 255, 0])])
    check(lum == [[255, 0]], "RGBA composites on black: %r" % lum)
    check(luminance(1, 4, [bytearray([200, 128])]) == [[100]], "gray + alpha composites on black")
    check(luminance(1, 0, [bytearray([77])]) == [[77]], "gray is its own luminance")
    check(luminance(1, 4, [bytearray([255, 255])]) == [[255]], "opaque gray + alpha is unchanged")
    check(luminance(1, 6, [bytearray([255, 255, 255, 255])]) == [[255]], "opaque white stays 255")
    check(luminance(1, 2, [bytearray([255, 255, 255])]) == [[255]], "RGB white is 255")
    check(luminance(1, 6, [bytearray([255, 0, 0, 255])]) == [[76]], "BT.601 red is 76: %r" % (luminance(1, 6, [bytearray([255, 0, 0, 255])]),))

    # -- The crop.
    pic = [[0] * 10 for _ in range(8)]
    pic[2][3] = pic[5][7] = 200
    check(crop_box(pic) == (3, 2, 8, 6), "crop box %r" % (crop_box(pic),))
    expect_error("blank picture", lambda: crop_box([[0] * 4] * 4))

    # -- The grid: a made-up pixel-art picture, scaled up by 7 with a margin and a phase, is found exactly and recovered.
    art = [[(x * 37 + y * 91 + (x * y) % 13 * 29) % 256 for x in range(20)] for y in range(16)]
    art = [[max(v, 30) for v in row] for row in art]   # nothing near black, so the crop keeps every block
    block, margin_x, margin_y = 7, 5, 3
    big = [[0] * (20 * block + 2 * margin_x) for _ in range(16 * block + 2 * margin_y)]
    for y in range(16 * block):
        for x in range(20 * block):
            big[margin_y + y][margin_x + x] = art[y // block][x // block]
    png = encode_png(len(big[0]), len(big), 0, [bytes(r) for r in big], filters=(4,))
    res = convert(png)
    check(res.mode == "grid", "a pixel-art picture upscaled by 7 is recognised as a grid (%s)" % res.mode)
    if res.mode == "grid":
        check(res.grid[:4] == (7, 7, 0, 0), "block 7 phase 0 after the crop: %r" % (res.grid,))
        check(res.native[0] == art, "the native picture is the art, exactly")
        check(res.scale == 16 and (res.width, res.height) == (20, 16), "a grid's scale is the largest that fits: %r" % (res.scale,))
        check(res.box == (margin_x, margin_y, margin_x + 20 * block, margin_y + 16 * block), "crop box of the scaled art %r" % (res.box,))
    # A larger block size within the tie margin wins: a picture whose runs are all 2 cells wide is also a grid of 2 x the block.
    wide = [[row[x // 2] for x in range(40)] for row in art for _ in range(2)]
    pw = encode_png(40 * block, 32 * block, 0, [bytes(v for v in r for _ in range(block)) for r in wide for _ in range(block)])
    rw = convert(pw)
    check(rw.mode == "grid" and rw.grid[0] == 2 * block, "the largest block that fits is taken: %r" % (rw.grid,))
    # A phase: the crop box is at a block boundary by construction, so shift the picture and its content by 3 blocks of garbage-free black.
    # Irregular "pixels" (the block size drifts from 7 to 11) are no grid: the fallback is taken and says so.
    xs = []
    for i in range(30):
        size = 7 + (i * 4) // 30
        xs += [i] * size
    ys = []
    for j in range(30):
        size = 7 + (j * 4) // 30
        ys += [j] * size
    cells = [[max(30, (i * 53 + j * 101 + (i * j) % 7 * 17) % 256) for i in range(30)] for j in range(30)]
    irregular = [bytes(cells[ys[y]][xs[x]] for x in range(len(xs))) for y in range(len(ys))]
    irregular_png = encode_png(len(xs), len(ys), 0, irregular)
    ri = convert(irregular_png)
    check(ri.mode == "box", "a drifting block size is not accepted as a grid (best %r permille)" % (ri.grid_best,))
    check((ri.width, ri.height, ri.scale) == (len(xs), len(ys), 1), "a picture smaller than the panel keeps its pixels, at the largest scale that fits: %r" % ((ri.width, ri.height, ri.scale),))
    rs = convert(irregular_png, scale=4)
    check((rs.width, rs.height, rs.scale) == (105, 105, 4), "a scale by hand fits the native size to the shown size / scale: %r" % ((rs.width, rs.height, rs.scale),))
    # Pictures under four native pixels are no logo: a clean LogoError, never a ZeroDivisionError.
    tiny = encode_png(3, 3, 0, [bytes([200, 100, 200])] * 3)
    expect_error("a 3 x 3 picture", lambda: convert(tiny))
    expect_error("scale 0", lambda: convert(irregular_png, scale=0))
    expect_error("a negative scale", lambda: convert(irregular_png, scale=-5))
    expect_error("a scale over MAX_SCALE", lambda: convert(irregular_png, scale=MAX_SCALE + 1))
    flat40 = convert(encode_png(40, 40, 0, [bytes([200] * 40)] * 40))
    check((flat40.width, flat40.height, flat40.scale) == (40, 40, 10), "a 40 x 40 picture is not enlarged into its native size: x10 (%r)" % ((flat40.width, flat40.height, flat40.scale),))
    expect_error("check_native 3 x 10", lambda: check_native(3, 10))
    expect_error("check_native 10 x 3", lambda: check_native(10, 3))
    check_native(TONE_BLOCK, TONE_BLOCK)
    expect_error("tone_error of a native picture under one patch", lambda: tone_error(integral([[9] * 8 for _ in range(8)]), 8, 8, [[9] * 3 for _ in range(3)]))
    # The edge detector alone: boundaries every 5 from phase 2 and nothing else.
    prof = [0] * 60
    for x in range(2, 60, 5):
        prof[x] = 100
    check(detect_axis(prof)[:3] == (5, 2, 1000), "detect_axis finds pitch 5 phase 2: %r" % (detect_axis(prof),))
    check(detect_axis([10] * 60)[0] is None, "a flat profile has no grid")
    check(detect_axis([0] * 60)[0] is None, "an empty profile has no grid")
    check(detect_axis([5] * 6)[0] is None, "a profile shorter than four blocks has no grid")

    # -- The fallback sampler: exact on a constant picture, and the central half of each cell on a pattern.
    flat = integral([[77] * 40 for _ in range(40)])
    check(resample_box(flat, 40, 40, 10, 10) == [[77] * 10 for _ in range(10)], "box filter keeps a constant picture")
    rows = [[0] * 4 + [255] * 4 for _ in range(8)]  # cells of 4 px across: the central half of cell 0 is 0, of cell 1 is 255
    check(resample_box(integral(rows), 8, 8, 2, 2) == [[0, 255], [0, 255]], "central half of each cell")
    edge = [[0, 0, 255, 255] for _ in range(4)]  # one cell of 4 px: the central half (px 1..3) averages 2 of 3 pixels... integer window
    check(resample_box(integral(edge), 4, 4, 1, 1)[0][0] == 128, "window mean rounds to nearest: %r" % (resample_box(integral(edge), 4, 4, 1, 1),))

    # -- The error measures: zero for a picture against itself, and what a known difference costs.
    check(tone_error(integral([[77] * 40 for _ in range(40)]), 40, 40, [[77] * 8 for _ in range(8)]) == 0, "tone error of an exact copy is 0")
    check(tone_error(integral([[77] * 40 for _ in range(40)]), 40, 40, [[87] * 8 for _ in range(8)]) == 1000, "tone error of a flat +10 is 10.00")
    check(rms_x100(0, 10) == 0 and rms_x100(36 * 10, 10) == 600 and rms_x100(2, 1) == 141, "rms in hundredths: %d" % rms_x100(2, 1))
    check(screen_error(integral([[100] * 20 for _ in range(20)]), 20, 20, [[100] * 4 for _ in range(4)], 2) == 0, "screen error of a flat copy is 0")

    # -- Quantisation and packing: levels, rounding, bit order, row padding.
    codes, recon = quantise([[0, 127, 128, 255]], 1)
    check(codes == [[0, 0, 1, 1]] and recon == [[0, 0, 255, 255]], "L1 levels %r" % (codes,))
    codes, recon = quantise([[0, 42, 43, 85, 128, 212, 213, 255]], 2)
    check(codes == [[0, 0, 1, 1, 2, 2, 3, 3]] and recon == [[0, 0, 85, 85, 170, 170, 255, 255]], "L2 levels %r %r" % (codes, recon))
    codes, recon = quantise([[0, 8, 9, 17, 255]], 4)
    check(codes == [[0, 0, 1, 1, 15]] and recon == [[0, 0, 17, 17, 255]], "L4 levels %r" % (codes,))
    check(pack([[1, 0, 1, 1, 0, 0, 0, 1, 1]], 1) == (bytes([0b10110001, 0b10000000]), 2), "L1 packs MSB first and pads the row")
    check(pack([[1, 2, 3, 0, 3]], 2) == (bytes([0b01101100, 0b11000000]), 2), "L2 packs two bits a pixel, first pixel on top")
    check(pack([[0xA, 0x5, 0xF]], 4) == (bytes([0xA5, 0xF0]), 2), "L4 packs the first pixel in the high nibble")
    check(pack([[1, 0], [0, 1]], 1)[0] == bytes([0x80, 0x40]), "one padded byte a row")
    # The format choice: the smallest that keeps the picture.
    two = [[0, 255] * 8 for _ in range(8)]
    check(pick_format(two)[0] == "L1", "a black and white picture is L1")
    four = [[0, 85, 170, 255] * 4 for _ in range(8)]
    check(pick_format(four)[0] == "L2", "a four-tone picture is L2")
    ramp = [[(x * 255) // 15 for x in range(16)] for _ in range(8)]
    check(pick_format(ramp)[0] == "L4", "sixteen tones are L4")
    smooth = [[(x * 255) // 63 for x in range(64)] for _ in range(8)]
    check(pick_format(smooth)[0] == "L4", "more tones than L4 holds: L4 is the last resort, not an error")
    check(pick_scale(124, 124) == 3 and pick_scale(100, 100) == 4 and pick_scale(190, 190) == 2, "scales 124 -> 3, 100 -> 4, 190 -> 2")
    check(pick_scale(800, 420) == 1 and pick_scale(4, 4) == MAX_SCALE and pick_scale(300, 50) == 2, "scale 1 at the largest shown size, MAX_SCALE at the smallest")
    expect_error("a native picture wider than the panel", lambda: pick_scale(801, 10))
    expect_error("a native picture taller than the shown limit", lambda: pick_scale(10, 421))
    check(fit_native(920, 565, 1) == (684, 420, False), "the O3 crop at scale 1: %r" % (fit_native(920, 565, 1),))
    check(fit_native(1494, 681, 2) == (400, 182, False), "a wide crop is width limited: %r" % (fit_native(1494, 681, 2),))
    check(fit_native(300, 200, 1) == (300, 200, True), "a crop smaller than the box is capped at its own size")
    check(bitmap_bytes(684, 420, 1, False) == 36120 and bitmap_bytes(10, 10, 8, True) == 100 + PALETTE_BYTES, "bytes of a blob: padded rows, then the palette")

    # -- Colour: a picture with chroma is PALETTED565, a grey one is not, a tint is for grey only.
    grey_png, colour_png = test_picture(False), test_picture(True)
    rg, rc = convert(grey_png), convert(colour_png)
    check(not rg.colour and rg.format in ("L1", "L2", "L4") and rg.palette_offset == 0, "the grey test picture is a grey format (%s)" % rg.format)
    check(rc.colour and rc.format == "Paletted565" and rc.format_id == 14 and rc.bits == 8, "the colour test picture is PALETTED565")
    check((rc.width, rc.height, rc.scale) == (160, 80, 5) or rc.scale >= 4, "the colour logo is as big as it fits: %r" % ((rc.width, rc.height, rc.scale),))
    bitmap = (rc.stride * rc.height + 3) // 4 * 4
    check(rc.stride == rc.width and rc.palette_offset == bitmap and len(rc.data) == bitmap + PALETTE_BYTES, "colour blob: index rows, padding, then the 512-byte palette")
    check(rc.data[bitmap:bitmap + 2] == b"\0\0", "palette entry 0 is black, the background")
    check(rc.palette_used == 122 + 1 or rc.palette_used <= PALETTE_ENTRIES, "palette entries used: %r" % (rc.palette_used,))
    pal = [struct.unpack_from("<H", rc.data, bitmap + 2 * i)[0] for i in range(PALETTE_ENTRIES)]
    rebuilt = [[expand565(pal[rc.data[y * rc.stride + x]]) for x in range(rc.width)] for y in range(rc.height)]
    check(rebuilt == rc.recon, "the palette and the indices in the blob rebuild the preview picture exactly")
    nat = [[tuple(px) for px in zip(*rows)] for rows in zip(*rc.native)]
    check(all(rebuilt[y][x] == expand565(rgb565(*nat[y][x])) for y in range(rc.height) for x in range(rc.width)), "a picture of at most 255 colours keeps them all (to RGB565)")
    check(rc.formats[0][1] == rms_x100(sum((a - b) ** 2 for ra, rb in zip(nat, rebuilt) for pa, pb in zip(ra, rb) for a, b in zip(pa, pb)), rc.width * rc.height * 3), "palette rms is measured")
    expect_error("a tint on a colour logo", lambda: convert(colour_png, tint=0xFF8000))
    expect_error("a tint out of range", lambda: convert(grey_png, tint=0x1000000))
    expect_error("a negative tint", lambda: convert(grey_png, tint=-1))
    check(convert(grey_png, tint=0x00FF00).tint == 0x00FF00 and rg.tint == 0xFFFFFF, "a grey logo's tint is its own, white by default")
    emitted = emit_cpp(rc, "colour.png")
    check("eve::BitmapFormat::Paletted565, %d, %d, 0xFFFFFF};" % (rc.scale, rc.palette_offset) in emitted and ("Bitmap<%d>" % len(rc.data)) in emitted, "emit: the palette is in the blob and the descriptor names it")
    # Chroma noise below CHROMA_THRESHOLD (a grey picture saved once as a JPEG) is not colour; one pixel above it is.
    noisy = [bytes(v for x in range(40) for v in ((200, 200 + (x * 5) % (CHROMA_THRESHOLD + 1), 200) if x else (200, 200, 200))) for _ in range(40)]
    check(not convert(encode_png(40, 40, 2, noisy)).colour, "chroma up to the threshold is noise: still grey")
    noisy[10] = bytes(noisy[10][:30]) + bytes((200, 200 + CHROMA_THRESHOLD + 1, 200)) + bytes(noisy[10][33:])
    check(convert(encode_png(40, 40, 2, noisy)).colour, "one pixel over the threshold is a colour logo")
    # More than 255 colours: median cut, at most 256 entries, black at 0, the same result twice, a bounded error.
    ramp_rows = [bytes(v for x in range(160) for v in ((x * 7 + y * 3) % 256, (x * 5 + y * 11) % 256, (x * 3 + y * 13 + 40) % 256)) for y in range(100)]
    ramp_png = encode_png(160, 100, 2, ramp_rows, filters=(4,))
    r1, r2 = convert(ramp_png), convert(ramp_png)
    check(r1.colour and r1.data == r2.data and r1.palette_used == PALETTE_ENTRIES, "median cut fills 256 entries, deterministically (%r)" % (r1.palette_used,))
    check(r1.formats[0][1] < 1500, "median cut keeps the error of a busy picture under 15 grey levels: %d" % r1.formats[0][1])
    check(len(r1.data) <= MAX_BYTES and r1.data[(r1.stride * r1.height + 3) // 4 * 4:][:2] == b"\0\0", "colour blob within MAX_BYTES, entry 0 black")
    # The byte limit counts the palette: a colour picture whose native size would be over MAX_BYTES is made smaller (the next scale), not refused.
    bigc = convert(encode_png(400, 240, 2, [bytes(v for x in range(400) for v in ((x * 7 + y) % 256, (x + y * 5) % 256, (x * 3 + 90) % 256)) for y in range(240)]))
    check(bigc.colour and len(bigc.data) <= MAX_BYTES and bigc.scale >= 2 and bigc.width * bigc.scale <= MAX_DRAW_W and bigc.height * bigc.scale <= MAX_DRAW_H,
          "a colour picture over MAX_BYTES at scale 1 takes a coarser native: %r" % ((bigc.width, bigc.height, bigc.scale, len(bigc.data)),))
    check(bitmap_bytes(250, 260, 8, True) <= MAX_BYTES < bitmap_bytes(250, 261, 8, True), "the limit counts the palette (ui/boot_logo.h, tests/boot_screen_test.cpp)")
    # -- Bilevel: dots stay one bit (L1) at the finest native size; a tonal picture does not.
    dots = [bytes(255 if (x % 12 - 6) ** 2 + (y % 12 - 6) ** 2 <= 16 else 0 for x in range(240)) for y in range(120)]
    rd = convert(encode_png(240, 120, 0, dots))
    check(rd.format == "L1" and rd.bilevel >= BILEVEL_PERMILLE, "a picture of white dots on black is L1 (%s, %d permille)" % (rd.format, rd.bilevel))
    tones = [bytes((x * 255) // 239 for x in range(240)) for _ in range(120)]
    rt = convert(encode_png(240, 120, 0, tones))
    check(rt.format in ("L2", "L4") and rt.bilevel < BILEVEL_PERMILLE, "a ramp is not bilevel (%s, %d permille)" % (rt.format, rt.bilevel))
    check(bilevel_permille([[0, 255, 20, 240, 100, 130]]) == 666, "bilevel share counts black and white only")
    check(max_chroma(channels(0, [bytearray([5, 200])])) == 0 and max_chroma(channels(2, [bytearray([10, 20, 70, 0, 0, 0])])) == 60, "max chroma")
    # -- The test pictures the build's tests use are valid logos of both kinds, and the tool writes them.
    with tempfile.TemporaryDirectory() as tmp:
        for kind in ("grey", "colour"):
            pic = Path(tmp) / (kind + ".png")
            check(main_args(["--make-test-png", str(pic), "--kind", kind]) == 0 and pic.read_bytes() == test_picture(kind == "colour"), "--make-test-png %s" % kind)

    # -- The generated file: deterministic, aligned, tied to the header, and the staleness check sees a change.
    text = emit_cpp(res, "logo.png")
    check(text == emit_cpp(convert(png), "logo.png"), "the output is deterministic")
    n = len(res.data)
    check(text.count("struct alignas(4) Bitmap") == 1 and ("constexpr Bitmap<%d> k_boot_logo" % n) in text, "emit: one aligned array")
    check("static_assert(alignof(Bitmap<%d>) >= 4" % n in text, "emit: alignment asserted")
    check("constexpr BootLogo k_logo = {k_boot_logo.bytes, %d, %d, %d, %d, eve::BitmapFormat::%s, %d, 0, 0xFFFFFF};" % (n, res.width, res.height, res.stride, res.format, res.scale) in text,
          "emit: the descriptor carries the size, stride, format, scale and the white tint")
    check("static_assert(boot_logo_valid(k_logo)" in text and "kBootLogoMaxBytes == %d" % MAX_BYTES in text, "emit: the limits are asserted")
    check("__attribute__((weak))" not in text and "__attribute__((weak)) BootLogo boot_logo()" in emit_cpp(res, "logo.png", weak=True), "emit: weak only for the kit's default")
    check("0xFF8000" in emit_cpp(convert(png, tint=0xFF8000), "logo.png"), "emit: a tint is the descriptor's colour")
    check(text.endswith("}  // namespace o3sdk::ui\n") and "\r" not in text, "emit: LF line ends, final newline")
    check(len(res.data) % 4 == 0, "the array is whole words")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "logo.png"
        out = Path(tmp) / "data.cpp"
        src.write_bytes(png)
        check(run_check(src, out, quiet=True) == 1, "--check: a missing file is stale")
        check(main_args(["--png", str(src), "--out", str(out)]) == 0 and out.exists(), "--out writes")
        check(run_check(src, out, quiet=True) == 0, "--check: a fresh file passes")
        out.write_text(out.read_text().replace("0x", "0X", 1))
        check(run_check(src, out, quiet=True) == 1, "--check: an edited file is stale")
        main_args(["--png", str(src), "--out", str(out)])
        src.write_bytes(encode_png(len(big[0]), len(big), 0, [bytes(r) for r in big], filters=(0,)))  # same pixels, other bytes
        check(run_check(src, out, quiet=True) == 1, "--check: a changed source (same picture, other file) is stale: the hash is part of the output")
        prev_png = Path(tmp) / "preview.png"
        src.write_bytes(png)
        check(main_args(["--png", str(src), "--preview", str(prev_png)]) == 0 and decode_png(prev_png.read_bytes())[0] == res.width * res.scale,
              "--preview writes a PNG of the screen size")

    print("logo_convert self-test: %s" % ("FAILED" if failures else "ok"))
    for f in failures:
        print("  " + f)
    return 1 if failures else 0


# ---- Command line ---------------------------------------------------------------------------------------------------------------------

def hex_colour(text):
    return int(text, 0)


def run_check(png_path, cpp_path, quiet=False, weak=False, scale=None, tint=None):
    res = convert(Path(png_path).read_bytes(), scale, tint)
    fresh = emit_cpp(res, Path(png_path).name, weak)
    try:
        committed = Path(cpp_path).read_text()
    except OSError:
        if not quiet:
            print("logo_convert: %s is missing: run tools/logo_convert.py --out" % cpp_path, file=sys.stderr)
        return 1
    if committed != fresh:
        if not quiet:
            print("logo_convert: %s is stale (the converter or %s changed): regenerate it with\n"
                  "  python3 -I tools/logo_convert.py --png %s%s --out %s" % (cpp_path, png_path, png_path, " --weak" if weak else "", cpp_path),
                  file=sys.stderr)
        return 1
    return 0


def main_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--png", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", type=Path)
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--scale", type=int, default=None, help="the integer scale by hand (default: the finest native size that fits)")
    parser.add_argument("--tint", type=hex_colour, default=None, help="0xRRGGBB, the colour a grey logo is drawn in (default white)")
    parser.add_argument("--weak", action="store_true", help="the kit's default: boot_logo() is a weak symbol an app's logo replaces")
    parser.add_argument("--make-test-png", type=Path, help="write a small made-up logo (--kind grey|colour) for the build's tests and exit")
    parser.add_argument("--kind", choices=("grey", "colour"), default="grey")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.make_test_png:
        args.make_test_png.parent.mkdir(parents=True, exist_ok=True)
        args.make_test_png.write_bytes(test_picture(args.kind == "colour"))
        return 0
    if not args.png or not (args.out or args.check or args.report or args.preview):
        parser.error("--png and one of --out, --check, --report, --preview are required")
    try:
        if args.check:
            return run_check(args.png, args.check, weak=args.weak, scale=args.scale, tint=args.tint)
        res = convert(args.png.read_bytes(), args.scale, args.tint)
    except (LogoError, OSError) as e:
        print("logo_convert: %s: %s" % (args.png, e), file=sys.stderr)
        return 1
    if args.report:
        print("\n".join(describe(res)))
    if args.preview:
        write_preview(args.preview, res)
    if args.out:
        text = emit_cpp(res, args.png.name, args.weak)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        if not args.out.exists() or args.out.read_text() != text:  # keep the mtime when nothing changed
            args.out.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main_args(sys.argv[1:]))
