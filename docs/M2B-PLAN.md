# M2b-R "remote": the USB console, the SD card and the firmware update (plan, interfaces and ownership)

Status: in progress, 2026-10-25. The first round of M2b. The maintainer: "quiero que flashee en remoto, como haciamos en track3r-os"
... "traete eso al sdk". The plan was drafted by a read-only exploration of both trees (the m2b-plan agent) and accepted by the
integrator; the decisions are at the end. M2 itself has not run on the unit yet (`docs/hw/2026-10-25.md`, prepared, not run).

## Goal

An o3-sdk image (HELLO first, then o3-303 rebuilt with no o3-303 change) carries the kit's console on USB CDC, so an agent at the
computer can, with the ported host tools: put, arm and update a `.ptf` through the bootloader path; take screenshots; inject keys,
pads and encoder turns; read version, stage, cpu, mem, crash, log, leds, state, usb, sd, ls and fw; reset the unit; record the master
over USB audio. The unit can then go back to track3r-os, or on to another image, with nobody at it unless it is stranded.

## 1. Scope

In scope:

| Module | Why remote flashing and testing need it |
|---|---|
| track3r-os `usb/`, all of it -> `kit/usb` | The device logic, CDC, the shell, the log ring, the UAC2 streams; pure, depends on `core` only. MSC and UAC2 are kept so the library and its tests stay identical; unused code goes with `--gc-sections`. |
| The USB0 driver, STANDARD and AUDIO sets | The console's transport; AUDIO (profile P2, capture only) lets us hear the unit. |
| The console: `help version stage cpu guard mem crash usb log input peek poke rot sd ls fw reset key pad enc state shot leds cache`, plus app commands | Everything the goal lists. |
| The SD card (SDHC, FatFs, read path, card swap, claims) | `fw` writes `Firmware/`; `sd` and `ls` show the card. |
| The update path (`path_guard`, `hex_validator`, `fat_firmware_store`, `fw_service`, `fw_sink`, `fw_host_mk66`) | The documented update path. `fw_sink` (the client API) comes with it, so the later firmware switcher UI is a UI-only slice. |
| HIL pieces (`hil_inject`, `input_hold`, `shot_stream`, `mem_access`, `log_command`) | key, pad, enc, shot, peek, log. |
| Host tools `o3console.py`, `o3shot.py`, `o3audio.py`, `o3hil.py` | put/update, shot, audio, scenarios. |

Deferred: USB MIDI (the descriptors keep the MIDI interfaces, so STANDARD stays byte-identical to track3r-os's; the port is silent;
the seam is `usb0::bind_midi` and the ISR's SOF / from-host wrap; it needs `midi/`, the MIDI milestone); MSC disk mode (`msc_bridge`,
the DISK set, `o3disk.py`: `fw put` already moves firmware); the Power switch; the system Config screen and firmware switcher UI (the
console is the path this round); screenshot to card; the USB return stream, stems beyond the master, `o3tone.py`; settings on the card;
the tracker's own commands (mix, mon, load, inst, mod, radio, voices, ogexp, disk, power, bench, `enc accel`, `usb profile/return/map/ep0`,
`audio start`).

USB audio fits this round: the library side comes with slice U; the new work is the master tap feeding `usb::StemRing` from the render
and its binding. USB audio is proven on the unit in track3r-os, and it is the master **in parallel with the main output** (the maintainer,
2026-10-25): the image boots in the AUDIO set, as track3r-os does by default (its USB AUDIO row is On by default), with that set's
descriptors identical; the master goes on the first stereo pair of the capture stream; the return (OUT) stream's endpoint stays as the
descriptors have it and what arrives is discarded this round. STANDARD stays as the fallback set (`usb mode standard`, the mode guard).
(FRAMEWORK.md section 10 said "audio class 1"; what is ported is UAC2.)

## 2. Where the code comes from

Renames as in M2: `o3os` -> `o3sdk`, `O3OS` -> `O3SDK`, `<o3os/X>` -> `<o3sdk/X>`; M2's audio move also applies: `<o3os/audio/X>` ->
`<o3sdk/platform/mk66/audio/X>`, namespace `o3sdk::platform::mk66::audio`.

Every file is in one of three classes (decision D1):
- **I (identical):** rename only.
- **X (cut only):** M2's rule. Every kept line is identical; whole declarations or blocks are removed; each cut is listed in the slice
  report; only the binding lines named here may be rewritten.
- **N (new):** o3-sdk glue in separate files; where a handler body is copied verbatim from track3r-os it says so.

| track3r-os | o3-sdk | Class |
|---|---|---|
| `usb/` (13 src, 22 headers) | `kit/usb/` (`o3sdk_library(usb ... DEPENDS core)`, `<o3sdk/usb/...>`), with `-fsingle-precision-constant` and host Debug `-O2` as in its CMake | I |
| `tests/usb_*` (19 tests, 5 helpers; not `usb_return_sync_test.cpp`) | `tests/usb/` (own suite `o3sdk_usb_tests`) | I |
| `third_party/fatfs` | `third_party/fatfs` (target `o3sdk_fatfs`, `-w`) | I |
| `platform/mk66/storage/` (all) | `platform/mk66/storage/`: `platform_mk66_storage` (pure) and `_hw` (OBJECT: `sdhc_bus.cpp`, `device.cpp`) | I |
| `platform/mk66/update/` (all, plus `tools/fw_hex_verdict.cpp`) | `platform/mk66/update/`: `platform_mk66_update` (pure) and `_hw` (OBJECT: `fw_host_mk66.cpp`) | I |
| `audio/include/o3os/audio/diag.h`, `audio/src/diag.cpp` | `platform/mk66/audio/include/o3sdk/platform/mk66/audio/diag.h`, `audio/src/diag.cpp`. Keep `Text`, `format_cpu`, `format_shed`, `MemRow`, `BudgetRow`, `MemReport`, `format_mem`; cut both `format_mix`, `format_voices`, `format_app_tap`, `format_module_perf` and the includes `master_limiter.h`, `meters.h`, `module_perf.h` | X |
| `platform/mk66/usb/`: `soc_facts.h`, `boot_faults.h`, `console.h`, `log_command.*`, `mem_access.*`, `hil_inject.*`, `input_hold.h`, `shot_stream.*` | `platform/mk66/usb/`: `platform_mk66_usb` (pure) | I |
| `diag_text.h/.cpp` | the same place; cut the `audio/stem_map.h` include and `format_stem_map` | X |
| `usb0.h/.cpp` | `platform_mk66_usb_hw` (OBJECT); cut list below | X |
| `console.cpp`, `command_table.h` | `platform/mk66/console/` (new library `platform_mk66_console` plus `_hw`): they are the tracker's composition of the console | N, handler bodies verbatim |
| `msc_bridge.*`, `screenshot_service.*` | not copied (deferred) | |
| `tools/o3console.py`, `o3shot.py`, `o3audio.py`, `o3hil.py`, `fw_hex_cross.py` | `platform/mk66/tools/` | I |
| `tools/check_layers.py` rules 6 and 7 | `platform/mk66/tools/check_writers.py`: only `fat_firmware_store.cpp` names FatFs write calls (plus `disk_write`/`disk_ioctl` in `fat_storage.cpp`); only `board.h` and `fw_host_mk66.cpp` name `kBootloaderRequest` | N (adapted) |

`usb0.cpp` / `usb0.h` cut list (X): the includes `audio/stem_map.h`, `bench_disk.h`, `storage/device.h`, `msc_bridge.h`,
`midi/usb_bind.h`, `msc*.h`, `platform/mk66/audio.h`; `g_map`; the whole DISK block (`g_msc_link`, bridge, engine, pump, throttle,
`kick_usb`, `disk_*`, `set_render_load`, the `g_disk_boot` branches; `Mode::Disk` becomes "refused"); the bench blocks; the MIDI globals,
`bind_midi`, `midi_bind_stats`; in `usb_isr()` the MIDI SOF and from-host lines, leaving `g_dev.isr()`. Kept identical: the register
sequence (steps 0-7), `module_on`, detach/attach, `request_layout`, `service`, `detach_for_power`, `irc48m_off`, the descriptors' static
constructor; `g_return_ring` and `g_return_reader` stay (`IsoAudio`'s constructor takes them). The only rewritten lines:
`StemMap::supported(p)` -> `usb_audio::supported(p)` (P2 only); `IsoAudio(graph.stems().ring(), ...)` -> `IsoAudio(usb_audio::tap().ring(), ...)`;
the body of `on_streams` -> `usb_audio::tap().set_enabled(in_active)` plus its two log lines; the removed `graph.attach_return` and
`g_map.reset/apply` calls.

## 3. Dependencies on track3r-os internals, and the glue

| Dependency | Used by | Glue in o3-sdk |
|---|---|---|
| `audio/diag.h` (`Text`, formatters pulling in the tracker's meters, limiter, module perf) | `diag_text.h`, `sd_text.h`, console | the X port above; its test cases (Text, cpu golden, shed) go into `platform/mk66/tests/audio_status_test.cpp` as in track3r-os |
| `audio/stem_map.h`, `taps.h`, `audio_engine().graph()` | `usb0`, `diag_text`, `usb map` | new `platform/mk66/usb/{include/.../usb_audio.h, src/usb_audio.cpp}`: `class UsbAudioTap` (2 ch x `StemRing::capacity()+2` blocks x 128 frames, about 2.5 KiB, SRAM_U; `ring()`, `set_enabled()`, `push(const int16_t* lr, frames)` in the render), `usb_audio::tap()`, `usb_audio::wrap(hal::audio::BlockCallback inner)` (calls inner, then `push(io.out)`), `supported(Profile)`; modelled on `StemTap`'s publish protocol; obeys the lifetime contract in `stream.h` |
| `midi/*` | `usb0` | cut (section 2); the descriptors still present the MIDI interfaces |
| MSC, `CardOwner::Host` hand-over | `usb0`, console `disk` | cut; `storage::device::owner()` is never Host, so `fw_host_mk66`'s `card_on_host()` stays identical and false |
| `appimg::*` (track3r-os `src/internal.h`) | `fw_host_mk66.cpp`: `storage_in_use`, `stop_transport`, `audio_closed` | slice S copies those three declarations plus `service_while_blocked` into o3-sdk `src/internal.h` (identical text); slice C defines them in `platform/mk66/console/src/appimg.cpp`: `storage_in_use()` = false (no app saves yet), `stop_transport()` = `engine.stop()`, `audio_closed()` = main's `opened = false` (the gate reopens after a failed hand-over), `service_while_blocked()` = the idle hook |
| `power::device` (shell gate, input lock) | console | cut; no gate; `lock_input()` kept, nothing calls it |
| `diag_report.h` `fill_mem_tables` | `mem` | the console fills `MemReport` from o3-sdk's linker symbols (`__ramfunc_l_*`, `__noinit_start`, `__bss_end`, `__sdram_bss_*`, `__sdram_arena_*`) |
| `mem::kDiagLogBase/Size` (an SDRAM region) | log ring | `O3SDK_BIG usb::diag::LogRecord g_log_slots[1024]`: 64 KiB in `.sdram_bss`, zeroed by the reset sequence, then `LogRing::init` |
| `mem::kRamfuncLSize` (`hex_validator.cpp` static_assert) | update | additive constant in o3-sdk `mem_map.h`: `kRamfuncLSize = 0xC800` ("the SRAM_L window the update path's validator accepts: track3r-os's; o3sdk's `kRamfuncLBudget` fits inside") plus `static_assert(kRamfuncLBudget <= kRamfuncLSize)`; `hex_validator.cpp` stays identical and accepts track3r-os images (the way back) and o3-sdk images |
| `O3OS_BUILD_LABEL` | version, banner | `o3sdk_build_info.h` (`O3SDK_IMAGE_LABEL`, `O3SDK_APP_VERSION`) and `o3sdk_app().info()` |
| the crash record for `crash` | console | main: `console::note_boot_crash(mk66::boot_crash_record())` before `clear_crash_record()` |
| the screenshot RAM_G region (`appimg::shot_region`, `screenshot_active`) | `shot` | `ui::plot::kRamGShot` / `kRamGShotBytes` (already reserved: the spare region minus the 64-byte DMA scratch); `screenshot_active()` is false |
| `app::describe_state`, `appimg::button_code`, `enc_accel_*` | state, key, enc | kit hooks (slice C) and `app::find_button()`; encoder acceleration cut |
| `Scanner::inject` + `injected_pads_`, `PadLeds::in_sync`, `Ft812Display::bus_idle` (cut by M2) | key/pad/enc, leds, shot | restored from track3r-os line for line, with their test cases |
| `audio_pipeline()`, `audio_cpu_reset()`, `audio_set_starve_blocks()`, `open_wanted()` (cut by M2) | cpu, guard; the output gate during `fw update` | restored (three one-liners in `audio.cpp`/`audio.h`; `output_gate.h` plus its test cases) |
| `platform/host/dir_card.h`, `save/fat_storage_writer.h` | the screenshot service test, 2 cases of the swap test | not ported; the 2 swap cases ("the card handed to a host..." and "a save that began on one mount...") are cut and listed |
| `hal/firmware.h`, `hal/storage*.h`, `core/action.h`, `audio_clock.h`, `ring.h`, `input_event.h` | many | already in o3-sdk, identical modulo rename (check) |
| board pins, IRQ table, clock, `regs.h` accessors | drivers | already there and identical: USB0 is IRQ 53 in the Usb row; the SDHC row, `kBootloaderRequest` and `clock_config.h` present; every accessor the drivers name is in `regs.h` |

## 4. Slices

M2-PLAN's rules apply: stay in your files, make additive one-line changes elsewhere and list them, build only your own targets in your
own directories (`build-m2b-<slice>-host`, `build-m2b-<slice>-mk66`, slice names usb, sd, dev, con, tools), never flash, do not commit.
**A defect found in ported code goes in the report's "defects seen" list and is not fixed** (the maintainer's rule: ported track3r-os
code stays identical). Remote CI (`docs/ci.md`) may carry a slice's host and mk66 runs; it sends the whole tree, other slices' work
included.

| Slice | Owns |
|---|---|
| **U** usb library | `kit/usb/**`, `tests/usb/**`; one line each in the root `CMakeLists.txt` (`add_subdirectory(kit/usb)`) and `tests/CMakeLists.txt` (`add_subdirectory(usb)`) |
| **S** SD, FatFs, update | `third_party/fatfs/**`; `platform/mk66/storage/**`; `platform/mk66/update/**`; `audio/include/o3sdk/platform/mk66/audio/diag.h`, `audio/src/diag.cpp` and their line in `audio/CMakeLists.txt`; `tools/fw_hex_cross.py`; tests `mk66_storage_*`, `mk66_update_*`, the restored cases in `audio_status_test.cpp`. Additive: the `kRamfuncLSize` lines in `mem_map.h` and `mk66_mem_map_test.cpp`, the `appimg` declarations in `src/internal.h` |
| **D** driver and platform parts | `platform/mk66/usb/**` (includes the identical `console.h`); tests `mk66_usb_soc`, `mk66_usb_text` (cut: no command-table or stem-map cases), `mk66_hil_inject`, `mk66_input_hold`, `mk66_mem_access`, `mk66_shot_stream`, `mk66_usb_console_test`, new `mk66_usb_audio_tap_test`, new `mk66_usb_bringup_test`. Restorations in `input/scanner.{h,cpp}`, `input/pad_leds.h`, `display/display.h` and their tests |
| **C** kit console and image | `platform/mk66/console/**` (new); `kit/app/include/o3sdk/app/diag.h` (new) plus the hook lines in `app.h`, `runtime.h`, `runtime.cpp`, `tests/app_diag_test.cpp`; HELLO's `describe_state`; `platform/mk66/src/main.cpp`; the image block of `platform/mk66/CMakeLists.txt`; the audio restorations (`audio.h`, `audio.cpp`, `output_gate.h`, `mk66_output_gate_test.cpp`) |
| **T** host tools and checks | `platform/mk66/tools/{o3console,o3shot,o3audio,o3hil}.py`, `check_writers.py`, `check_usb_image.py` (new), `tools/hil/hello_smoke.json`; the self-test lines in the python-tools block of `platform/mk66/CMakeLists.txt` |
| integrator | `docs/**`, `THIRD_PARTY.md` (FatFs), `CLAUDE.md`, the hw-session doc, `fw-gate-check` runs |

Interfaces:

| Slice | Exposes |
|---|---|
| U | `o3sdk::usb::*`, identical to track3r-os: `UsbDevice<Regs>`, `ControlPipe`, `Config` / `build_*_descriptor` / `validate`, `CdcTxRing` / `CdcRxRing` / `CdcTxWriter`, `MidiRing`, `IsoAudio`, `StemRing` / `StemBlock` / `ReturnRing` / `ReturnReader`, `diag::{Shell, Output, Args, Command, LogRing, ConsolePump}`, `identity.h`. Target `usb` |
| S | `storage::device::{init, mount, last_mount_status, storage, fat, block, watch_begin, poll, owner, claim, card_busy, host_sessions, report}`; `run_ls_command`, `ls_release`, `format_sd`; `update::device::{service(), sink()}`; `FwService::{run, poll, terminal_closed, blocks_injection, phase}`; `fw_phase_holds_card`; `audio::diag::{Text, format_cpu, format_shed, format_mem}`. Its `_hw` objects link against C's `appimg::*`, `console::log` and D's `usb0::{serial, cdc_writer}` |
| D | `usb0::{start(Mode, Profile, bool), attached, service, mode, profile, return_enabled, request_layout, change_phase, pending_*, control, device_counters, stream_counters, cdc_writer, cdc_rx, serial, iso, reset_stream_counters, detach_for_power, irc48m_off}`; `usb_audio::{tap, wrap, supported}`; the `diag_text` formatters; `Injector` / `parse_key` / `parse_pad` / `parse_enc`; `ShotStream` / `ShotPort` / `ShotParams`; `InputHold`; `check_peek`, `check_poke`, `parse_hex32`, `parse_dec`; `run_log_command`; `Scanner::inject`, `PadLeds::in_sync`, `Ft812Display::bus_idle`. `usb0.cpp` links against C's `console::log*` |
| C | `console.h` implemented (`init_log`, `log`, `log_dec`, `log_hex`, `log_audio_clock_fault`, `note_boot_crash`, `note_boot_fault`, `clear_boot_fault`, `tap_input`, `lock_input`, `hold_input`, `service`, `shot_active`, `screenshot_port`) plus `console::attach(app::Runtime&)`; the kit command table (`console/commands.h`, names and help copied from `command_table.h`); `appimg.cpp`. Kit API (decision D2): `app::DiagOut`, `DiagArgs`, `DiagCommand`; `App::diag_commands(const DiagCommand*&) const` (default 0); `App::describe_state(const Context&, char*, size_t) const` (default 0); `Runtime::state_line(char*, size_t)`, giving `app=<name> screen=<title> playing=0|1 tempo=<mbpm> cpu=<pct> frames=<n>` then the app's pairs, with track3r-os key names |
| T | the tools identical except the rename; `check_writers.py check --root` with a self-test; `check_usb_image.py check <elf> --nm` (vector 16+53 resolves to `USB0_IRQHandler`, not `Default_Handler`; `g_bdt` 512-aligned; `"Track3r"` in the image), self-test included |

App commands (enforced in `console/core.cpp`, tested): names 2-7 lowercase letters, help at most 70 characters; no collision with a kit
command; no forbidden word (`update`, `flash`, `boot`, `bootloader`, `fwinfo`, `upload`, `dfu`, `program`, `erase`, `write`: `fw` is the
only writer); at most 16; a bad row is skipped and logged; the shell gets one table built at `attach()` (the kit's rows, then the app's
bound to one dispatcher).

Order: **U** first (a mechanical copy) and **T** in parallel from the start. **S** and **D** start once U's `usb` builds (both may copy
files earlier). **C** starts at once on its pure parts (`console/core`, the kit hooks, `trial_revert`, `usb_mode_guard`) and does
`console.cpp`, `main.cpp` and the image once D's and S's headers are in; the image links when D's and S's `_hw` objects compile. Then
the integrator gates both images, the docs and the hw-session page; then o3-303 is rebuilt against the new o3-sdk and gated (its own
`describe_state` and commands come later, in its repository).

| Slice | Host targets | mk66 targets |
|---|---|---|
| U | `usb usb_hdrcheck o3sdk_usb_tests` | `usb` |
| S | `platform_mk66_storage(_hdrcheck) platform_mk66_update(_hdrcheck) platform_mk66_audio_hdrcheck mk66_storage_tests mk66_update_tests mk66_audio_tests o3sdk_fw_hex_verdict` | the same plus `platform_mk66_storage_hw platform_mk66_update_hw` |
| D | `platform_mk66_usb(_hdrcheck) mk66_usb_tests mk66_input_tests mk66_display_tests` | the same plus `platform_mk66_usb_hw` |
| C | `platform_mk66_console(_hdrcheck) mk66_console_tests o3sdk_tests` | `hello`, then `ctest -L mk66` |
| T | `ctest -R 'mk66_o3(console\|shot\|audio\|hil)_selftest\|mk66_writers'` | |

Tests. Ported, identical unless noted: U the 19 usb tests; S `mk66_storage_{card,fat,protocol,soc,swap}` (swap minus 2 cases),
`mk66_update_{client,hex,path,store,transfer}`, `mk66_fw_hex_cross`, the audio diag cases; D `mk66_usb_soc`, `mk66_usb_text` (cut),
`mk66_hil_inject`, `mk66_input_hold`, `mk66_mem_access`, `mk66_shot_stream`, `mk66_usb_console_test` (real shell, pump and log pager
over the device against the fake controller), the restored inject / in_sync / bus_idle cases; T the tools' self-tests. New: D
`mk66_usb_audio_tap_test` (interleaved blocks in, `StemReader` out, samples exact; enable/disable; the lifetime contract under an
overwriting ring) and `mk66_usb_bringup_test` (the descriptors of the boot layout `(Standard, P2, false)` byte-identical to track3r-os's
STANDARD golden `usb_golden.h`; `(Audio, P2, false)` passes `validate()` with 2 capture channels; start preconditions); C
`mk66_console_core_test` (table assembly, the app-command rules, dispatch and the unknown-command error, help layout, `state` = kit
pairs then app pairs), `mk66_console_e2e_test` (the console core, `Shell`, `ConsolePump` and `UsbDevice` over track3r-os's `usb_rig`
fake controller with a fake App: `help`, `version`, `state`, an app command, `key f1` reaching the app through a fake injector),
`trial_revert_test`, `usb_mode_guard_test`, `app_diag_test`, the `<app>_usb` ctest (`check_usb_image.py`); T `mk66_writers`.

## 5. Integration

`main.cpp` (slice C). Module ids gain `kStorageModule = 5` and `kUsbModule = 6`. A new option `O3SDK_MK66_CONSOLE`, default ON; OFF
links exactly M2's image (no usb, storage or update), so the M2 session images do not change.

```
main:
  console::init_log(); log("boot: main reached"); log_hex reset sources
  record = boot_crash_record(): console::note_boot_crash(record); play kCrashSeen; clear_crash_record()
  display bring_up (log, note_boot_fault(kFaultDisplay) on failure); idle hook
  input init
  Platform{display, console::tap_input(input::input()), &input::leds(), clock, true}; Runtime rt; rt.start(); boot screen
  render = usb_audio::wrap(rt.block_callback())      (test-tone image: unwrapped)
  audio_device().start(render)                        (log, note_boot_fault(kFaultAudio))
  set_module(kStorageModule): storage::device::init(); mount() (at most about 3 s; the idle hook keeps the display going)
      -> log, kFaultSd if a card is in and does not mount; watch_begin(); claim(&firmware_has_card)
  console::attach(rt)
  set_module(kUsbModule): trial check; usb0::start(Mode::Standard, Profile::P2, false) -> log; set_module(0)
  loop: wfi; console::service(); input::service(); display::service(); rt.poll(); audio_main_pass();
        storage::device::poll(ms) (a card taken out or put in);
        every 100 ms: audio snapshot and the gate:
          open when !opened && open_wanted(audio_up, opened, false, false, appimg::firmware_update_active(), false) && output_gate.ready
        LED; trial-revert tick
```

`console::service()` runs track3r-os's pass in the same order: injector releases; cancel a `shot` stream the user ended;
`update::device::service().poll(shell)`; `usb0::service()`; the DTR banner, `terminal_closed()`, `pump.service()`, `shell.tick()`,
`cdc_writer().flush()`; then o3-sdk's `UsbModeGuard` (after a change to AUDIO, if `control().configured()` is still false 10 s later,
`request_layout(Standard, P2, false)` and log it). Command handlers are main-loop only, each puts at most 1.5 KiB of text on the 24 KiB
stack; `fw put` does one 2 KiB `f_write` per pass, as in track3r-os.

Priorities: the IRQ table is unchanged and identical to track3r-os (AudioDma 16 > SysTick 32 > Usb 64 > Completion 80 > Render 128).
The USB ISR is bounded (`kMaxIsrLoops` 16), runs from flash and preempts the render, as in track3r-os. The tap's `push` runs in the render
and writes the `StemRing`; the USB ISR is its only reader (at most one packet per SOF): one writer, one reader, no lock. USB starts
last, after audio: enumeration never delays the audio bring-up.

Memory [est, from track3r-os's release map less MSC]: flash about +75 KB (HELLO 68 -> about 145 KB; o3-303 113 -> about 190 KB);
SRAM_U `.bss` about +24 KB (HELLO 48 -> about 72 KB, o3-303 55 -> about 79 KB; the 32 KiB free rule holds); SRAM_L unchanged; SDRAM
+64 KiB log ring; RAM_G: the `shot` bands in `kRamGShot`.

USB identity (decision D3): `identity.h` identical (VID:PID 16D0:0E9F, manufacturer "track3r-os", product "Track3r", interface strings
"Track3r diag/MIDI/Audio", bcdDevice 0x0001 STANDARD and 0x0101 AUDIO, serial from the chip UID). STANDARD stays byte-identical, so
host driver caches stay valid and the host tools find the unit unchanged; o3-sdk is told apart by `version` and the banner
("Track3r diag - o3sdk HELLO 0.1.0 <label>"). Caveat: AUDIO P2 shares bcdDevice 0x0101 with track3r-os's AUDIO P8; use a Linux host
(the tools already assume one).

Host tools: no functional change. `o3console.py` finds `/dev/serial/by-id/*Track3r*`; `--put` checks with `--ramfunc-l
0x1FFF0000:0xC000` (accepts both); `--expect-version` is a substring match (use the app's hash or "o3sdk"); `o3audio.py` finds the unit
by "track3r" and reads 2 channels; `o3hil.py` works with `cmd`, `keys`, `pad`, `enc`, `state`, `shot`, `leds`, `wait` (the step kinds
`mix`, `voices`, `midi`, `card`, `disk` need commands o3-sdk does not have yet). T writes `hello_smoke.json` (version, state, keys, pad,
enc, leds, a shot compared with `hello_sim`'s PNG).

## 6. Done when

1. `build-host`: green, warning-free; `ctest` passes in full (kit, usb, mk66, tools).
2. `build-mk66`: `hello.elf/.hex/.ptf` warning-free; static gate (`hello_image`, `_flashconfig`, `_ramfunc`, new `_usb`, `mk66_writers`,
   `mk66_dma_owner`); no `__aeabi_d*`, no heap; `fw-gate-check` reproducible twice for `hello.ptf`.
3. On the host, `fw_hex_cross.py --hex <track3r-os v1.0.0 .ptf>` gives "valid" with o3-sdk's validator (the way back).
4. The unit procedure, run through `hw-session` (the test session trak-303-0f, with the maintainer's go):
   0. Preconditions: the unit runs track3r-os v1.0.0 and `o3console.py version` works; `fw` lists `PolyendTracker_0.0.1.ptf` (a known-good
      recovery image) as **first-ptf**, the target of the hidden reset button (if not, the maintainer fixes it by card first); note the
      volume serial and the CRC of the recovery file; back up the card.
   1. `o3console.py --put hello.ptf --name o3sdk-hello.ptf --expect-volume XXXX-XXXX` -> `ok ...`; the reply's `first-ptf` must
      still be `PolyendTracker_0.0.1.ptf`; if not, `fw rm o3sdk-hello.ptf` and stop.
   2. `o3console.py --update o3sdk-hello.ptf --crc <crc> --yes-update --expect-version <app hash>` -> re-enumerates and prints
      `Track3r o3sdk, build ...`.
   3. Over the o3-sdk console: `version stage "log 40" crash cpu mem usb sd fw leds state`; `--shot hello.png`, then `o3shot.py compare
      hello.png <hello_sim reference>`; `"key f1"`, `"pad 12"`, `"enc 3"`, then `state` and `--shot`; `"pad 12 down"`, `leds` (pad 12
      lit), `"pad 12 up"`; `"fw arm PolyendTracker_0.0.1.ptf <crc>"` -> `armed <token>` (the way back validates on the device; let the
      token expire).
   4. `"usb audio"` (the AUDIO set is up from boot); `o3audio.py record --seconds 3 --not-silent` while `pad 12 down`; `"usb audio"`
      (no underruns), `cpu`; `"usb mode standard"` and back to check the fallback.
   5. The way back: `o3console.py --update PolyendTracker_0.0.1.ptf --crc <crc> --yes-update --expect-version v1.0.0` (the file is
      already on the card); then from track3r-os, `fw rm o3sdk-hello.ptf`.
   6. Steps 1-5 again with o3-303's image.

A stranded unit (the o3-sdk image does not bring its USB up) and the mitigations: (a) the first-ptf rule (step 0 and the check in step
1): recovery is one press of the hidden reset button by a person at the unit, no card handling; (b) the boot is M2's with USB last: a
USB failure (`usb0::start` false, logged) leaves a working unit with a blink code; (c) no crash loop: if the last crash record says
module `kUsbModule` or exception 69 (IRQ 53), this boot skips `usb0::start`; (d) no trial auto-revert (the maintainer: "dejate de
autoreversion en la imagen"); (e) the mode guard: after a change of USB set, if the host has not configured the device 10 s later, the
unit goes back to STANDARD; (f) bring-up proved on the host as far as possible (the fake-controller enumeration and
property tests, `mk66_usb_console_test`, the bring-up golden, the e2e console test, `check_usb_image.py`); (g) no watchdog this round
(its clock and timeout are unverified on the unit: a wrong guess is a reset loop on the first boot). If the image does not boot at all
(M2 has never run on the unit), only (a) helps: someone presses the hidden reset button.

## 7. Sizes and risks

| Slice | Ported (lines) | New (lines) |
|---|---|---|
| U | lib 6.8k + tests 9.1k | about 60 (CMake) |
| S | storage + update 5.6k, FatFs 23.5k (third-party), audio diag about 250, tests about 6.6k, `fw_hex_cross.py` 280 | about 120 |
| D | about 2.5k (usb0 cut to about 470, `diag_text` about 585, the rest identical) + tests about 2.2k + restorations about 200 | tap about 150 + test 200, bring-up test about 120, CMake about 60 |
| C | about 450 lines of handler bodies verbatim inside new files; restorations about 50 | `console.cpp` about 650, core about 250 + tests about 500, `appimg` 80, trial revert and mode guard about 210 + tests about 300, kit hooks about 180 + test 150, `main.cpp` +150, image CMake +50 |
| T | tools about 3.95k | `check_writers.py` about 250, `check_usb_image.py` about 150, `hello_smoke.json` about 60 |

Risks: USB does not come up on o3-sdk (mitigations a-f); M2 has never booted on the unit (D6); UAC2 unproven on a Tracker (runtime
only, the fallback); "identical" classification disputes (the X and N files are listed above); shared-file collisions (kit/app hooks
and a later system-Config slice); the validator window constant (Done when 3 and step 3's `fw arm`); the card is written (`fw put`):
back it up; the same VID/PID/REV for different AUDIO descriptors on non-Linux hosts; the USB ISR's real-time cost during `fw put` and
AUDIO streaming (compare `cpu` with and without).

## Decisions

- **D1** the I/X/N classification above: accepted by the integrator (M2's rule; the maintainer's "keep track3r-os code identical").
- **D2** the App hook shape (`app::DiagCommand`, `App::diag_commands`, `App::describe_state`, `Runtime::state_line`): accepted.
- **D3** `identity.h` identical: accepted (the host tools and host caches work unchanged).
- **D4** trial auto-revert: **no** (the maintainer, 2026-10-25: "dejate de autoreversion en la imagen").
- **D5** the image boots in track3r-os's default AUDIO set, identical descriptors, the master in parallel with the main output (the
  maintainer: "el usb audio si se ha probado, y es en paralelo a la salida main"); STANDARD is the fallback.
- **D6** M2's card session runs first, with the maintainer at the unit (the maintainer: "todo lo demas OK" to the recommendation); the
  same session also runs o3-303's image and the fill-rate check for the 303 skin.
