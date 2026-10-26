---
name: fw-gate-check
description: Static release gate for an app's firmware image (.ptf) before it goes to the maintainer for a unit test - reproducible build twice, image checks, flash configuration field, link sanity, sizes, manifest. Never flashes. Use before handing any image to the maintainer.
---

# Gate: static validation of a candidate image

**One command does the mechanical steps** (1 to 5 and 7: identity, two fresh builds, identical HEX, the image ctests, link sanity, no doubles, the host ctest, sizes, SHA-256, the manifest): `python3 -I tools/gate.py <app> --host-build build-host` (or `--skip-host`, which the manifest then flags; `--arm`/`ARM_GNU_ROOT`; in an app repository the kit is `$O3SDK_DIR` or `../o3-sdk`). It writes `build-gate-a/CANDIDATE.md` with the verdict CANDIDATE, BENCH BUILD (a tree is dirty or has no commit) or FAIL. You still do step 6 (what the maintainer is told about the card) and step 8, the second pass over the manifest against the raw logs (`gate-*.log`). The steps below are what it does, for when you need one by hand.

This produces evidence for the maintainer's decision to update a unit. It never flashes, never writes to a card, and never names a candidate so that it could be mistaken for an official file (no name starting with `Polyend`). A file on a card is not proof of an update. Paths below are `build-gate-a/apps/<app>/<app>.{elf,hex,map,ptf}` for an app of o3sdk's own tree; an app in its own repository has `build-gate-a/app/<app>.{elf,hex,map,ptf}`, its kit is `../o3-sdk`, and the checks are the same.

1. **Identity.** `git status --short` and `git rev-parse HEAD` in each tree the image comes from: the app's and, for an app of another repository, the kit's (`../o3-sdk`). The image carries both ids, from git (`platform/mk66/tools/gen_build_info.cmake`; `O3SDK_IMAGE_LABEL` is `<app label> / o3sdk <kit label>`, or `o3sdk <label>` for o3sdk's own apps; `strings` on the `.bin` or `.elf` shows it): `unknown` with no commit, `<hash>-dirty` with changed tracked files. An image with either, in either tree, is a bench build (a clean app with a `-dirty` kit is not a release build) the tester cannot trace to a source: say so in the manifest, and ask the maintainer to commit first for a real candidate (you do not commit). Record the app, the options (`O3SDK_RATE_44100`) and the toolchain (`<toolchain>/bin/arm-none-eabi-gcc --version | head -1`).
2. **Reproducible build twice** into two fresh directories with identical options, then compare:
   ```sh
   for d in a b; do
     nice -n 10 cmake -S . -B build-gate-$d -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain dir>
     nice -n 10 cmake --build build-gate-$d -j4
   done
   cmp build-gate-a/apps/<app>/<app>.hex build-gate-b/apps/<app>/<app>.hex
   ```
   Both exit 0 without warnings, the HEX files identical (no date or host name goes into the image).
3. **Image checks.** The build runs them as ctests behind `.hex`/`.ptf`; list them with `ctest --test-dir build-gate-a -N`, run them again and read the verdicts. Required: every record checksum valid; every address at or below `0x000FFFFF`; one contiguous run from 0; initial SP at the top of SRAM_U; a Thumb reset vector inside the image; the flash configuration field `0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff` (a wrong value can lock the MCU for good: any difference is an automatic FAIL); the SRAM_L hot code calling nothing in flash (`check_ramfunc.py`); only `boot::dma_init()` writing the DMA arbitration (`check_dma_owner.py`); and, for an image that will be installed over USB, `<app>_usb` (the USB vector and the console are in the image: without them the unit cannot be read or updated remotely). By hand: `python3 -I platform/mk66/tools/check_image.py flashconfig build-gate-a/apps/<app>/<app>.hex --expect ffffffffffffffffffffffffdef9ffff`, and `check_image.py check` with the arguments `ctest -V -R <name>` shows. A failing HEX is deleted by `--delete-on-fail`; never keep one.
4. **Link sanity.** No "region overflowed", no undefined references, no warnings, no `__aeabi_d*` (skill `fw-build`). Record the region usage and `arm-none-eabi-size -A <app>.elf`.
5. **Tests.** The full host `ctest --test-dir build-host` exits 0 on this code (not scoped). Do not re-run what already passed on identical code; name the earlier run.
6. **Hand-over facts.** The deliverable is `<app>.ptf` (the checked HEX under that name). The maintainer puts it alone in `Firmware/` of the SD card and uses the hidden reset button; the bootloader flashes the first `.ptf` it finds (track3r-os `docs/manual/install.md`): the manifest lists the `.ptf` files already on the card if you were told. You do not copy to a card: the guard hook refuses it unless the maintainer opened one card for you.
7. **Manifest.** Write `build-gate-a/CANDIDATE.md` (untracked): app, git state, options, toolchain, SHA-256 of `<app>.hex` and `<app>.ptf` (`sha256sum`), sizes, the test results, every check above with PASS/FAIL, what the host cannot prove (what the unit must show: boot screen, input, LEDs, sound, the CPU % in the header) and the explicit line "Not flashed. A unit test needs the maintainer's go."
8. **Second pass.** Ask `firmware-reviewer` (or re-read deliberately) to check the manifest against the raw outputs. Report the candidate SHA-256 and stop. The maintainer decides whether to install.

Never: SWD or debug-probe flashing, an improvised uploader, a bypassed confirmation, a write to a card, an image in git.
