---
name: fw-gate-check
description: Static release gate for an app's firmware image (.ptf) before it goes to the maintainer for a unit test - reproducible build twice, image checks, flash configuration field, link sanity, sizes, manifest. Never flashes. Use before handing any image to the maintainer.
---

# Gate: static validation of a candidate image

This produces evidence for the maintainer's decision to update a unit. It never flashes, never writes to a card, and never names a candidate so that it could be mistaken for an official file (no name starting with `Polyend`). A file on a card is not proof of an update. The image exists once the M2 image block lands (`docs/M2-PLAN.md`, "The image"); paths below are `build-gate-a/apps/<app>/<app>.{elf,hex,map,ptf}`.

1. **Identity.** `git status --short` and `git rev-parse HEAD`. The build id comes from git (`platform/mk66/tools/gen_build_info.cmake`): `unknown` with no commit, `<hash>-dirty` with changed tracked files. An image with either is a bench build the tester cannot trace to a source: say so in the manifest, and ask the maintainer to commit first for a real candidate (you do not commit). Record the app, the options (`O3SDK_RATE_44100`) and the toolchain (`<toolchain>/bin/arm-none-eabi-gcc --version | head -1`).
2. **Reproducible build twice** into two fresh directories with identical options, then compare:
   ```sh
   for d in a b; do
     nice -n 10 cmake -S . -B build-gate-$d -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain dir>
     nice -n 10 cmake --build build-gate-$d -j4
   done
   cmp build-gate-a/apps/<app>/<app>.hex build-gate-b/apps/<app>/<app>.hex
   ```
   Both exit 0 without warnings, the HEX files identical (no date or host name goes into the image).
3. **Image checks.** The build runs them as ctests behind `.hex`/`.ptf`; list them with `ctest --test-dir build-gate-a -N`, run them again and read the verdicts. Required: every record checksum valid; every address at or below `0x000FFFFF`; one contiguous run from 0; initial SP at the top of SRAM_U; a Thumb reset vector inside the image; the flash configuration field `0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff` (a wrong value can lock the MCU for good: any difference is an automatic FAIL); the SRAM_L hot code calling nothing in flash (`check_ramfunc.py`); only `boot::dma_init()` writing the DMA arbitration (`check_dma_owner.py`). By hand: `python3 -I platform/mk66/tools/check_image.py flashconfig build-gate-a/apps/<app>/<app>.hex --expect ffffffffffffffffffffffffdef9ffff`, and `check_image.py check` with the arguments `ctest -V -R <name>` shows. A failing HEX is deleted by `--delete-on-fail`; never keep one.
4. **Link sanity.** No "region overflowed", no undefined references, no warnings, no `__aeabi_d*` (skill `fw-build`). Record the region usage and `arm-none-eabi-size -A <app>.elf`.
5. **Tests.** The full host `ctest --test-dir build-host` exits 0 on this code (not scoped). Do not re-run what already passed on identical code; name the earlier run.
6. **Hand-over facts.** The deliverable is `<app>.ptf` (the checked HEX under that name). The maintainer puts it alone in `Firmware/` of the SD card and uses the hidden reset button; the bootloader flashes the first `.ptf` it finds (track3r-os `docs/manual/install.md`): the manifest lists the `.ptf` files already on the card if you were told. You do not copy to a card: the guard hook refuses it unless the maintainer opened one card for you.
7. **Manifest.** Write `build-gate-a/CANDIDATE.md` (untracked): app, git state, options, toolchain, SHA-256 of `<app>.hex` and `<app>.ptf` (`sha256sum`), sizes, the test results, every check above with PASS/FAIL, what the host cannot prove (what the unit must show: boot screen, input, LEDs, sound, the CPU % in the header) and the explicit line "Not flashed. A unit test needs the maintainer's go."
8. **Second pass.** Ask `firmware-reviewer` (or re-read deliberately) to check the manifest against the raw outputs. Report the candidate SHA-256 and stop. The maintainer decides whether to install.

Never: SWD or debug-probe flashing, an improvised uploader, a bypassed confirmation, a write to a card, an image in git.
