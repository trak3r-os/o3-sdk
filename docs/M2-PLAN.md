# M2: the MK66 platform (plan, interfaces and ownership)

Status: built and statically gated, 2026-10-25; criterion 3 (the maintainer's unit test) has not happened, so M2 is not done.
The working brief for the parallel slices of milestone M2; it becomes `platform/mk66/README.md` when M2 ends.

## Goal

An app built with `o3sdk_app()` becomes a firmware image the Tracker's bootloader flashes, and HELLO runs on the unit.

Done when:

1. `cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain> && cmake --build build-mk66` builds, warning-free,
   `build-mk66/apps/hello/hello.elf`, `hello.hex`, `hello.map` and `hello.ptf` (the checked HEX under a `.ptf` name), and the static
   image checks pass as ctests: record checksums, every address at or below `0x000FFFFF`, one contiguous run from 0, the initial SP,
   a Thumb reset vector inside the image, the flash configuration field `0x400..0x40F` exactly
   `ffffffffffffffffffffffffdef9ffff` (a wrong value can lock the MCU for good), the SRAM_L hot code calling nothing in flash.
2. The host build stays green: `ctest --test-dir build-host` (kit, sim, and the `mk66` label: the platform's pure parts).
3. On the unit (the maintainer's test, not ours): the kit boot screen, then HELLO; pads, buttons and encoder drive it; the pad LEDs
   follow; sound on the outputs; the header shows the render's CPU %. A crash leaves a record and a blink code, as in track3r-os.

The toolchain: `../track3r-os/.toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi` (ARM GNU 15.3).

## Not in M2

USB (CDC console, MIDI, USB audio), the SD card, the firmware update over USB, the Power-switch shutdown, MIDI DIN, the FM radio,
audio recording, the CPU bench, the HIL harness. They are not optional: the kit exposes every capability of the hardware
(`docs/FRAMEWORK.md` section 10), and they follow as milestones M2b (USB console and update, SD, MIDI, Power) and M2c (audio in,
FM radio), after the unit has run the M2 image. Keep that in mind when you cut: drop the code, not the seams a later driver needs
(the I2C bus shared with the radio, the codec's input select, the DMA channel and interrupt tables). Until then an image
reaches the unit only by the card: the `.ptf` alone in `Firmware/` and the hidden reset button (track3r-os `docs/manual/install.md`).

## Where the code comes from

`platform/mk66` here is a copy of track3r-os `platform/mk66` (and, for the audio pipeline, `audio/{pipeline,slot_ring,status,hot}`),
renamed `o3os` -> `o3sdk` and `O3OS` -> `O3SDK`, minus usb/, storage/, update/, save/, power/, bench, si4703, midi_din, capture and
the tracker's composition root. The copy does not build yet: each slice makes its part build, cuts what the kit does not need,
and keeps what it keeps close to the original (it is proven on the unit). The original tree stays the reference: read
`../track3r-os/platform/mk66/` (README.md is the long account of every hardware fact) and its `src/app_main.cpp`
(how the tracker wires display, input, LEDs and audio) whenever the copy is unclear. Never edit the track3r-os tree.

Namespace `o3sdk::platform::mk66`, headers `<o3sdk/platform/mk66/...>`, macros `O3SDK_*`. Comments that cite `OS-…` or `ADR …`
refer to track3r-os documents and may stay.

## The image (architecture)

- One image per app: on mk66, `o3sdk_app(<name> ...)` (cmake/O3sdkTargets.cmake) also links the executable `<name>` (`<name>.elf`)
  from the platform's objects (`platform_mk66_image` and the `_hw` object libraries) and the app library (whole archive: it defines
  `o3sdk::app::App& o3sdk_app() noexcept`), then `.hex`, `.bin`, `.ptf` behind the checks.
- `src/main.cpp` is the composition root (the integrator writes the final one; the boot slice leaves a minimal one that boots and
  blinks, so the image links and the checks run):
  ```
  reset sequence (startup.cpp, boot.cpp): clocks 180 MHz, SDRAM up and tested, `.data` copied, `.bss` and `.sdram_bss` zeroed,
                                           `.ramfunc_l` copied, then the static constructors, then main()
  main: crash record of the last run -> display bring-up -> input init -> static dsp::Engine, app::Runtime(o3sdk_app(), engine,
        Platform{display, input, &leds, system_clock(), cpu_meter = true}) -> rt.start() -> audio_device().start(rt.block_callback())
        (muted) -> loop { wfi; input service; rt.poll(); LED flush; audio beat; the outputs opened once after a clean run; blink }
  ```
- The engine's cycle counter is `hal::cycles` (the DWT counter): `engine.set_cycle_counter(...)` gives the header's CPU %.
- The render line (the software interrupt, priority kRender) calls `Runtime::block_callback()` once per 64-frame block. In M2 the
  kit's `Engine::render` and the units run from flash with the LMEM code cache on; only the audio ISRs and the pipeline are in
  SRAM_L. Moving the engine's hot loop to SRAM_L is a later, measured change.

## Memory

| Region | Use |
|---|---|
| FLASH 1 MiB at 0 | vectors, the flash configuration field at `0x400`, code, constants, the load images of `.ramfunc_l` and `.data` |
| SRAM_L 64 KiB at `0x1FFF0000` | `.ramfunc_l` (audio ISRs, pipeline; later the engine's hot loop), the rest free for hot data later. track3r-os's tap memory and module slab are gone |
| SRAM_U 192 KiB at `0x20000000` | `.noinit` (crash block first), fault stack, `.dma_rings`, `.data`, `.bss`, the main stack at the top |
| SDRAM 16 MiB at `0x70000000` | `.sdram_bss` first: every `O3SDK_BIG` object (kit/core/include/o3sdk/core/placement.h), NOLOAD, zeroed by the reset sequence after the SDRAM test and before the constructors; a linker assert on overflow. The rest is the app's arena (symbols for its start and end), for samples in M4 |

track3r-os's eleven fixed SDRAM regions (sample arena, delay, reverb, project...) were the tracker's; the kit keeps the SDRAM as
`.sdram_bss` plus one free arena. HELLO puts 705,600 bytes of delay lines in `.sdram_bss` today.

## Interfaces between the slices

What the integrator's `main.cpp` calls. Keep these names unless there is a reason; say so in the report if you change one.

| Slice | Exposes |
|---|---|
| boot | `system_clock()` -> `const hal::Clock&` (SysTick, 1 ms); `hal::cycles()`; `boot::delay_ms()`; the crash record (`boot_crash_record()`, `clear_crash_record()`, `set_module()`); blink codes and the status LED; the idle hook a blocking wait calls |
| display | `display::device::bring_up()` -> `{up, dma}`; `display::device::display()` -> `hal::Display&`; `hold_dark()`; whatever the main loop must call to service it |
| input | `input::device::init()` (true when every scanner answered); `input::device::input()` -> `hal::Input&`; `input::device::leds()` -> `hal::Leds&`; the per-pass service call. Button codes are kit/app/include/o3sdk/app/panel.h (`Button::F1` = 0 ... `Button::Right` = 32), pads 0..47 row by row from the top left, the encoder id 0 with signed detents |
| audio | `audio_device()` -> `hal::audio::Device&` (`start(callback)` muted, `unmute()`, `mute()`); a status snapshot (boundaries, underruns, hardware faults, render cycles); the main-loop beat and the starvation guard; `OutputGate` (open the outputs after a clean run) |

## Ownership (other slices edit other files at the same time)

| Slice | Owns |
|---|---|
| boot | `platform/mk66/CMakeLists.txt` (the image block, `platform_mk66_image`), `mk66.ld.in`, `tools/`, `src/` (`main.cpp` until the integrator takes it), `include/`, the base test suite, `cmake/O3sdkTargets.cmake` (the mk66 branch of `o3sdk_app`), the toolchain files |
| display + input | `platform/mk66/display/`, `platform/mk66/input/`, `tests/mk66_display_*`, `tests/mk66_input_*` |
| audio | `platform/mk66/audio/`, `tests/mk66_audio_config_test.cpp`, `mk66_sgtl5000_test.cpp`, `mk66_output_gate_test.cpp`, `audio_*_test.cpp` |
| harness | `.claude/` only |
| integrator | `src/main.cpp` at the end, `docs/`, the root CMake and `tests/`, any change to `kit/` |

A slice that needs a change in a file it does not own (for example a declaration in `src/internal.h`, a row of `resources.h`)
makes the smallest additive change, keeps it to its own lines, and lists it in its report.

Each slice builds in its own directories, `build-m2-<slice>-host` and `build-m2-<slice>-mk66`, and builds only its targets:
another slice's half-done work may break the rest of the tree for a while (do not fix it, do not wait for it).

| Slice | Host targets | mk66 targets |
|---|---|---|
| boot | `platform_mk66_base`, `platform_mk66_base_hdrcheck`, `mk66_base_tests` | the image (`hello`) once the others link; before that its own objects |
| display + input | `platform_mk66_display(_hdrcheck)`, `platform_mk66_input(_hdrcheck)`, `mk66_display_tests`, `mk66_input_tests` | the same plus `platform_mk66_display_hw`, `platform_mk66_input_hw` |
| audio | `platform_mk66_audio(_hdrcheck)`, `mk66_audio_tests` | the same plus `platform_mk66_audio_hw` |

```sh
nice -n 10 cmake -S . -B build-m2-audio-host
nice -n 10 cmake --build build-m2-audio-host -j4 --target mk66_audio_tests platform_mk66_audio_hdrcheck
ctest --test-dir build-m2-audio-host -L mk66 -R 'audio' --output-on-failure
nice -n 10 cmake -S . -B build-m2-audio-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<toolchain>
nice -n 10 cmake --build build-m2-audio-mk66 -j4 --target platform_mk66_audio platform_mk66_audio_hw
```

## Rules (CLAUDE.md applies; these are the ones that bite here)

- Never flash, never run an uploader or a debug probe, never write to a card. The image is a build artefact.
- Never add code or data from the official firmware. The hardware facts come from track3r-os, which states their sources.
- No heap, RTTI, exceptions or `double`; warning-free with the kit's strict flags; ISR-shared data is atomic or volatile with the
  protocol stated at the declaration; real-time paths do not allocate, block or touch the card.
- Keep the proven register values exactly (clock tree, SDRAM controller, I2S dividers, SGTL5000 writes, FT812 bring-up, TCA8418 and
  IS31FL3731 setup, the flash configuration bytes). Cutting code is fine; "improving" a hardware value without a unit to test it
  on is not.
- Tests: keep the copied tests that still apply, fix their includes, delete the ones for code you removed.
- Do not commit.
