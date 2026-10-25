---
name: fw-build
description: Build o3sdk for the host or the MK66 and check warnings, tests, simulator runs, archive/image sizes and software doubles. Use whenever you need to know if the tree compiles, how many bytes a change costs, or before reporting any change.
---

# Build and test

There is no CI script: these commands are the whole pipeline. Everything runs under `nice -n 10` and `-j4` at most. Nothing here flashes or copies an image anywhere.

```sh
# host: the kit, units, apps, the simulator and the host tests
nice -n 10 cmake -S . -B build-host
nice -n 10 cmake --build build-host -j4
nice -n 10 ctest --test-dir build-host --output-on-failure          # -R <name> for one test, -L mk66 for the platform's pure parts

# an app in the simulator: PNG snapshots of the screen and the pads, the audio as a WAV
mkdir -p /tmp/hello && build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello

# ASan + UBSan (host only), in its own directory
nice -n 10 cmake -S . -B build-host-asan -DO3SDK_SANITIZE=ON && nice -n 10 cmake --build build-host-asan -j4 && ctest --test-dir build-host-asan --output-on-failure

# cross: the kit, units and apps as Cortex-M4F libraries; with the M2 image block, <app>.elf .hex .map .ptf
nice -n 10 cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT="$PWD/../track3r-os/.toolchains/"arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi
nice -n 10 cmake --build build-mk66 -j4
```

- Directories: `build-host`, `build-mk66`; while milestone M2 is open each slice uses `build-m2-<slice>-host` and `build-m2-<slice>-mk66` and builds only its targets (`docs/M2-PLAN.md`, "Ownership"). All `build*/` are untracked. `build-host/compile_commands.json` feeds the syntax-check hook.
- Warnings are errors (`O3SDK_WERROR` is ON: `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion -Werror`). Also grep the configure and build output for `warning`; none is acceptable. The host build type is Debug, the cross build MinSizeRel.
- Tests: one executable `o3sdk_tests`, one ctest per `tests/*_test.cpp` (globbed: rerun the configure after adding one). Platform tests are in `platform/mk66/tests` with the label `mk66`. Name a test in the report by its ctest name.
- Layering is link-time: a header of a layer you did not list in `DEPENDS` does not resolve. Every public header also compiles alone (`<layer>_hdrcheck` targets, part of the default build).

## Reading the result
- Exit 0 means built, warning-free and every test passed. Quote the summary line ("100% tests passed, 0 tests failed out of N") and the simulator's summary line (frames drawn and refused, the largest display list, audio blocks, dropped events and commands; exit 1 = scenario failed or a frame did not fit).
- Sizes: the cross build prints `size -t` after each archive. For an image, `arm-none-eabi-size -A <app>.elf` and the link's region summary; the limits are strict (a "region overflowed" error is a budget failure). Quote bytes and the delta against the previous build or the figure in the plan.
- Software doubles must not appear: `arm-none-eabi-nm -u <archive> | grep __aeabi_d` over the kit, unit and platform archives prints nothing; on a linked `.elf`, `arm-none-eabi-nm <elf> | grep ' __aeabi_d'` likewise (the toolchain's `bin/` is the directory in `-DARM_GNU_ROOT`).
- A host test is not hardware verification: name the unit check that remains.

## When it fails
Read the first error, not the last. For a size problem, the top consumers: `arm-none-eabi-nm --size-sort -C --radix=d <elf or archive> | tail -40`, and the `.map` next to the image. A break in a file owned by another M2 slice is theirs: report it, do not fix it.
