# FILLRATE: how much can the FT812 draw on one scan line?

A test image for the unit, not an instrument. o3-303's skinned panel (Variant A, "obsidian") asks the FT812 for about 2,034 bitmap and
2,174 filled pixels on its busiest line (o3-303 docs/M3B-PLAN.md S4). The chip has no frame buffer: it walks the whole display list for every scan line, with about
2,112 clocks of its 60 MHz clock a line (PCLK 30 MHz, HCYCLE 1056). Its fill rate per pixel is not in anything the kit cites and it has
no underrun counter, so an overrun only shows as broken lines. This image draws a band of rows (y 200-263) whose every row carries a
load you choose, and lets you raise it until the lines break.

The simulator does not model the scan-line limit (`apps/fillrate/scenarios/tour.txt` only proves that every frame builds and fits).
Nothing here is a result until it has run on the unit.

## The image

`<mk66 build dir>/apps/fillrate/fillrate.ptf` (also `.hex`, `.bin`, `.elf`), built as in `CLAUDE.md`
(`-DO3SDK_PLATFORM=mk66`, target `fillrate`). It loads like any app image. No sound, pads dark, no unit attached to the engine.
At start it uploads two test bitmaps to RAM_G (`ui/plot/ram_g_map.h`, the start of the app region `kRamGApp`: an 800 x 96 L8 and an 800 x 96 L4, 112.5 KiB,
at the end of the spare region); the screen says "UPLOADING THE TEST BITMAPS n/29" for about half a second, then the band appears.

## The screen

```
L8 BITMAP NEAREST                      kind of layer
K 3   2400 PX/LINE                     layers stacked on every row of the band, and K x 800 pixels a line
BMP 2400 + FILL 0 PX   3 SPRITES/LINE   CELL 800 PX
LIST 542/2048 WORDS   BALLAST 0 WORDS   GRID ON       display-list words (the soft-key row included), ballast, grid
-------------------------------------------------   a rule on its own row, above the band
|  the band: rows 200-263, K layers, white vertical grid lines every 40 px   |
-------------------------------------------------   a rule on its own row, below the band
```

Every layer is 800 px wide and covers all 64 rows, so the load of a row is the sum of the layers. The layers are translucent
(COLOR_RGB + COLOR_A, source-over), and bitmap layer j reads the source from row j on, so no two layers fetch the same bytes on a line.
A red line under the numbers says when the list is full (the app then draws fewer layers or less ballast than asked, and shows what it drew).

## Controls

| Control | Effect |
|---|---|
| Encoder | K, 0..32. SHIFT + turn: steps of 4 |
| F1 | L8 bitmap, NEAREST, tinted |
| F2 | L8 bitmap, BILINEAR, scaled by 4/3 (BITMAP_TRANSFORM 0.75) |
| F3 | L4 bitmap, NEAREST, tinted |
| F4 | filled RECT, translucent |
| F5 | MIX: bitmaps (L8, L4 and BILINEAR in turn) and RECTs alternating, bitmap first. K = 5 is 2,400 bitmap + 1,600 fill pixels, K = 6 is 2,400 + 2,400: the skin's line is 2,034 + 2,174 |
| F6 | cell width 800, 400, 200, 100, 50 px (SHIFT: back). A layer is 800 / W sprites side by side: the pixels a line carries stay K x 800, the sprites and list words grow. The skin is many small sprites |
| F7 | ballast 0, 256 .. 1,280 display-list words (SHIFT: back): 1 px rectangles in the background colour below the band, nothing to see, only list for the chip to walk (the skin's frame is about 870 words) |
| F8 | K = 0 |
| PLAY | the vertical grid on and off (the two rules stay) |

## What a broken line looks like

The grid and the rules are the reference: 1 px lines in the brightest colour, drawn over the layers. With every line finished you see
straight, unbroken vertical lines through a steady band, and the diagonal stripes of the bitmaps continuous from row to row. A line the
chip did not finish is not described in anything we have; expect any of: grid lines with gaps, a jag or a step at some row, the right
part of a row shifted, stale or repeated, a row of noise or black, the band flickering from frame to frame, rows outside the band
(the text) breaking too once the list or the load gets very large. Note where along the line the damage starts (the x of the
first jagged grid line: the chip got that far in the line) and whether it is stable or flickers. If the whole picture goes wrong, turn K
down; if it does not recover, power-cycle. A chip re-initialisation (the screen blanks and returns) loses the test bitmaps and the app does not re-send them: restart the image.
Before each pass, set K = 1 and check that the band shows the test stripes; a blank or garbled band at K = 1 means the bitmaps are gone,
not that the line broke.

## Procedure

1. Boot, wait for the band (K = 0: nothing in it, the rules and grid clean). Photo.
2. Pass 1, full-width layers (F6 at 800, F7 at 0): for each kind F1..F5, from K = 0 raise K one step at a time and watch each K for
   a few seconds. Write down the largest K with clean lines and the first K with broken ones. Photo of the whole screen at the largest clean K
   and at the next one, with the text legible: the photo is the record of kind, K and pixels a line.
3. Pass 2, small sprites (F6 to 100, then 50): repeat for F1 (L8), F3 (L4), F4 (RECT) and F5 (MIX). If the limit in pixels falls with the
   cell width, the cost is per sprite, not per pixel.
4. Pass 3, a list as long as the skin's (F7 to 768, then 1,280, cell 800): repeat F5 (MIX). If the limit falls, the cost of walking the
   list is part of the line time. Rows of text breaking before the band does says the same.
5. Return to K = 0 and photograph once more: the screen must be clean again.

Write down, for each (kind, cell width, ballast): the largest clean K and its PX/LINE, the first broken K and what it looked like, where
along the line it began, the LIST number, and the photos. Tentative reading: chip clocks a pixel are about 2,112 divided by the pixels a
line of the largest clean K carry (a bound from above on the cost of the layers); the real limit lies between the clean and the broken K.
Suggested design margin: keep the skin's busiest line (MIX K = 5 or 6) at most 75 % of the largest clean MIX pixels a line, on the same
cell width and list length the skin will have.

## Simulator

```sh
build-fillrate-host/apps/fillrate/fillrate_sim --scenario apps/fillrate/scenarios/tour.txt --out <dir>     # twelve PNGs
ctest --test-dir build-fillrate-host -R fillrate                                                           # the band's arithmetic, the upload, every kind fits
```

The simulator refuses BILINEAR with a BITMAP_TRANSFORM, so the host build draws F2 with NEAREST in the same words (the label says so);
the unit draws BILINEAR.
