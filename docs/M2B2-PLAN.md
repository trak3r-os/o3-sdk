# M2b, second round: the firmware switcher, MIDI and the Power switch (plan, interfaces and ownership)

Status: plan, 2026-10-26; D1-D4 taken as recommended by the orchestrator under the maintainer's standing "no questions, decide by the rules" (2026-10-25), open to the maintainer's override. Drafted by a read-only exploration of both trees (track3r-os at 022221c6). `docs/M2B-PLAN.md`
is the model: its rules, renames, I/X/N classes (its D1) and its section 4 working rules apply unchanged unless said here.

## Goal

Every o3-sdk image (HELLO first, then o3-303 rebuilt with no o3-303 change) gets: the kit's system Config screen on the Config button,
with FIRMWARE (switch the unit to another `.ptf` from the card, no computer) and INFO; MIDI in and out on DIN and USB, notes into the
engine, the transport following or sending MIDI clock; the Power switch's hold-to-off. An app reaches MIDI through its `Context`, never a
driver.

## 1. Scope

| Item | What the user gets | track3r-os state |
|---|---|---|
| System Config | the runtime takes the Config button; sections FIRMWARE, MIDI, DISPLAY (theme), INFO; `App::config_pages` declared | new: the tracker's Config is its own screen |
| FIRMWARE | list `Firmware/*.ptf`, check, two confirmations, install, the bootloader flashes, the new image boots | shipped in v1.0.0 (OS-FWCONFIG); on the unit IMAGE OK was reached, F1 INSTALL never pressed (`docs/STATUS.md` item 4) |
| MIDI | DIN in/out (UART4 RX, PIT0-timed TX), USB MIDI (the interfaces the descriptors already present, silent today), notes and CCs in, clock in (DLL follow) and out, transport in/out | shipped in v1.0.0 |
| Power | hold Power 2 s: silent, dark, card released, VLPR off, a press boots; `power` console commands | code shipped, unit-pending (`docs/STATUS.md` item 4: OS-POWER) |

Deferred: settings on the card (D4); the MIDI learn UI (`core/learn.h` exists, `CcInput` comes with MC); the AUDIO section; app config
pages beyond the declaration; Shift+Power screenshot to card; MSC disk.

## 2. Where the code comes from

Renames as M2 and M2b-R. New include move (D1): `<o3os/midi/X>` -> `<o3sdk/platform/mk66/midi/X>`, namespace `o3os::midi` ->
`o3sdk::platform::mk66::midi`: the M2 audio-move rule applied to `midi/`, because it includes `audio/hot.h` and `audio/events.h`, which
o3-sdk keeps under `platform/mk66/audio`, and the kit must not include the platform.

| track3r-os (lines) | o3-sdk | Class |
|---|---|---|
| **Firmware switcher** | | |
| `app/include/o3os/app/firmware_view.h` (61) | `kit/app/include/o3sdk/app/firmware_view.h` | I |
| `app/src/firmware_actions.cpp` (300) | `kit/app/src/firmware_flow.cpp` | N, bodies verbatim: `refusal_text`, `to_list`, `step`, `to_failure`, the phase logic of `firmware_open/go/back`, `firmware_question_taken_back`, `firmware_question_drop`, `firmware_poll`, the card-change close (lines 22-300); `blocked()` becomes `App::update_blocker()`; `ActionContext` becomes the kit's `Context` |
| `app/src/screens/firmware_overlay.cpp` (184) | `kit/app/src/system_firmware_page.cpp` | N: drawn with `kit/ui`, texts copied; its includes (`app/frame.h`, `key_prompt.h`, `screens/overlay.h`, `dialog.h`) are the tracker's UI and are not ported |
| `platform/host/include/o3os/platform/host/fake_firmware.h` (211), `tests/fake_firmware_test.cpp` (197) | `platform/host/include/o3sdk/platform/host/fake_firmware.h`, `tests/host_fake_firmware_test.cpp` | I |
| `tests/firmware_update_test.cpp` (755) | `tests/app_firmware_flow_test.cpp` | N: its cases against the kit flow and the fake (the tracker's App harness is not ported) |
| `hal/firmware.h` (100), `platform/mk66/update/` incl. `fw_sink.cpp` | already in o3-sdk; `hal/firmware.h` checked identical | - |
| **MIDI** | | |
| `midi/src/{din,usb_out,clock_dll,in,out,usb_bind}.cpp` (1,139), `midi/include/o3os/midi/*.h` 13 files (1,683) | `platform/mk66/midi/`: `o3sdk_library(platform_mk66_midi ... DEPENDS core usb platform_mk66_audio)`, the compile options of its CMake (`-fno-math-errno`, cross `-fno-tree-loop-distribute-patterns`, host Debug `-O2`) | I + include move |
| `audio/include/o3os/audio/events.h` (242, no includes but `<cstdint>`, `<type_traits>`) | `platform/mk66/audio/include/o3sdk/platform/mk66/audio/events.h` (`midi/out.h` takes `audio::SeqEvent`) | I |
| `platform/mk66/include/o3os/platform/mk66/midi_config.h` (100), `platform/mk66/tests/mk66_midi_config_test.cpp` (127) | `platform/mk66/midi/include/o3sdk/platform/mk66/midi_config.h` (same include path), `platform/mk66/midi/tests/` | I |
| `tests/midi_{core,din,usb,router,in,cc_in,clock_dll,io}_test.cpp` (3,009) | `platform/mk66/midi/tests/`, suite `mk66_midi_tests` | I |
| `platform/mk66/include/o3os/platform/mk66/midi.h` (37), `platform/mk66/src/midi_din.cpp` (153) | the same places; in the image | I |
| `platform/mk66/usb/src/usb0.cpp` MIDI lines (45, 195-199, 577-585, 594-603) and `usb0.h` `bind_midi`, `midi_bind_stats` | o3-sdk `usb0.cpp`/`usb0.h` | restoration of the M2b-R cuts, line for line (the MIDI rings 120-124 are still there) |
| `platform/mk66/src/audio.cpp:433` `audio_block_heard_at()`, `audio.h:75` | o3-sdk `audio.cpp`/`audio.h` | restoration |
| `seq/include/o3os/seq/midi_link.h` (232), `seq/src/midi_link.cpp` (434), `tests/seq_midi_link_test.cpp` (955); `platform/mk66/src/app_main.cpp:835-904` | not ported: they wrap the tracker's `seq::Sequencer` (`set_follow`, `next_follow_clock`, `step`). The reference for o3-sdk's `MidiService` (N); bodies that do not name the sequencer (the drain, the follow state, the flush and kick) are copied and listed | N |
| `platform/mk66/tools/hil/midi_clock_tool.py` (184) | `platform/mk66/tools/hil/` | I |
| **Power** | | |
| `platform/mk66/power/src/{power_service,power_report,power_off}.cpp` (315, 86, 111), `include/.../power/*.h` 5 files (393) | `platform/mk66/power/`: `platform_mk66_power` (pure) | I |
| `platform/mk66/power/src/power_host_mk66.cpp` (387) | `platform_mk66_power_hw` (OBJECT) | X: cut the MSC lines (`usb0::DiskState`, `disk_state()` 231-233, the Host clause of 254, `disk_close()` 258) |
| `platform/mk66/tests/mk66_power_off_test.cpp` (441), `mk66_power_service_test.cpp` (725) | `platform/mk66/tests/` | I |
| `platform/mk66/src/boot.cpp:134` `set_core_hz`, `internal.h:39`; `audio.h:36` `audio_stop()` and its body | o3-sdk `boot.cpp`, `internal.h`, `audio.h`, `audio.cpp` | restoration (`clock_config.h` already has `kVlprCoreHz`, `kExitCoreHz`, `kExitCrystalCoreHz`) |
| `platform/mk66/usb/src/console.cpp` `power` handler (header lines 52-54) and the "ERR powering off" shell gate | `platform/mk66/console/src/console.cpp` | N, handler body verbatim |
| `core/power_hold.h`, `tests/power_hold_test.cpp`; board `kPowerSwitch` PTC3, `kPowerPull` | already in o3-sdk; `power_hold.h` checked identical | - |
| `tests/power_app_test.cpp` (364), `platform/mk66/tools/hil/qa_power*.json` | not ported (the tracker App's `set_power`); the strip and bar cases become kit cases; the scripts are the reference for `hello_power.json` | |

## 3. How an app sees MIDI (D2, D3)

```
UART4 RX ISR (48) -stamp-> DIN rx ring --+                                              +-> DinEmitter -PIT0 (96)-> UART4 TX
USB0 ISR (64)     -stamp-> USB rx ring --+-> MidiService::begin_block   ...   end_block -+-> UsbMidiOut -SOF-> USB MIDI IN
                                             | MidiIn drain, ClockDll        ^ MidiOut send/flush: clock from the Transport,
                                             +-> core::ClockFrame -> Engine::follow       Start/Stop, the hooks' send_midi
                                             +-> routed notes -> dsp::Event on a unit at their frame (render)
                                             +-> CcInput ring -> main loop -> App::midi()
```

Kit API (N, slice K). It obeys the layering `core <- hal`, `core <- dsp`:
- `kit/core/include/o3sdk/core/midi_types.h`: `ClockFrame {bool follow, playing, started, stopped; uint64_t pos, inc; uint32_t tempo_mbpm;}`
  (Q32.32 ticks at 960 ppqn, as `dsp::Transport`); `MidiMessage {uint8_t ports, status, data1, data2; uint16_t frame;}`.
- `kit/hal/include/o3sdk/hal/midi.h`: `hal::midi::{Port (Din, Usb), Config, Status, Device}`. `Device`, render side:
  `begin_block(frames, MidiMessage* in, cap, ClockFrame&) -> n` (this block's messages for the routes, frame-stamped; the clock frame
  when following), `end_block(const ClockFrame& transport, const MidiMessage* out, n)`; main side: `set_config()` (the `SeqLockBox` of
  `midi/config.h`: the render never waits), `status()`, `poll(MidiMessage&)` (everything, for the app), `send(const MidiMessage&)`.
  `Config` = `MidiConfig`'s fields: `channel` (0 omni, 1-16), `out_channel`, `out_ports`, `clock_out`, `follow`, `follow_port`,
  `transport_in`, `transport_out`. `Status`: following, locked, the master's tempo, the rx/tx/dropped counters.
- `kit/dsp`: `Engine::follow(const core::ClockFrame&)` and `Engine::inject(UnitId, const Event&)`, both render side, before `render()`.
  With `follow` set, the block's `Transport` is the frame's instead of the engine's own advance. `Transport` gains `following`
  (additive). `BlockContext::send_midi(const core::MidiMessage&)` takes at most 32 a block. While following, the main loop's `play()`
  and `stop()` go to the device: Play starts on the master's next clock (track3r-os OS-MIDISLV b).
- `kit/app`: `Platform::midi` (`hal::midi::Device*`; null means none: the test rig, the M2 image). `Runtime::render_block` runs
  `begin_block`, `inject` for each routed note, `follow`, `render`, `end_block`. `Context::midi()` returns `app::Midi`, main loop only:
  `route(UnitId, channel)` (at most 4 routes), `config()`, `set_config()`, `status()`, `send()`.
  `App::midi(Context&, const core::MidiMessage&)` runs in the main loop, default false: every channel message (CCs, program changes,
  notes not routed). The MIDI section of the system Config edits `Config`.

Notes into the engine: a note on/off on a route's channel becomes `dsp::note_on/note_off` for that unit, at the frame its ISR stamp maps
to (through `ClockMap`/`TimeRef`), one block later: a constant latency of about 1.5 ms and no jitter, as track3r-os's live notes.
Messages that are not routed reach `App::midi()` only.

Clock sync for an app like o3-303: its sequencer is a clocked hook walking `ctx.transport()` with `for_each_step`. With follow on, the
transport is the DLL's: pos at frame 0 is the master's tick at that sample x 40 (960/24), and inc is 40 per the DLL's period in samples,
Q32.32. The DLL slews and never jumps. So o3-303's steps land on the master's grid with no o3-303 change:
- Start: `started` with pos 0.
- Continue after an SPP: playing from the SPP position, with `started` false.
- A master quiet for 48 clocks stops the transport (track3r-os rule a).
- Clock out: `end_block` puts a 0xF8 at every `frame_at(t, k * kTicksPerMidiClock)` of the block. That is sample-accurate, on the
  emitter's "clock is sacred" lane. Start goes out at `started`, Stop at `stopped`. A tempo change needs nothing.

## 4. The firmware switcher in an app-image world

- One app is one `.ptf`, the whole firmware: changing engine is changing image. The kit's FIRMWARE section, in every app (FRAMEWORK
  section 5, the maintainer 2026-10-25), drives the update path M2b-R ported and verified: `hal::firmware::FirmwareSink` =
  `update::device::sink()`.
  - List: `Firmware/*.ptf`, read in one pass of the main loop.
  - Check: sliced, 2 KiB a pass. `hex_validator` accepts o3-sdk and track3r-os images, so the list reaches track3r-os and back.
  - Confirm: two questions, F1 each; nothing is written before INSTALL.
  - Install: `appimg::stop_transport`, `_fwinfo` written, the bootloader request, the reset. The other image boots.
  - Each image carries the switcher, so any unit can always change engine again. track3r-os has its own: Config, INFO, F8 UPDATE.
- The kit owns it, not the app. An app cannot hide FIRMWARE or INFO. It can refuse for a moment with
  `App::update_blocker(ctx) -> const char*` (default: none; track3r-os's `blocked()`), for example while it records.
- What the user sees: file names. `FirmwareView::kMax` is 12; the files beyond it are counted. o3-sdk images are named
  `o3sdk-<app>-<version>.ptf`, which `o3console.py --put --name` already writes. INFO shows the running app's name, version and build id.
  The 12-file limit is track3r-os's OS-BUG-FWLIST: ported as it is, reported, not fixed.
- Safety, all inherited:
  - The switcher never writes, renames or deletes a file (only `fat_firmware_store` writes; `check_writers.py`), so the recovery image
    stays first-ptf for the hidden reset button.
  - No auto-revert (M2b-R D4).
  - A good check waiting for INSTALL refuses HIL key injection (`FwService::blocks_injection`), so the final F1 INSTALL needs a finger
    on the unit.
  - Installing locks input and Power (`firmware_active()` refuses the off).
  - A card removal closes the dialog and cancels.
- Host: `fake_firmware.h` gives the simulator a sink (`<app>_sim --firmware-dir DIR`), so the pages have goldens; nothing touches
  flash off the unit.

## 5. Kit and platform

| | kit (core, hal, dsp, ui, app) | platform/mk66 | platform/host, sim |
|---|---|---|---|
| Config, FIRMWARE | the Config intercept and system layer (`system_config.h`), `firmware_view.h`, `firmware_flow.cpp`, the pages, `App::update_blocker`, `App::config_pages`, `Platform::firmware` | `update::device::sink()` passed from `main.cpp`; `appimg` (exists) | `fake_firmware.h`, the simulator option |
| MIDI | `core/midi_types.h`, `hal/midi.h`, `Engine::follow/inject`, `BlockContext::send_midi`, `Context::midi`, `App::midi`, routes, the MIDI section | `platform_mk66_midi` (ported core), `MidiService` (implements `hal::midi::Device`), `midi_din.cpp`, the usb0 binding, console `midi` | `FakeMidi` (scripted input, captured output, a clock generator); scenario lines `midi <port> <hex...>`, `clock <bpm> start/stop` |
| Power | `Runtime::system_strip(text, progress)`, `Runtime::lock_input(bool)` (the system layer, slice F) | `platform/mk66/power/**`, the pass in `main.cpp`, console `power` | none: the shutdown is the unit's; the strip has kit tests |

## 6. Slices

M2B-PLAN section 4's rules apply. Build directories are `build-m2b2-<slice>-{host,mk66,asan}`, with the slice names sys, mcore, kmidi,
power and mdrv. At most two agents at a time, with no shared file:

| Wave | Agent 1 | Agent 2 |
|---|---|---|
| 1 | **F** system Config + FIRMWARE | **MC** MIDI core port |
| 2 | **K** kit MIDI API, engine follow, host fake (after F: `kit/app`) | **P** Power (after F: `main.cpp`, the strip API) |
| 3 | **MD** MK66 MIDI service and drivers (after MC, K, P) | integrator: gates, hw-session page, docs |

| Slice | Owns |
|---|---|
| **F** | `kit/app/**` (`runtime.h/.cpp`, `app.h`, `context.h`, new `system_config.{h,cpp}`, `firmware_view.h`, `firmware_flow.{h,cpp}`, `system_firmware_page.cpp`); `platform/host/include/.../fake_firmware.h`; `sim/**` (the sink option); `apps/hello/**` (theme toggle -> DISPLAY, scenario `config.txt`); tests `app_system_config_test`, `app_firmware_flow_test`, `host_fake_firmware_test`; `platform/mk66/src/main.cpp` (the sink in `Platform`) |
| **MC** | `platform/mk66/midi/**` (new: include, src, tests, CMake); `platform/mk66/audio/include/o3sdk/platform/mk66/audio/events.h`; one line in `platform/mk66/CMakeLists.txt` (`add_subdirectory(midi)`) |
| **K** | `kit/core/include/o3sdk/core/midi_types.h`; `kit/hal/include/o3sdk/hal/midi.h`; `kit/dsp/**` (`engine.h/.cpp`, `transport.h`); `kit/app/**`; `platform/host/**` (`fake_midi.h`); `sim/**`; `apps/hello/**` (a route to its synth, `midi.txt`); tests `dsp_follow_test`, `app_midi_test`, `sim_midi_test` |
| **P** | `platform/mk66/power/**`; `platform/mk66/console/**`; `platform/mk66/src/{main.cpp, boot.cpp, internal.h}`; `audio.h`/`audio.cpp` (`audio_stop` only); the power block and `O3SDK_POWER_VLPR_OFF` (ON, as track3r-os) in `platform/mk66/CMakeLists.txt`; tests `mk66_power_*`; `platform/mk66/tools/hil/hello_power.json` |
| **MD** | `platform/mk66/midi/{include,src}/.../midi_service.*` (new); `platform/mk66/src/midi_din.cpp`; `platform/mk66/include/o3sdk/platform/mk66/midi.h`; `platform/mk66/usb/**` (restoration); `platform/mk66/console/**`; `main.cpp`; `audio.h`/`audio.cpp` (`audio_block_heard_at`); the image block of `platform/mk66/CMakeLists.txt`; `platform/mk66/tools/hil/{midi_clock_tool.py, hello_midi.json}`; test `mk66_midi_service_test` |
| integrator | `docs/**`, `CLAUDE.md`, `fw-gate-check`, the hw-session page, o3-303's rebuild and gate |

| Slice | Exposes |
|---|---|
| F | `Runtime`: the Config intercept (an app never sees Config), `system_strip(text, progress, hold_ms)`, `lock_input(bool)`, `system_open()`, `system()`, `card_changed()` (main.cpp calls it from the card watch); `App::update_blocker`, `App::config_pages` (default 0); `Platform::firmware` (null hides FIRMWARE); `state` pairs `sys=0/1 section=<name> fw=<phase>` |
| MC | `o3sdk::platform::mk66::midi::*`, identical to track3r-os's `o3os::midi::*`: `DinEmitter`, `RtInRing`, `MidiIn`, `ClockDll`, `ClockMap`, `TimeRef`, `MidiOut`, `UsbMidiOut`, `CcInput`, `MidiConfig`, `SeqLockBox`, `usb_midi_sof/from_host`; `audio::SeqEvent`; target `platform_mk66_midi` |
| K | section 3 |
| P | `power::device::{init, pass, locked, request_off, inject, report, vlpr_off, take_screenshot_request, pressed}`; console `power`, `power press/release/pin`, `power off [dry]` |
| MD | `MidiService` (a `hal::midi::Device`); `midi_io::{start, kick, rx_stats, tx_stats}`; `usb0::{bind_midi, midi_bind_stats}`; console `midi` (counters, mode, DLL lock and BPM, the last bytes) |

| Slice | Host targets and tests | mk66 targets | Only the unit (over USB through o3console unless noted) |
|---|---|---|---|
| F | `app sim o3sdk_tests hello_sim`; `hello_sim` on `config.txt` (Config opens and closes, the list from a fake folder, the two questions, a failed check, BACK) | `hello`, `ctest -L mk66` | `key config`, `shot`, `state` (`sys=1 section=firmware`); the list matches `fw`; a check to IMAGE OK (`fw=ask_install`) and BACK; the final F1 INSTALL is a finger (injection refused by design), then `o3console.py version` shows the other image; the way back with `--update` |
| MC | `platform_mk66_midi(_hdrcheck) mk66_midi_tests`, ASan too | `platform_mk66_midi` | none |
| K | `core hal dsp app sim platform_host o3sdk_tests hello_sim`; `dsp_follow_test` (a scripted ClockFrame: an o3-303-like hook's steps land on the master's ticks within 1 sample; Start, Continue at SPP, Stop, a quiet master); `app_midi_test` (routes to frames, the rest to `App::midi`, the MIDI section); `sim_midi_test`, `midi.txt` (notes sound in the WAV; `clock 140` gives tempo 140) | `hello` (MIDI null: the image unchanged) | none |
| P | `platform_mk66_power(_hdrcheck) mk66_power_tests mk66_console_tests` | + `platform_mk66_power_hw`, `hello`, `ctest -L mk66` | `power` (the pin in three pulls: track3r-os's open M4, pull and polarity); `power press`, 2.5 s, `shot` (the bar), `power release`; `power off dry` (the stages in `log`, re-enumeration, `version`); a real hold, the VLPR off and the wake: the maintainer at the unit (USB is down while off) |
| MD | `platform_mk66_midi mk66_midi_service_test mk66_usb_tests mk66_console_tests`, `midi_clock_tool.py --self-test` | `hello`, `ctest -L mk66` (`hello_ramfunc`, `_usb`) | the computer as the far end of USB MIDI: notes in, then `o3audio.py record --not-silent`; `midi_clock_tool.py send DEV --profile 120:10,140:10 --start`, then `midi` reports locked and `state` tempo follows within 0.1 BPM; `measure DEV` with HELLO playing gives 24 ppqn at its tempo; `cpu` with and without; DIN: a loopback cable (out to in) placed once by the maintainer, then `midi` counters show the loop with 0 framing errors |

## 7. Integration

`main.cpp` (P, then MD): `power::device::init()` after input. The `MidiService` is constructed, then `midi_io::start` and
`usb0::bind_midi` run before `audio_device().start(render)`. The loop gains `power::device::pass(now, shift, ready)`, which feeds
`rt.system_strip()`. IRQ priorities stay as `hal/irq.h` has them, identical to track3r-os: MidiUart 48, Usb 64, MidiOutTimer 96,
Render 128. PIT0 is `PitOwner`'s MIDI channel in `resources.h`.

Memory [est, from track3r-os]:
- SRAM_U: track3r-os's MIDI link takes about 23 KB (`app_main.cpp:195`, capped at 24 KiB). o3-303's `.bss` would go from about 79 KB
  to about 100 KB, so the 32 KiB free rule must be checked again.
- SRAM_L: the MIDI hot functions must fit `kRamfuncLBudget` (32 KiB).
- Flash: about +15 KB for MIDI, +8 KB for Power, +10 KB for the system Config.

## 8. Sizes

| Slice | Ported (lines) | New (lines) |
|---|---|---|
| F | `firmware_view` 61, `fake_firmware` 211 + test 197; about 200 verbatim inside new files | system layer and intercept about 350, pages 250, flow glue 100, tests 600, sim and HELLO 100 |
| MC | lib 2.8k, `events.h` 242, `midi_config` 100, tests 3.1k | CMake about 50 |
| K | - | `midi_types` 50, `hal/midi` 120, follow and inject 80, app API 200, fake and sim 200, MIDI section 120, tests 600 |
| P | pure 0.9k, host 387 (X), tests 1.17k, restorations about 20, `power` handler about 80 | appimg power hooks 60, main +30, CMake +30, `hello_power.json` 40 |
| MD | `midi.h` 37, `midi_din` 153, `midi_clock_tool` 184, usb0 about 35, `heard_at` 2 | `MidiService` about 450 + test 400, console `midi` 80, main +25, CMake +15, `hello_midi.json` 50 |

## 9. Risks

- The follow seam is new code on the render path. The DLL-to-Transport mapping must not jump at a DLL rebase. It is tested on the host
  against a scripted clock, then on the unit with `midi_clock_tool.py`.
- SRAM_U and SRAM_L budgets (section 7). If the rings move to SDRAM, the ISR latency has to be measured on the unit.
- CPU: the USB ISR does MIDI work at every SOF, and the PIT0 ISR runs. Compare `cpu` with USB audio streaming and MIDI on, and without.
- Power has never run on a Tracker: the pull and polarity are open (M4) and the VLPR wake is unproven. A wrong pull would read as a
  ghost hold; track3r-os logs "held or stuck since boot". The fallback is `O3SDK_POWER_VLPR_OFF=OFF` (restart).
- The Config takeover changes the controls of HELLO and o3-303 (their theme toggle moves to DISPLAY). o3-303 needs a rebuild and a
  scenario update in its own repository.
- The final INSTALL from the screen cannot be done remotely: a hands session is needed. The firmware list stops at 12 files
  (OS-BUG-FWLIST).
- Files shared across waves: `kit/app` (F, then K), `main.cpp` and `console/` (F, then P, then MD). The waves keep agents apart, but
  each slice starts by rebasing on the last.
- Defects seen in track3r-os (for the maintainer, not fixed):
  - `midi/CLAUDE.md` names `platform/mk66/tools/midi_clock_tool.py`; the file is in `tools/hil/`.
  - The header of `docs/specs/OS-POWER.md` still says "no code yet".

## Open decisions

- **D1** Where the MIDI core lives. Recommended: `platform/mk66/midi`, with the audio-move include rule (I plus the include move). The
  alternative, `kit/midi` with the rename only, would make the kit include platform headers (`hot.h`, `events.h`).
- **D2** How notes reach an app. Recommended: routes in the render (a channel to a unit, frame-accurate, constant latency) plus every
  message to `App::midi()` in the main loop. The alternative, the main loop only (track3r-os's `CcInput` path), is simpler but adds 1-2
  blocks and a pass's jitter.
- **D3** Clock sync through the engine's Transport (`Engine::follow`). Recommended: yes. Every clocked hook (o3-303's sequencer)
  follows with no change; Play while following waits for the master's next clock.
  - Defaults: follow off; clock and transport out on both ports while playing; notes in on any channel (omni), out on channel 1.
  - The alternative, each app reading the clock itself, changes nothing in the engine, but every app has to solve sync again.
- **D4** Saving settings. Recommended: none this round. The kit and MIDI settings last until power-off, an app sets its defaults in
  `setup()`, and the update path stays the card's only writer (`check_writers.py` unchanged). The alternative, a settings file on the
  card now (track3r-os's `Settings/device.o3st`), means a second writer with its own guard; it belongs with the app file API.

### Critical files for implementation
- kit/app/src/runtime.cpp
- kit/dsp/include/o3sdk/dsp/engine.h
- platform/mk66/src/main.cpp
- track3r-os: seq/include/o3os/seq/midi_link.h (reference only)
- track3r-os: app/src/firmware_actions.cpp (reference only)
