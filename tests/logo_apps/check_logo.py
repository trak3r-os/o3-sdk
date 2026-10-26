#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs an app's simulator on a boot scenario and checks the logo in the picture it writes (tests/logo_apps/CMakeLists.txt).

    check_logo.py --sim <exe> --scenario <boot.txt> --out <dir> --expect colour|tint|default [--tint 0xRRGGBB]

colour: the palette's colours are on the screen (the test picture's cyan disc and its red-to-yellow ramp), not just white.
tint:   every lit pixel of the logo is the tint scaled by the pixel's level, and the brightest one is close to the tint itself.
default: the kit's O3 logo: white on black, in a 684 x 420 box centred on the panel.
The logo's rows are those above --rows (the line under the logo starts below it: 450 for the O3 logo, which is 420 px tall); the pads below the screen are not looked at.
"""
import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

PANEL_W = 800


def load_converter():
    spec = importlib.util.spec_from_file_location("logo_convert", Path(__file__).resolve().parents[2] / "tools" / "logo_convert.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", required=True)
    ap.add_argument("--scenario", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--expect", choices=("colour", "tint", "default"), required=True)
    ap.add_argument("--tint", type=lambda t: int(t, 0), default=0xFFFFFF)
    ap.add_argument("--rows", type=int, default=450, help="look at the rows above this one: the line under the logo starts below it")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    png = out / "01_boot.png"
    png.unlink(missing_ok=True)
    run = subprocess.run([args.sim, "--scenario", args.scenario, "--out", str(out)], capture_output=True, text=True)
    if run.returncode != 0 or not png.exists():
        print("the simulator failed (%d):\n%s%s" % (run.returncode, run.stdout, run.stderr))
        return 1
    lc = load_converter()
    width, height, ctype, rows = lc.decode_png(png.read_bytes())
    if (width, ctype) != (PANEL_W, 6):
        print("unexpected picture: %dx%d type %d" % (width, height, ctype))
        return 1
    pixels = {}  # (r, g, b) -> count, over the logo's rows
    lit_x0, lit_x1 = PANEL_W, -1
    for y in range(args.rows):
        row = rows[y]
        for x in range(width):
            px = (row[4 * x], row[4 * x + 1], row[4 * x + 2])
            if px != (0, 0, 0):
                pixels[px] = pixels.get(px, 0) + 1
                lit_x0, lit_x1 = min(lit_x0, x), max(lit_x1, x)
    lit = sum(pixels.values())
    if lit == 0:
        print("nothing is drawn above the line")
        return 1
    if args.expect == "colour":
        cyan = lc.expand565(lc.rgb565(0, 220, 220))
        chromatic = sum(n for (r, g, b), n in pixels.items() if max(r, g, b) - min(r, g, b) > 64)
        if pixels.get(cyan, 0) < 500 or chromatic < lit // 2 or len(pixels) < 30:
            print("the palette's colours are not on the screen: cyan %r x%d, chromatic %d of %d lit, %d colours" % (cyan, pixels.get(cyan, 0), chromatic, lit, len(pixels)))
            return 1
    elif args.expect == "tint":
        tr, tg, tb = (args.tint >> 16) & 255, (args.tint >> 8) & 255, args.tint & 255
        top = max(pixels, key=lambda px: px[0] * tr + px[1] * tg + px[2] * tb)
        if top[0] * 4 < tr * 3 or top[1] * 4 < tg * 3 or tg * top[0] * 4 < top[1] * tr * 3:  # the brightest pixel is near the full tint
            print("the tint %r is not on the screen: the brightest pixel is %r" % ((tr, tg, tb), top))
            return 1
        for (r, g, b), n in pixels.items():
            if abs(r * tg - g * tr) > 255 or abs(r * tb - b * tr) > 255 or abs(g * tb - b * tg) > 255:  # on the line from black to the tint
                print("pixel %r (x%d) is not a shade of the tint %r" % ((r, g, b), n, (tr, tg, tb)))
                return 1
    else:
        if set(pixels) != {(255, 255, 255)}:
            print("the default logo is not pure white on black: %r" % (sorted(pixels)[:5],))
            return 1
        if not (58 <= lit_x0 <= 70 and 730 <= lit_x1 <= 742):
            print("the default logo is not 684 px wide and centred: lit columns %d..%d" % (lit_x0, lit_x1))
            return 1
        if not 40000 <= lit <= 120000:
            print("the default logo lights %d pixels, expected about 80000" % lit)
            return 1
    print("check_logo %s: ok (%d lit pixels, %d colours, columns %d..%d)" % (args.expect, lit, len(pixels), lit_x0, lit_x1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
