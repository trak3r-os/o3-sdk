# platform/mk66/tools: host tools and static checks

Python 3 standard library only, run as `python3 -I <tool>` (nothing from the current directory is imported). Every tool has a
`--self-test` that needs no hardware; the ctests of `platform/mk66/CMakeLists.txt` run them (`mk66_o3*_selftest`, `mk66_writers*`,
`mk66_usb_image_selftest`, plus `mk66_image_selftest` and friends for the build checks).

## The USB console tools

`o3console.py`, `o3shot.py`, `o3audio.py`, `o3hil.py` and `o3disk.py` are track3r-os's, copied from its `platform/mk66/tools` and
identical but for the rename `o3os` -> `o3sdk` (mock replies and example names in the self-tests, an option name in a help text) and
two edits in `o3hil.py`'s self-test, which looked for the tracker's tree (see "Differences", below). The unit keeps the USB identity of
track3r-os (`16D0:0E9F`, product "Track3r"), so the tools find it the same way: `/dev/serial/by-id/*Track3r*` for the console and an
ALSA card that names "track3r" for the audio. What an image calls itself is in `version`: track3r-os says `Track3r o3os, build <id>`,
an o3sdk image `Track3r o3sdk, build <id>`; no tool parses that line (`--expect-version TEXT` is a substring test), so either works.

| Tool | Use |
|---|---|
| `o3console.py` | one console command per argument; `--put` / `--update` (the firmware path); `--shot out.png` |
| `o3shot.py` | decode the `shot` stream to a PNG; `compare A.png B.png` with tolerances (the FT812 is not the simulator's rasteriser) |
| `o3audio.py` | `record` the unit's USB audio and measure each channel (level, silence, clipping, pitch) |
| `o3hil.py` | JSON scenarios in `hil/` driving key, pad, enc, state, leds, shot and audio steps |
| `o3disk.py` | the PC side of the USB disk scenarios (`o3hil` `disk` steps); o3sdk has no disk set yet, kept so the port stays whole |

Never run them against a unit from an agent session; the test session at the unit does (`docs/hw/`). Each one's header says what it
sends and which exit status means what.

### Putting and starting a firmware image

```sh
python3 -I platform/mk66/tools/o3console.py --put build-mk66/apps/hello/hello.ptf --name o3sdk-hello.ptf
python3 -I platform/mk66/tools/o3console.py --update o3sdk-hello.ptf --crc <crc> --yes-update --expect-version <text>
```

`--put` first runs `check_image.py` on the file (`--expect-sp 0x20030000 --ramfunc-l 0x1FFF0000:0xC000`, a window that accepts
track3r-os's images and o3sdk's) and prints the unit's `ok <name> <bytes> <crc> image <length>`; the CRC is the one `--update` wants.
`--update` arms and starts the hand-over, waits for the unit to leave and come back, and prints `version`. Exit status of both: 0 done,
1 usage, 2 no prompt, 3 wrong card (`--expect-volume`), 4 the unit refused or failed, 5 it did not come back, 6 `--expect-version` not in
the reply, 7 the bootloader did not take over.

**A name that is already on the card is refused**: `--put` ends with `ERR already exists` and exit 4 (proven on a unit with track3r-os
v1.0.0, 2026-10-25); the unit never overwrites a firmware file. The way out is not to put it again but to `--update` the file that is
already there, with its CRC:

```sh
python3 -I -c 'import sys, zlib; print("%08X" % (zlib.crc32(open(sys.argv[1], "rb").read()) & 0xFFFFFFFF))' build-mk66/apps/hello/hello.ptf
python3 -I platform/mk66/tools/o3console.py --update o3sdk-hello.ptf --crc <that value> --yes-update
```

(the file on the card must be the one the CRC was taken from; after a rebuild, `--put` under a new `--name`, or `fw rm <name>` first.)

### The screen

`o3console.py --shot out.png` is 800 x 480. `hello_sim`'s PNGs are 800 x 697: the screen with the 48 pads drawn below it, so
`o3shot.py compare` and `o3hil.py` refuse them (the size differs). Crop to the top 800 x 480 first:

```sh
python3 -I -c 'import importlib.util as u, sys; s = u.spec_from_file_location("o3shot", "platform/mk66/tools/o3shot.py"); m = u.module_from_spec(s); s.loader.exec_module(m); w, h, rgb = m.read_png(sys.argv[1]); m.write_png(sys.argv[2], 800, 480, rgb[:800 * 480 * 3])' <sim out>/01_boot.png <golden dir>/01_boot.png
```

### Audio

The image boots in track3r-os's default AUDIO set (profile 8). The master is on channels 7-8; 1-6 are silent (kept for an app's stems):

```sh
python3 -I platform/mk66/tools/o3audio.py record --seconds 3 --channel 7 --not-silent
```

### Scenarios (`hil/`)

`python3 -I o3hil.py list | validate | run <scenario> --port /dev/ttyACM0 [--audio-device auto] [--golden-dir <dir>] [--out <dir>]`;
the step kinds are in the header of `o3hil.py`. o3sdk's console has `version stage cpu usb log state key pad enc leds shot fw` (and the rest of its `help`), so a scenario
may use the step kinds `cmd keys pad enc wait state leds shot audio`; `mix voices midi card disk` need commands it does not have.

- `hello_smoke.json`: version, log, state (`app=HELLO screen=PLAY`), the pad levels, the boot screen against the simulator's, `key f1`,
  `enc 3`, `pad 12` down and up with its LED following, a second shot. The compare step is `"soft"` until a unit has shown that the
  picture agrees. **Its reference is `<golden-dir>/01_boot.png`**: the top 800 x 480 of the `01_boot` snapshot of
  `apps/hello/scenarios/tour.txt` (crop as above; a PNG is a binary and does not go into git). Run with `--golden-dir <dir>`; the
  header's CPU readout (right of x = 700) is ignored because the simulator shows none.
- `hello_audio.json` (needs `--audio-device`): `pad 12` held, channel 7 not silent and unclipped, channels 1-6 silent (soft).
- `hello_power.json`: the Power switch with no finger: `power` (the pin in the three pulls), an injected hold (`power press`: the line fills) and tap (the hint), then `power off dry`, which ends the run
  in a restart (read the record with `o3console.py power` after the boot). The real hold, the VLPR off and its wake are by hand (the scenario's last SAY).

## The static checks

| Check | What it proves |
|---|---|
| `check_writers.py check [--root <o3sdk dir>]` (`mk66_writers`) | only `platform/mk66/update/src/fat_firmware_store.cpp` names FatFs write calls and write open flags, only `platform/mk66/storage/src/fat_storage.cpp` names `disk_write` / `disk_ioctl`, only `board.h` and `update/src/fw_host_mk66.cpp` name `kBootloaderRequest`; tests and `third_party` are not scanned; a file that is absent is not an error |
| `check_usb_image.py check <elf> --nm <arm-none-eabi-nm>` (`<app>_usb`) | vector 16 + 53 (USB0) is `USB0_IRQHandler`, not the `Default_Handler` alias; `g_bdt` is 512-aligned; "Track3r" is in the loadable image. An image built without the console fails it |
| `check_image.py`, `check_ramfunc.py`, `check_dma_owner.py`, `gen_ld.py` | the image, the SRAM_L hot code, the eDMA owner and the linker script (M2) |
| `fw_hex_cross.py` | the device's HEX validator against `check_image.py` on a corpus (the update slice's) |

## Differences from track3r-os's tools

Besides the rename `o3os` -> `o3sdk` (`o3console.py`: the example name in the header and the self-test's file names and mock replies;
`o3audio.py`: the unit-name hint list; `o3hil.py`: the same strings, a help text, `-DO3SDK_BENCH`), `o3hil.py`'s self-test changes in
two places, both because o3sdk has no tracker tree around it: the button names are compared with `kit/app/src/panel.cpp` (`kNames`),
not with `app/include/o3os/app/buttons.h`; and the check of the tracker's `save_test_card` scenario runs only when that file is in
`hil/`. `o3hil.py`'s `--sim` default still names track3r-os's `build-os-host/sim/o3sim`, and `--sim <hello_sim>` would hand the compare the
800 x 697 picture: give `--golden-dir` with cropped PNGs until the simulator can write the screen alone.
