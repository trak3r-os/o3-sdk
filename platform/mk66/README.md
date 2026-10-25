# platform/mk66: the MK66 platform and the firmware image

The MK66FX1M0 platform of o3sdk (milestone M2), ported from track3r-os `platform/mk66`, whose README is the long account of every
hardware fact and its source. This file covers the boot path and the image; `docs/M2-PLAN.md` has the slices, their interfaces and
who owns what. The display (`display/`), input (`input/`) and audio (`audio/`) drivers say their own facts in their headers.

## Build

```sh
cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir>
cmake --build build-mk66 -j4 && ctest --test-dir build-mk66 -L mk66 --output-on-failure
```

Every app gets an image: `o3sdk_app(<name> ...)` (cmake/O3sdkTargets.cmake) calls `o3sdk_mk66_image()` (CMakeLists.txt), which
links `<name>.elf` from `platform_mk66_image` (src/: reset sequence, fault path, clock, I2C master, code cache, crash record, `main.cpp`, or `main_skeleton.cpp` in the skeleton), the
drivers' object libraries `platform_mk66_{display,input,audio}_hw`, the pure libraries and the app library (whole archive: it
defines `o3sdk_app()`).

| Option | Default | |
|---|---|---|
| `O3SDK_MK66_SKELETON` | OFF | link without the three `_hw` object libraries: the reset sequence and the image pipeline alone |
| `O3SDK_MK66_SDRAM_FULL_TEST` | ON | test every SDRAM cell at boot (a few seconds, by estimate); the wiring test always runs |
| `O3SDK_CODE_CACHE` | ON | the LMEM code cache on in the reset sequence (`lmem.h`), as proven in track3r-os |
| `O3SDK_SDRAM_ICODE_ALIAS` | ON | after the SDRAM test the controller also answers at `0x0800_0000` (`sdramc_config.h` `kCm0WithAlias`), as proven; nothing in the kit reads through the alias yet |
| `O3SDK_MK66_AUDIO_TEST` | OFF | the audio bring-up image: the device plays the 1 kHz test tone (`audio/test_tone.h`) instead of the app's engine |
| `O3SDK_DISPLAY_ROTATE` | 1 | panel orientation, `REG_ROTATE` 0..7 (cache STRING, set in `display/CMakeLists.txt`); 1 is upright on the maintainer's unit, 0 is upside down |

## Outputs (in the app's binary directory, e.g. `build-mk66/apps/hello/`)

| File | |
|---|---|
| `<name>.elf`, `<name>.map` | the linked image and its map (`--print-memory-usage` prints the region table at link time) |
| `<name>.hex` | Intel HEX, written only behind `check_ramfunc.py` and `check_image.py` (a HEX that fails is deleted) |
| `<name>.ptf` | a copy of the checked HEX under the name the Tracker's bootloader takes; removed before each check, copied only after it passed |
| `<name>.bin` | raw binary from address 0 |
| `<name>.image-check.json` | the checks' results and the image facts |

ctests (label `mk66`): `<name>_image` (record checksums, every address at or below `0x000FFFFF`, one contiguous run from 0, initial
SP `0x20030000`, a Thumb reset vector inside the image, every vector in flash or in the SRAM_L code region), `<name>_flashconfig`
(bytes `0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff`, the test's own literal: a wrong value can lock the MCU for good),
`<name>_ramfunc` (the SRAM_L code calls nothing outside itself, no veneer), `mk66_ld` (the generated linker script against
`mem_map.h`), `mk66_noinit_guard` (an initialised noinit, DMA-ring or `O3SDK_BIG` object does not compile), `mk66_dma_owner` (only
`boot::dma_init()` writes DMA_CR and the DCHPRIs), `mk66_build_info` (`gen_build_info.cmake`: the kit's and the app's identity, "unknown",
"-dirty", the version tag), and the tools' self-tests. The host build runs the self-tests, `mk66_dma_owner`, `mk66_build_info`,
`mk66_ld` and the base suite (`mk66_base_tests`).

## Memory map (`include/o3sdk/platform/mk66/mem_map.h` is the single source; `tools/gen_ld.py` generates `mk66.ld` from it)

| Region | Use |
|---|---|
| FLASH 1 MiB at 0 | vector table, flash configuration field at `0x400`, code and constants, then the load images of `.ramfunc_l` and `.data`, one run with no hole |
| SRAM_L 64 KiB at `0x1FFF0000` | `.ramfunc_l` (the audio ISRs and pipeline; `O3SDK_RAMFUNC_L`) within `kRamfuncLBudget` = 32 KiB, linker-asserted; the rest free |
| SRAM_U 192 KiB at `0x20000000` | `.noinit` (the 128-byte crash block first, at `0x20000000`), the 512-byte fault stack, `.dma_rings`, `.data`, `.bss`, at least 32 KiB free (asserted), the 24 KiB main stack at the top (`0x20030000`) |
| SDRAM 16 MiB at `0x70000000` | `.sdram_bss` first: every `O3SDK_BIG` object (kit/core placement.h), NOLOAD, cleared word by word after the SDRAM test and before the constructors; then the app's arena, `__sdram_arena_start` to `__sdram_arena_end` (`sdram_arena()` in `platform.h`), neither placed nor cleared |

`O3SDK_BIG` puts its objects in the input section `.bss.sdram_bss` (NOBITS), which the `.sdram_bss` output section collects (listed
before `.bss`, whose `.bss.*` pattern would take it into SRAM_U; `gen_ld.py check` verifies the order). An object starts at zero
because the reset sequence clears the section; the NOLOAD section keeps no contents, so a non-zero initialiser would be lost, and
GCC refuses it in a `.bss.*` section ("only zero initializers are allowed in section"; `mk66_noinit_guard` proves it on the
cross compiler). A constructor still runs and is the way to give such an object a value. track3r-os's tap memory, module slab,
`.noinit_sdram`/`SdramBox` window and eleven fixed SDRAM regions were the tracker's and are gone.

Linker usage of `hello` (2026-10-25, the fresh ARM GNU 15.3.1 builds of the M2 gate; the integrator's `main.cpp` and all three
`_hw` libraries in the default image; percentages are of the region sizes in `mem_map.h`):

| Region | Skeleton (`O3SDK_MK66_SKELETON=ON`) | Default (all three `_hw`) |
|---|---|---|
| FLASH | 37,016 B | 69,540 B (6.63%) |
| SRAM_L | 24 B | 1,476 B |
| SRAM_U | 1,528 B | 51,080 B (25.98%) |
| SDRAM | 705,600 B | 705,600 B |

## Reset sequence (`src/startup.cpp`, `src/boot.cpp`)

Interrupts off; watchdog unlocked and disabled; FPU on; the quiet pins (amplifier muted first, `board.h` `kQuietPins`); crash
context reset; `.data` and `.ramfunc_l` copied, `.bss` cleared (`.noinit` left alone); code cache on; reset cause and crash record
captured; DWT; clocks (16 MHz crystal, HSRUN, PLL: 180 MHz core, 90 MHz bus); NVIC priorities (hal/irq.h via `irq_map.h`), fault
handlers, SysTick 1 kHz, interrupts on; SDRAM power, controller, JEDEC sequence, wiring and full tests, ICode alias; `.sdram_bss`
cleared; `boot::dma_init()` (DMA_CR and every DCHPRI from `resources.h`); constructors; `main`. Every hardware value is
track3r-os's, unchanged. Every wait is bounded; a failure stops with a blink code.

What `main` may use: `system_clock()`, `boot_info()`, `boot_crash_record()`, `clear_crash_record()`, `set_stage()`, `set_module()`,
`panic()`, `sdram_arena()` (`platform.h`); `hal::cycles()` (DWT); and from `src/internal.h` `boot::delay_ms()`/`delay_us()`,
`boot::set_idle_hook()` (`idle_hook.h`), `boot::led_set()`, `boot::led_from_sram_l()`, `boot::play_code()`, `boot::fail()`,
`i2c::init()`/`bus()`/`write_reg()`/`read_reg()`, `cache::*`. `main.cpp` is the composition root (the order is in `docs/M2-PLAN.md`). `main_skeleton.cpp`, the skeleton image's main, plays a
crash record's code, clears it, then runs the heartbeat in a `wfi` loop.

## Faults and blink codes

A fault (HardFault, MemManage, BusFault, UsageFault, NMI, an interrupt with no handler) or `panic()` drops the amplifier key, writes
a 64-byte CRC-checked crash record into the `.noinit` block and resets (SYSRESETREQ); the next boot finds it (`crash_record.h`).
The status LED is `PTC19`, active low (`blink.h`):

| Pattern | Meaning |
|---|---|
| on, steady, after power-up | the SDRAM tests are running |
| 100 ms pulse every second | heartbeat: `main` alive; driven from SRAM_L, so it also proves `.ramfunc_l` was copied |
| 2 flashes, repeated | clock failure (crystal, HSRUN or PLL) |
| 3 flashes, repeated | SDRAM wiring or full test failed |
| 6 flashes, twice, then heartbeat | a crash record from the previous run |
| 4, 5, 8, 9 | display codes (no chip, polled path, stalled, app failure) |
| 10, 11, 12 | audio codes (bring-up failed, fault seen, over budget) |
| 13 | a key scanner did not answer at boot |
| dark | the image never reached `early_pins`, or the LED is not where `board.h` says |

7 is reserved (track3r-os's fault drill; M2 builds no drill image).

## Verified here, and what only the unit can show

Verified by the build and the host: warning-free compile with the kit's strict flags; the pure parts' tests (`mk66_base_tests`); the
linker script generated from and checked against `mem_map.h`; the image checks above; no `__aeabi_d*` in the image or any archive;
the reset sequence's loops stay loops (disassembly). Not verified by anything here: that the image boots at all, the clock tree, the
SDRAM bring-up and test time, the crash record surviving a reset, the code cache, `.sdram_bss` being zero when the constructors run,
and the drivers' behaviour on the hardware (the host tests run their logic against fakes). Those are the maintainer's unit test.

## Getting an image onto the unit

Never by us, and never by a debug probe or an uploader: only the maintainer installs an image, by the card. The `.ptf` goes alone in
`Firmware/` on the card and the hidden reset button flashes it (track3r-os `docs/manual/install.md`: the bootloader flashes the first
`.ptf` in `Firmware/`, and the official update menu lists only names starting with `PolyendTracker`). USB, the SD card driver and the
firmware update over USB are milestone M2b.
