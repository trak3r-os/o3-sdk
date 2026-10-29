# M2c: the whole hardware for every app (plan, interfaces and ownership)

Status: plan, 2026-10-29. Decisions D1-D18 are taken as recommended under the maintainer's standing "no questions, decide by the rules"
(2026-10-25); the maintainer may override any of them. The maintainer (2026-10-29): first finish the whole SDK properly, with all functionality
available from the SDK; only after that, a usability round of the o3-303 app. This plan is the map of that remaining SDK work. It was drafted
by a read-only exploration of both trees (track3r-os at 5155757a). `docs/M2B-PLAN.md` is the model: its I/X/N classes (its D1), its renames and
its section 4 working rules apply here unchanged unless this plan says otherwise.

## Goal

Every capability of the Tracker hardware reaches an app through a kit contract, with a host fake and a simulator command, and without
touching a register. An app that does not use a capability pays nothing in the render. At the end, `docs/FRAMEWORK.md` section 10 has no
"planned" row.

**Where it starts (2026-10-29).**
- Verified on the unit over USB: M2 (display, input, LEDs, audio out) and M2b-R (console, install, USB audio out, card read path).
- Built and host-tested, not yet run on a unit: slices F (system Config, FIRMWARE, DISPLAY, INFO), MC (MIDI core), K (kit MIDI API, the MIDI
  section, clock follow) and P (Power).
- Being finished by another agent: MD (MK66 MIDI service). This plan treats it as done.
- Their unit session belongs to M2b's second round, not to this plan.

## 1. Scope

| # | Item | What an app gets | track3r-os state |
|---|---|---|---|
| 1 | App file API | save and load its patterns, kits, settings and samples in its own folder on the card; listing; delete by marker; never blocks the render | save layer shipped in v1.0.0; project and WAV saves PASS on the unit (`docs/qa/final-report-2026-10-20.md`); card pull and power cut never run on a unit |
| 2 | System Config: AUDIO, persistence, the rest | AUDIO section; MIDI (exists) and every kit setting kept on the card; an app's own pages; brightness; MIDI learn | Config pages MIDI, DISPLAY, SD, INFO, USB, PATTERN; `Settings/device.o3st` |
| 3 | Audio in | line, line L/R, mic low/high, radio, USB as the source; gain; monitor; input peaks | shipped; a real line-in signal never seen on a unit (fqa.11), the mic never measured |
| 4 | Recording | takes into an SDRAM buffer the app owns: free, step, bar or level start, pre-roll, stop at a boundary; saved as WAV | shipped; `qa_record` PASS (SAVE WAV wrote `REC_001.wav`) |
| 5 | FM radio | power, region, tune, seek, status; a source to record | driver shipped; on the unit the first run said Failed and the fix was never re-run; RDS never written |
| 6 | USB audio | stems beyond the master (P8, P10, P11), the computer's return into the engine; the card as a USB disk | stems and duplex return verified (P8, P2); P11 never measured; P10 duplex fails behind a hub; disk mode PASS (int.15-17) |
| 7 | Leftovers | crash record proven; list and keyboard widgets; one-command install; toolchain fetch; screenshot to card; header without a transport; setup errors in `state` | mixed (each section 9 row) |

Out of scope:
- The o3-303 usability round. o3-303 is rebuilt and gated at each wave with no o3-303 change.
- Loadable units from the card.
- A watchdog (track3r-os has none either; M2B-PLAN section 6 (g)).
- The headphone level as a user control (D13).

## 2. Where the code comes from

Renames as in M2 and M2b: `o3os` -> `o3sdk`, `<o3os/X>` -> `<o3sdk/X>`. The audio move applies: `<o3os/audio/X>` ->
`<o3sdk/platform/mk66/audio/X>`. New for this plan: track3r-os's `formats/` library becomes `kit/formats` (`core <- formats <- app`);
`<o3os/formats/X>` -> `<o3sdk/formats/X>`.

| track3r-os (lines) | o3-sdk | Class |
|---|---|---|
| **Files (item 1)** | | |
| `formats/include/o3os/formats/{crc32.h (24), progress.h (12), sample_probe.h (17)}`, `formats/src/{crc32.cpp (44), sample_probe.cpp (57)}` | `kit/formats/` (new library `formats`, DEPENDS core) | I |
| `formats/wav/include/o3os/formats/wav.h` (211), `formats/wav/src/wav_importer.cpp` (823) | `kit/formats/` (the resumable WAV reader: a sample into SDRAM) | I |
| `app/include/o3os/app/atomic_save.h` (211), `og_timing.h` (126) | `kit/app/include/o3sdk/app/` | I |
| `app/src/atomic_save.cpp` (472) | `kit/app/src/` | X: line 13 `namespace guard = hal::storage::save;` becomes `hal::storage::app_guard` (D2) |
| `app/include/o3os/app/{dir_scan.h (140), dir_index.h (108), text_entry.h (88)}`, `app/src/{dir_scan.cpp (325), text_entry.cpp (172)}` | `kit/app/` (sliced folder listing; the on-screen keyboard's state machine) | I |
| `platform/mk66/save/include/.../fat_storage_writer.h` (71), `CMakeLists.txt` (22) | `platform/mk66/save/` (`platform_mk66_save`, pure as in track3r-os) | I |
| `platform/mk66/save/src/fat_storage_writer.cpp` (276) | the same | X: line 15, the same alias line |
| `platform/mk66/save/{bench_scratch.*}` (305) | not ported (the bench) | |
| `platform/host/include/o3os/platform/host/{dir_storage.h (69), host_card.h (22), fake_storage.h (103)}`, `src/{dir_storage.cpp (286), fake_storage.cpp (297)}`, `platform/host/fat/**` (`fat_image` 82+382, `fat_fs_storage` 124+227) | `platform/host/` (a card over a host directory; a real FatFs volume over an image for the power-cut test) | I |
| `platform/host/include/.../dir_card.h` (109), `src/dir_card.cpp` (338) | `platform/host/` | X: line 19, the alias line |
| `tests/support/mem_card.h` (445) | `tests/support/` (an in-memory card with `cut_at`: a power cut at any write) | X: line 24, the alias line |
| `tests/support/{save_source.h (68), memory_io.h (104)}` | `tests/support/` | I |
| `app/src/save_service.cpp` (2108), `save_service.h` (653) | not ported: they are the tracker's project. `WavSaveSource` (with `make_header`), `BufferSaveSource` and the settings debounce (`kSettingsDebounceMs` 1500, `poll`, `power_begin`) are copied into the kit's files service | N, bodies verbatim |
| `formats/src/settings_*` (349), `model/settings.*` (221), `docs/formats/o3-settings.md` (196) | not ported (the tracker's 26 device fields). The container shape (FORM, a SETS chunk of key and f32 entries, CRC) is the model for the kit's settings file | N |
| `tools/check_layers.py` rules 6 and 8 | `platform/mk66/tools/check_writers.py` | N (adapted, section 3) |
| **Audio in, radio, taps (items 3-5)** | | |
| `audio/include/o3os/audio/{taps.h (359), tap_mem.h (34), kernels.h (157), kernels_impl.h (450), simd.h (146), test_signal.h (137), stem_map.h (57)}`, `audio/src/{taps.cpp (303), kernels.cpp (170), stem_map.cpp (141)}` | `platform/mk66/audio/` | I + audio move |
| `platform/mk66/include/o3os/platform/mk66/si4703.h` (224), `src/si4703.cpp` (514) | `platform/mk66/audio/` (pure, built with the audio library; the include path stays `<o3sdk/platform/mk66/si4703.h>`, as `midi_config.h` did) | I |
| `platform/mk66/src/capture.cpp` (130), `include/.../capture.h` (30) | `platform/mk66/src/`, `include/` (the image's `CaptureHw`) | I |
| `platform/mk66/src/audio.cpp` (482): `apply_input`, `audio_input_apply`, `adc_line_` | o3-sdk `audio.cpp`/`audio.h` | restoration (`sgtl5000.h` already has `line_input_plan`, `mic_input_plan` and the gain plans) |
| `hal/include/o3os/hal/capture.h` (140) | already in o3-sdk, checked identical | - |
| `app/include/o3os/app/recorder.h` (423), `recorder.cpp` (823), `record_view.h` (105), `screens/record_screen.*` (551), `record_actions.cpp` (494) | not ported (the tracker's Record screen). The reference for the source-switch settle (`kSourceSettleMs` 200) and the retry of a failed I2C write (500 ms) | N |
| `audio/include/o3os/audio/graph.h` (607), `meters.*` (250), `app_tap.h` | not ported (the tracker's mixer graph): the engine has its own peaks | |
| **USB (item 6)** | | |
| `platform/mk66/usb/src/msc_bridge.cpp` (92), `include/.../msc_bridge.h` (58), `platform/mk66/include/.../disk_share.h` (323) | `platform/mk66/usb/`, `platform/mk66/include/` | I |
| `platform/mk66/usb/src/usb0.cpp` (612) DISK block | o3-sdk `usb0.cpp`/`usb0.h` | restoration of the M2b-R cuts, line for line |
| `platform/mk66/power/src/power_host_mk66.cpp` MSC lines | o3-sdk `power_host_mk66.cpp` | restoration of M2b2 P's cuts |
| `platform/mk66/tools/o3tone.py` (425) | `platform/mk66/tools/` (plays a tone into the return stream) | I |
| console handlers `usb profile/return/map`, `disk`, `radio`, `mon` (`console.cpp` 1537, `command_table.h`) | `platform/mk66/console/` | N, handler bodies verbatim |
| **Leftovers (item 7)** | | |
| `app/include/o3os/app/brightness.h` (35) | `kit/app/` | I |
| `platform/mk66/usb/src/screenshot_service.cpp` (242), `include/.../screenshot_service.h` (114) | `platform/mk66/usb/` | I |
| `hal/screenshot_file.h` (99) | already in o3-sdk, checked identical | - |

Tests:
- Ported identical (I or X as their subject): `atomic_save_test` (608), `save_fatfs_test` (582), `save_guard_test` (806), `dir_card_test` (440),
  `fat_fs_storage_test` (303), `wav_test` (1174), `dir_scan_test` (470), `text_entry_test` (164), `mk66_save_writer_test` (483),
  `audio_kernels_test` (744), `audio_test_signal_test` (223), `audio_stem_map_test` (140), `mk66_si4703_test` (654),
  `mk66_usb_msc_bridge_test` (597), `mk66_disk_share_test` (608), `mk66_screenshot_service_test` (655), `screenshot_file_test` (75).
- N, the cases copied: `audio_taps_test` (398) and `audio_capture_test` (387), only their cases that do not drive the tracker's Graph;
  `audio_usb_return_test` (413), only its mixing rules.
- Not ported: `save_recover_test` (250; it needs `save_service`), `audio_taps_thread_test` (`app_tap`, `meters`).

Already in o3-sdk and identical (checked by diff after the rename): `core/job.h` (the sliced job runner), `core/byte_io.h`, `core/learn.h`,
`hal/storage.h`, `hal/storage_write.h`, `hal/save_guard.h`, `hal/capture.h`, `hal/screenshot_file.h`, `i2c_bus.h`.

## 3. The app file API (item 1)

This is the biggest gap and the first wave.

**What an app gets.**
- Its own folder `Apps/<app>/`, with one level of project folders under it.
- Saves that never leave a half-written file: a power cut, a pulled card or a failing write at any step leaves the old file or the new one,
  whole. This is track3r-os's atomic save (`atomic_save.h`): write a temp file, read it back with a CRC, then rename. The old version is kept
  as `.bak`.
- Loads that fall back to the `.bak` after a cut.
- Sliced folder listings, and the WAV reader.
- The kit's own settings, saved for it.

All of it runs in the main loop, in slices; the render never sees the card.

**Card layout (D1, D2).**

| Path | Kind | Rule |
|---|---|---|
| `Apps` | folder | made when absent |
| `Apps/<app>` | folder | `<app>`: the app's id from the build (D6), `valid_name`, at most 15 bytes |
| `Apps/<app>/<stem>.<ext>` | Target | Replace (the old one kept as `.bak`) or NewOnly. `<stem>` is a `valid_name`. `<ext>` is on the allowlist `o3a` (an app's own data), `o3s` (settings), `wav`, `mid`, `syx`, `txt`, `json`, `csv`. Never `ptf`, `hex` or `o3pj` |
| `Apps/<app>/<stem>.<ext>.bak` | Backup | the previous version |
| `Apps/<app>/<project>` | folder | a project folder, `valid_name` |
| `Apps/<app>/<project>/<stem>.<ext>` and `.bak` | Target, Backup | as above, one level down |
| `Apps/<app>/<project>/.deleted` | Marker | DELETE (D4): the listing hides the folder; nothing is removed |
| `Apps/<app>/system.o3s` | Target | the kit's settings for this app (D5) |
| `<folder>/_o3save.tmp` | Temp | the file a save is written to before it replaces anything |
| `Samples/<stem>_NNN.wav` | Target | track3r-os's existing row, NewOnly: a take the user wants in any firmware |

- The longest path fits the guard's 96-byte cap: "Apps/" + 15 + "/" + 31 + "/" + 31 + ".json.bak" is 94 bytes with the NUL.
- o3-sdk never writes track3r-os's `Projects/`, `Settings/`, `instruments/` or `AppExport/`.
- `Firmware/` stays the update path's alone: its own `path_guard`, identical. No path is in both guards' tables, so no file can be written by
  both writers.

**The guard (D2).**
- `hal/save_guard.h` stays identical. A new header `kit/hal/include/o3sdk/hal/app_guard.h` (N) answers with the same names the ported code
  calls: `classify`, `may_make_dir`, `may_create`, `may_create_marker`, `may_rename`, `may_remove`, `join`, `Place`, `kPathCap` (96, the same
  constant), `kNameMax` and `detail`.
- Its table is `save::classify` plus the `Apps/` rows, so the ported tests run unchanged against it.
- Four ported files change one line each, their alias, and are class X: `fat_storage_writer.cpp`, `atomic_save.cpp`, `dir_card.cpp` and
  `mem_card.h`.
- A new test, `app_guard_test`, walks the `Apps/` rows and proves that the kit's files service only forms `Apps/<app>/...` and `Samples/`
  paths.

**The contract** (`kit/app/include/o3sdk/app/files.h`, N; `Context::files()`, main loop only):

```cpp
class Files {
 public:
  enum class Card : std::uint8_t { None, Ready, ReadOnly, Unsupported, OnComputer, InUse };  // InUse: the update path holds it
  enum class Mode : std::uint8_t { Replace, NewOnly };
  struct Entry { char name[32]; std::uint32_t size; bool folder; bool recover; };  // recover: only the .bak is left (a cut save)
  struct Status { enum Phase : std::uint8_t { Idle, Running, Done, Failed } phase; std::uint16_t permille; const char* text; };
  bool available() const;  Card card() const;  bool busy() const;
  // The app's folder is Apps/<id>/; `folder` is "" or one project folder; `name` is "<stem>.<ext>". false: refused now (busy, a bad name, no card).
  bool save(const char* folder, const char* name, const void* data, std::size_t bytes, Mode mode = Mode::Replace);  // `data` unchanged until Done
  bool save(const char* folder, const char* name, SaveSource& source, Mode mode);       // streamed (atomic_save.h)
  bool save_wav(const char* folder, const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate,
                const char* stem);                                                         // NewOnly <stem>_NNN.wav; folder "Samples": the card's Samples/
  bool load(const char* folder, const char* name, void* dst, std::size_t cap);            // whole file, the .bak when the file is gone
  bool load(const char* folder, const char* name, core::ByteSink& sink);                  // streamed, a slice a pass (a WAV through formats/wav.h)
  bool list(const char* folder, Entry* out, std::size_t cap);                             // DirScan, a few entries a pass, sorted
  bool remove_folder(const char* folder);                                                 // the .deleted marker (D4)
  Status status() const;  std::size_t listed() const;  std::uint32_t loaded() const;
  void cancel();                  // repairs at once (T removed, B moved back)
  std::uint32_t generation() const;   // moves when the card goes out, comes in or goes to a computer: re-list
  void set_quiet(bool quiet);     // no system strip for this app's jobs
};
```

The kit adds a small container, `kit/formats/chunk_file.h` (N, D7), for an app that wants versioned data: IFF `FORM <type>`, chunks, and a
`CRC ` trailer, written into a buffer and read from one. An app may also save raw bytes.

**Never blocking.**
- `Runtime::poll()` runs `core::JobRunner` once a pass, with track3r-os's budget `kJobBudgetUs` = 2000.
- Each step does a bounded amount of card work:
  - a write chunk is the budget times `kBytesPerUs` (2), clamped to 512..8192 bytes;
  - a read-back is 2 KiB;
  - a load reads 16 KiB a pass (`kLoadBytesPerPass`);
  - a directory operation counts as one step.
- A card call blocks only the main loop, for the card's time. A FAT sync can take longer than the budget. `JobStats::max_step_us` measures
  it, and the console's `files` command shows it.
- The render is an interrupt and never waits on the main loop. Under a light render, the main-loop starvation guard does not shed for a long
  pass (`pipeline.h`, OS-MAINGUARD).
- One card job runs at a time: the writer has one file open, and the read side has two handles (`kMaxOpenFiles`). The kit's settings save
  waits its turn behind an app's job.

**The update path and `check_writers.py`.** Today the update path is the card's only writer. After this plan there are two writers, with
disjoint paths, that never run at once:
- `check_writers.py` rule 1's users become `update/src/fat_firmware_store.cpp` and `save/src/fat_storage_writer.cpp` (track3r-os rule 6 without
  the bench).
- A new rule 4 (track3r-os rule 8) names who may include `storage_write.h`, `save_guard.h` and `app_guard.h`: the guards, the writer,
  `atomic_save.cpp`, the kit's files service, the host cards, `main.cpp` and later the screenshot service.
- They exclude each other through the card claims (`card_claims.h`):
  - The files service refuses to start while `appimg::firmware_update_active()`, and shows `Card::InUse`.
  - The update path refuses `fw put` and the FIRMWARE install while `appimg::storage_in_use()`. That becomes `files.busy()`. The identical
    `fw_host_mk66.cpp` already asks it.
  - The system Config's FIRMWARE refuses with "SAVING" before it asks `App::update_blocker`.

**Card removal.**
- `CardSwap` releases the volume 250 ms after the pull.
- The writer pins the card it began on (`begin_save`), so a pulled card, even one put back, fails the job with NoCard. Repair is best effort.
- The next save of that file heals it: the `.bak` goes back as the file, and the stale temp file is removed.
- `Runtime::card_changed()` also tells the files service: `generation()` moves, and a load or a listing in progress fails.

**Power.** `appimg::power_save_busy` becomes `files.busy()`, `power_save_committing` the job's `past_write()`, and `power_save_cancel` becomes
`files.cancel()`. The ported power service waits up to 5 s (`kSaveWaitMs`) for a save, and the settings are flushed at once when the off
begins.

**No card.** An image without a card (`O3SDK_MK66_CONSOLE=OFF`, a test rig) has `Platform::storage` and `Platform::writer` null. Then
`available()` is false, every call is refused with "NO CARD", and the app runs.

**Host fake and simulator.**
- `<app>_sim --card DIR` uses `DirCard` (I) over a host directory.
- Scenario commands: `card out`, `card in`, `card protect on|off`, and `reboot` (the runtime restarted on the same card, which proves the
  settings survive).
- Tests use `MemCard` with `cut_at` (a power cut at every write) and `fat_fs_storage` (the real writer on a FAT32 image).
- HELLO gets SAVE and LOAD of its riff, with the scenario `apps/hello/scenarios/files.txt`.

**Unit checks** (the tester session over USB; section 7 has the rest):
- HELLO saves through injected keys. Then `files` shows Done, the bytes, `max_step_us` and the measured write speed.
- `files crc Apps/hello/riff.o3a` (N) matches the simulator's file.
- `ls Apps/hello`.
- `cpu` and `guard` show 0 underruns and 0 sheds over 20 scripted saves.
- `fw put` during a save is refused; the FIRMWARE list says SAVING.
- The theme is changed, `reset` is sent, and `state` shows it kept.
- A hands session: a card pulled mid-save (the next save heals it), the write-protect switch, and the Power switch held during a save.

## 4. The system Config: AUDIO, persistence and the rest (item 2)

**What an app gets.**
- Sections in this order (D18): FIRMWARE, AUDIO, MIDI, CARD, DISPLAY, INFO, then the app's own pages.
  - MIDI exists (slice K, `system_midi_page.cpp`).
  - AUDIO: INPUT, GAIN, MONITOR, then FREQ, REGION and SEEK (F5/F6, as track3r-os's RADIO page) while INPUT is RADIO; then USB AUDIO, PROFILE,
    RETURN and RETURN LEVEL. A USB change asks F1 twice: the port reconnects, as in track3r-os.
  - CARD: the volume, the files service state, CARD AS DISK and READ ONLY.
  - DISPLAY gains BRIGHTNESS and PAD LIGHTS (`brightness.h`: duty 20, 50 or 128; the pads in thirds).
  - INFO gains the last crash record.
- A section whose driver is absent is hidden, as now.
- `App::config_pages()` becomes real: up to eight items a page, each a value (a `ParamDesc` with get and set, main loop) or an action (run
  after a confirming hold). The kit stores the values.
- MIDI learn (`core/learn.h`, already in the kit): `Midi::learn(unit, param)` binds the next CC. The bindings are stored with the settings.

**Persistence (D5).**
- The kit's settings live in `Apps/<app>/system.o3s`: the theme, `hal::midi::Config`, the audio-in choice, the USB layout and stem map,
  brightness, pad lights, the app's page values and the learn bindings.
- Format: `FORM 'O3SK'`, a `SETS` chunk of (u16 key, f32 value), `APPV` (the app's page values), `LRNB` (the bindings), then `CRC `; at most
  2 KiB.
- Unknown keys and chunks are skipped and counted. A newer major version is refused, and the defaults are kept.
- When it is written: 1.5 s after the last change (track3r-os's debounce, verbatim), at once when the power-off begins, and later if the card
  is on a computer or held by the update path.
- When it is read: at boot, in `main.cpp`, after the mount and before USB starts (one read, bounded). An app's `setup()` sets the defaults;
  the user's saved values win.
- A bad file means the defaults. A saved USB layout that is not `usb_audio::supported` means the boot layout. CARD AS DISK is never restored
  at boot (D14).
- The spec is `docs/formats/o3sdk-settings.md`.

**Host fake and simulator.** The same card fake (`--card`, `reboot`). The scenario `apps/hello/scenarios/config_pages.txt`: an app page, an
AUDIO row, brightness, a learn.

**Unit checks.**
- Over USB: the sections by `--shot` and `state` (`section=audio`); a MIDI channel set, `reset`, then kept; a learn bound by a CC the
  computer sends over USB MIDI (`amidi`), then kept after `reset`.
- A hands session: brightness and pad lights by eye.

## 5. Audio in (item 3) and the FM radio (item 5)

**What an app gets.** `Context::audio_in()` (N, over `hal::capture::CaptureHw`, identical):
- `select(Source, gain_percent)`, `set_gain()`, `source()`, `gain()`, `monitor(level)`, `peaks()`.
- The sources are `hal::capture::Source`: LineMix, LineLeft, LineRight, MicLow, MicHigh, Radio and Usb.

The input reaches the render as now: `in_l`/`in_r` of a unit, and `BlockContext::input_l()` of a hook. While the source settles (200 ms,
track3r-os's `kSourceSettleMs`) the engine holds those planes silent.

The monitor (D9):
- The input joins the master bus before the master inserts, ramped. It is off by default; `on` is 70, as track3r-os.
- The input peaks are computed only while an app or the AUDIO section asks for them.

`Context::radio()`: `power(bool)`, `region()`, `tune(freq)`, `seek(up)` and `status()` (`RadioStatus`: state, frequency, RSSI, stereo
flag, chip id, failure code). `CaptureHw::service()` runs once a pass of the main loop and does at most one I2C transfer. RDS is not in this
round (D10).

**Contract and source.**
- `hal/capture.h` (I, exists).
- `capture.cpp` (I): the SGTL5000 input plans over I2C0, the analogue switch PTE26 (HIGH line and mic, LOW radio, polarity assumed) and the
  Si4703.
- `audio_input_apply` (restored). `si4703.{h,cpp}` (I, tested against a model of the chip).
- The I2C bus is shared with the codec and the button scanner, so every call is main loop only. A failed write is retried every 500 ms.
- Engine side (kit/dsp, N): `Engine::set_input_gate(bool)`, `set_input_monitor(float)`, `input_peaks()`. Each is one branch a block when
  unused.

**Host fake and simulator.**
- `platform/host/fake_capture.h` (N) records the calls and scales the input by the gain.
- `input tone <hz> [dbfs]`, `input wav <file>`, `input off`: what the codec hears.
- `radio stations <f...>` and `radio absent`: a seek finds the listed stations, and a tuned station is a tone at its own pitch in the input
  while the source is Radio.
- Scenario `apps/hello/scenarios/audio_in.txt`. HELLO gets an input-through with the monitor.

**Unit checks.**
- Over USB, with a loopback cable from the main out to the line in, placed once by the maintainer (as the MIDI DIN loopback):
  - HELLO plays a pad with INPUT LINE and the monitor on, and `o3audio.py record --channel 7 --not-silent` shows the input.
  - INPUT RADIO with the same loop: the tone is gone. That settles the PTE26 polarity.
- `radio` answers state and chip id. The first answer decides whether FM exists on this board: Absent or Failed ends the radio row as "not on
  this unit".
- With an antenna: `radio` seek and RSSI.
- `cpu` with the monitor on.
- A hands session: the microphone by voice, the radio by ear.

## 6. Recording into SDRAM and to the card (item 4)

**What an app gets.** `Context::recorder()` (N, over `hal::record::Recorder`, a new contract):
- `arm(Take)`: a buffer the app owns, `O3SDK_BIG int16_t take[...]` (D8; no arena); capacity; mono (Mix, Left or Right) or stereo; Align
  Free, Step, Bar, Pattern or Level; threshold and pre-roll for Level; `auto_steps`.
- `stop_at(Align)`, `disarm()`, `state()`, `frames()`, `settle()` (put a level take's ring in order), `release()`, `overrun()`.
- `set_tap(Input | Master)`: the codec input (or the USB return, when the source is Usb), or the engine's output, for a resample.

Steps are the transport's sixteenths: Bar is 16 of them, and Pattern is the app's pattern length (default 16). A take is saved with
`Files::save_wav` (NewOnly, the next free `_NNN`), in the app's folder or in `Samples/`.

**Contract and source.**
- `RecTap` (I, `taps.h`) and its kernels run in the render. Idle, it costs one load a block.
- The record service (`platform/mk66/audio/src/record_service.cpp`, N, pure) wraps the render callback, as `usb_audio::wrap` does:
  1. it calls the runtime's block;
  2. it calls `on_step` and `on_stopped` for the sixteenths and the stop that fell in the block (from `Engine::block_frame()`);
  3. it pushes the tapped planes.
- The buffer is in SDRAM. RecTap's hot kernels go to SRAM_L (`O3SDK_AUDIO_HOT`), inside `kRamfuncLBudget`.

**Host fake and simulator.** The simulator wraps its block callback with the same record service: the same code as the unit; only the codec
is faked (D8). Scenario `apps/hello/scenarios/record.txt`: arm at a bar, the input tone, stop, then the take saved to the card directory.
HELLO gets REC.

**Unit checks.**
- Over USB, with the loopback tone:
  - a take: `rec` (N) shows state, frames and overrun 0;
  - `files crc` of the WAV; the frame count against the time asked;
  - a Level start; a Bar start against HELLO's riff;
  - a 60 s take;
  - `cpu` while recording.
- Takes go to `Apps/hello/`, not `Samples/`, until the disk mode lets the tester remove them (section 10).

## 7. USB audio: stems, the return, the card as a disk (item 6)

**What an app gets.** `Context::usb_audio()` (N, over a new `hal::usb_audio::Device`):
- `profile()` and `request_profile(P2 | P8 | P10 | P11, duplex)`: the port reconnects.
- `set_stem(channel, slot_mask)`: track3r-os's `StemMap`, with o3-sdk's slots 1..8 as its T1..T8 (post-fader, pre-pan, mono), the sends
  and the master as its sources.
- `return_level(float)` and `streaming()`.

P8 stays the boot profile, with the master on 7-8 as today, so a DAW's routing is unchanged.

The return (D11):
- It is added after the render at RETURN LEVEL, as track3r-os does: outside the master inserts and never captured.
- When the audio-in source is Usb, it is also the engine's input planes, so a unit, a hook or the recorder can take it.

The card as a disk: CARD AS DISK in the CARD section (F1 twice), READ ONLY, eject. While the computer has the card, the files service shows
`Card::OnComputer` and saves are refused with "CARD ON COMPUTER - EJECT FIRST". The settings stay dirty until the eject.

**Contract and source.**
- `StemTap` and `StemMap` (I) replace the master-only tap inside `usb_audio.cpp` (N).
- The engine stages the selected slots' mono blocks only while the IN stream runs (kit/dsp, N, slice SE).
- `StemPool` is about 18 KB [est] in SRAM_L's free 32 KiB, as track3r-os's `tap_mem.h`.
- The return goes through the existing `drain_return()` seam.
- The disk mode is the `usb0.cpp` DISK block, `msc_bridge` and `disk_share.h` (I and restoration), with the console's `disk` and
  `usb profile/return/map` (bodies verbatim).

**Host fake and simulator.**
- `fake_usb_audio.h` (N) runs the real `StemTap`.
- `usb return tone <hz>|wav <file>|off`: the computer plays.
- `usbwav <name>`: the IN stream written as a multichannel WAV.
- `card disk on|off`.
- Scenario `apps/hello/scenarios/usb.txt`.

**Unit checks.** Over USB:
- `usb profile 10 duplex`, then 8.
- A slot mapped to S1, then `o3audio.py record --channel 1 --not-silent`.
- `o3tone.py` plays 1 kHz into the return; `--channel 7` shows it mixed.
- With the source Usb, a take of the return.
- `cpu` with stems and the return on.
- `usb mode disk`: the test host mounts the card under `/media` and reads `Apps/hello/` against the simulator's files. It removes the files
  the sessions put there, and only those. Then eject and take back.
- P11 is measured for the first time. P10 duplex is tried on a direct port, not a hub.

## 8. How the pieces sit (kit and platform)

| | kit (core, hal, formats, dsp, ui, app) | platform/mk66 | platform/host, sim |
|---|---|---|---|
| Files | `hal/app_guard.h`; `formats/*`; `app/{atomic_save, dir_scan, text_entry, files, settings}`; `Context::files()`; `Platform::{storage, writer}` | `save/` (writer), claims, appimg, boot settings read, console `files` | `DirCard`, `MemCard`, `fat_fs_storage`; `--card`, `card ...`, `reboot` |
| Config | sections AUDIO, CARD, the app's pages, DISPLAY rows, INFO crash, MIDI learn | `Platform::backlight` bound to the FT812's `REG_PWM_DUTY` | the pages' goldens |
| Audio in, radio | `Context::{audio_in, radio}()`; `Engine::{set_input_gate, set_input_monitor, input_peaks}`; `Platform::capture` | `capture.cpp`, `si4703`, `audio_input_apply`; console `radio`, `mon` | `FakeCapture`; `input ...`, `radio ...` |
| Recording | `hal/record.h`; `Context::recorder()`; `Platform::recorder` | `taps`, `record_service`, the wrap; console `rec` | the same record service; HELLO's REC |
| USB audio, disk | `hal/usb_audio.h`; `Context::usb_audio()`; `Platform::usb_audio`; engine stem staging | `usb_audio.cpp` with `StemTap`, `msc_bridge`, `disk_share`, the DISK block; console `usb ...`, `disk` | `FakeUsbAudio`; `usb return ...`, `usbwav`, `card disk ...` |

Every new `Platform` pointer is null by default: none of it exists, and the runtime behaves as before (test rigs, the M2 image).

## 9. The leftovers (item 7)

- **Crash record** (D15). A test app, `apps/crashtest` (N, never released), has app commands `fault hard|bus|usage|render`; the render case
  faults inside a clocked hook. Over USB: `fault ...`, the unit resets, then `crash` shows the record (PC, LR, CFSR, module, IPSR) and `stage`
  shows the reset reason. A hands session reads the blink code by eye.
- **Widgets.** `kit/ui/list_view.h` (N): a scrolling list with a cursor, values and a scroll mark. `kit/ui/keyboard.h` (N): draws
  `TextEntry`'s 42 keys from plain arguments. The kit/app glue (a file picker over `Files::list`, a name entry) comes with CP.
- **One-command install** (D16). `tools/o3install.py <ptf>` (N) does put, the `first-ptf` check, update and `--expect-version`, and stops at the
  first failure with `o3console.py`'s exit codes. `o3console.py` stays identical.
- **Toolchain fetch** (D17). `tools/fetch_toolchain.py` (N) downloads Arm GNU 15.3.rel1 for x86_64, checks a pinned SHA-256 (taken from Arm's
  published checksum) and unpacks it under `.toolchains/` (git-ignored). `tools/ci.sh` looks there first, then `../track3r-os/.toolchains/`.
- **Screenshot to card.** Shift + Power, through `screenshot_service` (I) and the writer: `Screenshots/SHOT_nnnn.bmp`, track3r-os's row.
  `power::device::take_screenshot_request` already exists.
- **Header.** `App::header()` (N, default as now) lets an app without a sequencer drop the tempo and the transport badge (`NEW-APP.md`
  section 13).
- **Setup errors.** `Engine::setup_errors()` goes into `state` (`setup_errors=<n>`; FRAMEWORK section 4's follow-up).
- **Template.** `templates/app` saves and loads its pattern and keeps a setting, so a new app starts with the file API in hand.

Checks that are the unit's, with no code: the FT812 fill rate (`apps/fillrate`) and the shot-versus-simulator colour order. Both go on the
hands session's list.

## 10. Slices

M2B-PLAN section 4's rules apply:
- Stay in your files. An additive one-line change elsewhere is listed.
- Build only your own targets, in `build-m2c-<slice>-{host,mk66,asan}`.
- Never flash. Do not commit.
- A defect found in ported code goes in the report's "defects seen" list and is not fixed.

At most two agents at a time, with no shared file. The A1 column owns `kit/app`, `sim`, `platform/host`, `apps/hello`, `main.cpp` and
`console/` in its wave, so those are serialised across waves; A2 never touches them.

| Wave | A1 | A2 |
|---|---|---|
| 1 | **FS** the save machinery (port) | **AP** taps, kernels, stem map, Si4703 (port, pure) |
| 2 | **FA** the app file API, kit settings, the card in the simulator, the image's wiring | **SE** engine: input gate, monitor, input peaks, stem staging |
| 3 | **AI** audio in and the radio, AUDIO's input rows | **LW** list and keyboard widgets; `o3install.py`, `fetch_toolchain.py` |
| 4 | **RC** recording | **CT** the crash test app; **TP** the template |
| 5 | **U** USB audio (stems, return, profiles) and the disk mode, AUDIO's USB rows, CARD | integrator: the hw-session page for waves 3-4 |
| 6 | **CP** app Config pages, DISPLAY rows, INFO crash line, MIDI learn, header, setup errors, screenshot to card, the file picker and name entry | integrator: docs, gates, the hw-session page for waves 5-6, o3-303 rebuild |

The integrator also writes FA's hw-session page between waves 2 and 3.

| Slice | Owns |
|---|---|
| **FS** | `kit/formats/**` (new, with its `add_subdirectory` line in the root `CMakeLists.txt`); `kit/app/{include,src}` new files only (`atomic_save`, `og_timing`, `dir_scan`, `dir_index`, `text_entry`) and their lines in `kit/app/CMakeLists.txt`; `kit/hal/include/o3sdk/hal/app_guard.h`; `platform/mk66/save/**` (new, its own `tests/` and CMake, like `midi/`); one line in `platform/mk66/CMakeLists.txt` (`add_subdirectory(save)`); `platform/host/**` new files (`dir_storage`, `dir_card`, `host_card`, `fake_storage`, `fat/`) and `platform/host/CMakeLists.txt`; `tests/` (the I tests, `app_guard_test`, `support/{mem_card,save_source,memory_io}.h`); `platform/mk66/tools/check_writers.py` |
| **AP** | `platform/mk66/audio/**` (taps, `tap_mem`, kernels, simd, `test_signal`, `stem_map`, `si4703`; its CMake gains `usb`); `platform/mk66/tests/` (the ported audio and si4703 tests) and `platform/mk66/tests/CMakeLists.txt` |
| **FA** | `kit/app/**`; `kit/formats/chunk_file.*`; `platform/host/**`; `sim/**`; `apps/hello/**` (SAVE/LOAD, `files.txt`); `platform/mk66/src/main.cpp`; `platform/mk66/console/**` (`appimg.cpp`, `files` command); the image block of `platform/mk66/CMakeLists.txt`; `docs/formats/**` (new); tests `app_files_test`, `app_settings_test`, `chunk_file_test`, `sim_files_test` |
| **SE** | `kit/dsp/**`; tests `dsp_input_test`, `dsp_stems_test` |
| **AI** | `platform/mk66/src/capture.cpp`, `include/.../capture.h`; `audio.h`/`audio.cpp` (`audio_input_apply` only); `kit/app/**` (audio in, radio, AUDIO section); `platform/host/**` (`fake_capture.h`); `sim/**`; `apps/hello/**` (`audio_in.txt`); `main.cpp`; `console/**` (`radio`, `mon`); the image block of `platform/mk66/CMakeLists.txt`; tests `app_audio_in_test`, `host_fake_capture_test`, `audio_capture_test` (N cases) |
| **LW** | `kit/ui/**` (`list_view`, `keyboard`); `tools/{o3install,fetch_toolchain}.py` and their self-test lines in the root `CMakeLists.txt` tools block; `tools/ci.sh` (the `.toolchains/` lookup); `.gitignore` (`.toolchains/`); test `ui_list_test` |
| **RC** | `kit/hal/include/o3sdk/hal/record.h`; `platform/mk66/audio/{include,src}/.../record_service.*`; `kit/app/**`; `platform/host/**`; `sim/**`; `apps/hello/**` (REC, `record.txt`); `main.cpp`; `console/**` (`rec`); tests `record_service_test`, `app_recorder_test`, `sim_record_test` |
| **CT** | `apps/crashtest/**` (new) and its line in the root `CMakeLists.txt` apps block |
| **TP** | `templates/app/**` |
| **U** | `platform/mk66/usb/**` (`usb_audio.*` rework, `usb0.*` restoration, `msc_bridge.*`); `platform/mk66/include/.../disk_share.h`; `platform/mk66/power/src/power_host_mk66.cpp` (restoration); `kit/hal/include/o3sdk/hal/usb_audio.h`; `kit/app/**` (USB rows, CARD section); `platform/host/**` (`fake_usb_audio.h`); `sim/**`; `apps/hello/**` (`usb.txt`); `main.cpp`; `console/**` (`usb profile/return/map`, `disk`); `platform/mk66/tools/o3tone.py`, `platform/mk66/tools/hil/hello_usb.json`; tests `mk66_usb_msc_bridge_test`, `mk66_disk_share_test`, `mk66_usb_audio_tap_test` (rework), `app_usb_audio_test`, `sim_usb_test` |
| **CP** | `kit/app/**` (pages, `brightness.h`, learn, header, `state`, the file picker and name entry); `platform/mk66/usb/{include,src}/.../screenshot_service.*`; `platform/host/**`; `sim/**`; `apps/hello/**` (`config_pages.txt`); `main.cpp` (backlight, screenshot); `console/**` if needed; tests `app_config_pages_test`, `app_midi_learn_test`, `mk66_screenshot_service_test`, `screenshot_file_test` |
| integrator | `docs/**` (but `docs/formats/`), `README.md`, `CLAUDE.md`, `AGENTS.md`, the hw-session pages, `fw-gate-check`, o3-303's rebuild and gate |

| Slice | Exposes |
|---|---|
| FS | `o3sdk::formats::{crc32, Progress, wav::*, sample_channels}`; `app::{AtomicSave, SaveSource, HeldCheck, DirScan, DirIndex, TextEntry}`; `hal::storage::app_guard::*`; `platform::mk66::save::FatStorageWriter`; host `DirCard`, `DirStorage`, `FatFsStorage`, `MemCard` |
| AP | `platform::mk66::audio::{RecTap, StemTap, StemPool, StemMap, SampleRing, TapMem}` and kernels; `platform::mk66::si4703::*` |
| FA | `app::Files`, `Context::files()`, `Platform::{storage, writer}`, the kit settings store (keys appended, never reused); `formats::chunk::{Writer, Reader}`; console `files [crc <path>]`; `state` pairs `card=<state> files=<phase>` |
| SE | `Engine::{set_input_gate, set_input_monitor, input_peaks, set_stem_slots, stem_block}` |
| AI | `app::AudioIn`, `app::Radio`, `Context::{audio_in, radio}()`, `Platform::capture`; the AUDIO section; console `radio [retry]`, `mon [off\|on\|0..100]` |
| LW | `ui::ListView`, `ui::keyboard()`; `o3install.py`, `fetch_toolchain.py` |
| RC | `hal::record::Recorder`, `Context::recorder()`, `Platform::recorder`; `record_service::wrap()`; console `rec` |
| CT, TP | `crashtest.ptf` (session-only); the template's files example |
| U | `hal::usb_audio::Device`, `Context::usb_audio()`, `Platform::usb_audio`; the CARD section; console `usb profile/return/map`, `disk [share [ro]\|eject]` |
| CP | `App::config_pages()` with items, `App::header()`, `Midi::learn()`, `Platform::backlight`; `state` `setup_errors=`; `app::FilePicker`, `app::NameEntry` |

| Slice | Host targets and tests | mk66 | Only the unit (tester over USB unless noted) |
|---|---|---|---|
| FS | `formats app platform_host platform_mk66_save(_hdrcheck) o3sdk_tests mk66_save_tests`; the I tests above plus `app_guard_test`; `mk66_writers` (rule 1 with two users, rule 4); ASan | `platform_mk66_save` | none (nothing new in an image) |
| AP | `platform_mk66_audio(_hdrcheck) mk66_audio_tests`, ASan | `platform_mk66_audio`; `hello_ramfunc` unchanged | none |
| FA | `app sim platform_host o3sdk_tests hello_sim`; `files.txt` (save, card out mid-save and NO CARD, card in, the heal, load, `reboot` keeps theme and MIDI channel); `MemCard` cut at every write of a HELLO save; settings read back with an unknown key and a newer minor | `hello`, `ctest -L mk66`, `mk66_writers` | section 3's list; hands: card pull, write protect, Power during a save |
| SE | `dsp o3sdk_tests`; the gate fades and never clicks; the monitor sums; stems match the slot's post-fader mono to the sample; a disabled feature costs no render work (counted) | `hello` | `cpu` deltas come with AI and U |
| AI | `app sim o3sdk_tests hello_sim`; `audio_in.txt` (tone in, the switch settles silent, gain, monitor on in the WAV, radio seek finds the listed stations) | `hello`, `ctest -L mk66` | section 5's list; hands: mic, radio by ear |
| LW | `ui o3sdk_tests`; the tools' self-tests (offline: a fake download dir, a fake console) | - | none; the next install uses `o3install.py` |
| RC | `app sim o3sdk_tests hello_sim mk66_audio_tests`; `record.txt`; aligned starts within one sample of the step; a level take's pre-roll in order; overrun counted at a full buffer | `hello`, `ctest -L mk66` (`hello_ramfunc` within budget) | section 6's list |
| CT | `crashtest_sim` builds; its commands registered | `crashtest`, gated | each fault, `crash`, `stage`; hands: the blink code by eye |
| TP | `ctest -L template` (host and mk66) | the template's image | none |
| U | `usb platform_mk66_usb mk66_usb_tests mk66_console_tests app sim o3sdk_tests`; `usb.txt` (P10 stems in `usbwav`, a return tone mixed and, as the source, recorded; disk on refuses a save) | `hello`, `ctest -L mk66` (`hello_usb`) | section 7's list |
| CP | `app sim o3sdk_tests hello_sim`; `config_pages.txt`; the screenshot service on `DirCard` | `hello`, `ctest -L mk66` | over USB: pages by `--shot`, INFO's crash line after CT, Shift+Power injected (`key shift down`, `power press`/`release`), then `ls Screenshots`; hands: brightness and pad lights by eye |

**Sessions.** A tester session after waves 2, 4 and 6, batched with the `hw-session` skill. The unit is shared: start from the state found
and restore exactly it (the image, and the card's files the session put there). Then one hands session with the maintainer at the end, for
everything above marked hands, plus the FT812 fill rate and the colour order.

## 11. Integration

`main.cpp`, in order; each step is added by the slice named:

```
log, crash record, display, input, power::init
Platform{..., storage = &storage::device::storage(), writer = &g_writer (FA), capture = capture_create(O3SDK_BIG slot) (AI),
         recorder = &record_service (RC), usb_audio = &usb_audio::device() (U), backlight = display::set_backlight (CP)}
rt.start()                                            App::setup: the app's defaults
render = usb_audio::wrap(record_service::wrap(rt.block_callback()))       (RC, U; the test-tone image unwrapped)
audio_device().start(render)
MIDI start (MD)
SD: init, mount, watch; claims: firmware_update_active, card_mount_pending, files_busy (FA)
settings: files.load_settings_at_boot() (FA): one read of at most 2 KiB, applied over the app's defaults; the USB layout from it if supported (U)
console attach; USB start with that layout (never DISK)
loop: wfi; console::service(); input; display; rt.poll() (the job pass, 2 ms); capture.service(now) (AI); audio_main_pass(); poll_card(); power pass
```

`appimg` (FA): `storage_in_use` becomes `files.busy()`, and the three `power_save_*` take real bodies.

IRQ priorities stay as `hal/irq.h` has them. Nothing new runs in an interrupt but the taps, which run inside the render line.

Memory [est, from track3r-os's sizes]:
- Flash: about +70 KB (files and formats 25, taps and kernels 12, radio and capture 10, recording 6, USB stems and disk 10, Config and
  screenshot 8). o3-303 goes from about 225 KB to about 295 KB of 1 MiB.
- SRAM_U: a few KB of state. The writer's FatFs objects (about 1.5 KB), AtomicSave's 2 KiB verify buffer and the settings buffer go to SDRAM
  (`O3SDK_BIG`), as track3r-os places them. Check the 32 KiB free rule again.
- SRAM_L: `StemPool` about 18 KB in the free 32 KiB. The new hot kernels must fit `kRamfuncLBudget` next to MIDI's.
- SDRAM: the app's take buffers.

## 12. Sizes

| Slice | Ported (lines) | New (lines) |
|---|---|---|
| FS | lib 4.8k (formats 1.2k, app 1.6k, writer 0.4k, host cards 2.0k), tests 5.6k | `app_guard.h` 150 + test 250, `check_writers.py` +80, CMake 100 |
| AP | 2.7k (taps 0.7k, kernels 0.9k, stem map 0.2k, si4703 0.7k), tests 1.8k + 0.4k of copied cases | CMake 40 |
| FA | about 350 verbatim inside new files (WAV and buffer sources, the debounce) | `Files` 700, settings 300, chunk container 250, kit hooks 150, sim 200, main and appimg 100, console `files` 100, HELLO 150, format docs 250, tests 900 |
| SE | - | engine 230, tests 400 |
| AI | capture 160, restorations 40, console handlers about 120 | `AudioIn`/`Radio` 250, AUDIO section 250, fake and sim 250, main 30, HELLO 60, tests 600 |
| LW | - | widgets 350 + test 250, `o3install.py` 200, `fetch_toolchain.py` 150, self-tests 150 |
| RC | - | `record.h` 120, record service 250, kit 150, sim 100, console 60, HELLO 80, tests 600 |
| CT, TP | - | 300, 200 |
| U | `msc_bridge` 150, `disk_share` 323, `o3tone.py` 425, restorations about 170, console handlers about 250, tests 1.2k | `usb_audio` rework 300, `usb_audio.h` 100, kit 150, rows and CARD 200, fake and sim 250, main 30, tests 600 |
| CP | `brightness.h` 35, screenshot 356, tests 730 | pages 300, rows 80, learn 200, header 60, picker and entry 200, main 30, tests 500 |

## 13. Risks

- **Card latency.** A FAT sync or a cluster allocation can block one main-loop pass for tens of milliseconds on a slow card. The render is
  untouched, but UI frames and input wait. The write speed constant (2 MB/s) is provisional in track3r-os too. Measure `max_step_us` on the
  unit with the maintainer's card; if a pass is too long, lower the chunk, not the safety (sync and read-back stay).
- **Two writers on the user's card.** The paths are disjoint by guard, and the claims keep them apart. The cut cases are proven on the host
  (`save_fatfs_test` cuts at every block of a FAT32 image), never on a unit, in track3r-os either. The hands session pulls a card once.
- **Test files on the maintainer's card.** The save layer never removes a file it finished. Files the sessions write stay in `Apps/hello/`
  (a folder track3r-os never reads) until U's disk mode lets the tester remove them from `/media`. Takes go there too, not to `Samples/`,
  which track3r-os shows in its sample browser.
- **The radio may not exist.** The Si4703 failed on its first unit run in track3r-os (OS-RAD1); the fix was never re-run. The board's radio
  wiring and the PTE26 polarity are unconfirmed. The first `radio` answer decides; Absent ends the FM row as "not on this unit", with the
  code kept.
- **Audio in never proven.** A real line-in signal was never seen on a unit (track3r-os fqa.11, STATUS row 72) and the mic never measured.
  The loopback cable makes the line check remote; the mic needs a person.
- **USB.** P11 was never measured; P10 duplex failed behind a hub. The stem staging and the return mix add render work: measure `cpu` with
  P10 duplex and o3-303 playing (49.9 % max today).
- **SRAM_L.** The ramfunc budget (32 KiB) now holds MIDI's and the taps' hot code; `StemPool` takes most of the free half. Watch `_ramfunc`;
  the fallback is cold paths from flash.
- **The four X files** (one alias line each). A reviewer diffs them against track3r-os after every wave.
- **Shared files across waves.** `kit/app`, `main.cpp` and `console/` are rewritten by every A1 slice; each starts by rebasing on the last.
- **o3-303.** The API only grows (new `Context` accessors, default hooks), so o3-303 rebuilds with no change. Its use of files, recording
  and stems is the later usability round.
- **Defects seen in track3r-os** (for the maintainer, not fixed):
  - `app/include/o3os/app/recorder.h:20` says the Record screen "needs the SD write layer, which this tree has not got"; `save_actions.cpp`
    saves takes through `SaveService::save_wav`.
  - OS-RAD1's fix was never re-run on a unit.

## Decisions

- **D1** Where an app writes. Decided (recommended): `Apps/<app>/`, with one level of project folders, plus `Samples/` for takes meant for
  any firmware; never track3r-os's `Projects/`, `Settings/`, `instruments/` or `AppExport/`. The maintainer may override. Alternative:
  track3r-os's own rows (`Projects/<name>/project.o3pj`, `Settings/device.o3st`), which collide with track3r-os on the same card.
- **D2** The guard. Decided (recommended): `save_guard.h` stays identical; o3-sdk's rows are a new `app_guard.h` (its table plus `Apps/`),
  and four ported files rewrite only their alias line (X). The maintainer may override. Alternative: add the rows to `save_guard.h`, which
  breaks the identity of a header track3r-os's tests walk.
- **D3** The API's shape. Decided (recommended): whole-file save and load between the card and the app's own buffer, plus track3r-os's
  streaming `SaveSource`; one card job at a time, a polled status, and the kit's strip for progress and failures. The maintainer may override.
  Alternative: callbacks into the app from the job pass.
- **D4** Delete. Decided (recommended): the `.deleted` marker on a project folder (track3r-os OS-FILES part B); firmware never removes a file
  the user made. The maintainer may override. Alternative: real removal inside `Apps/<app>/`.
- **D5** Kit settings. Decided (recommended): per app, `Apps/<app>/system.o3s`, debounced 1.5 s, flushed at power-off, read at boot before
  USB. The maintainer may override. Alternative: one file shared by every o3-sdk app.
- **D6** The folder's name. Decided (recommended): the app's CMake id (`o3sdk_app(<id>)`), passed by the build like the version, at most 15
  bytes; no API change. The maintainer may override. Alternative: a new `AppInfo::id` field.
- **D7** File contents. Decided (recommended): an IFF-shaped chunk container with a CRC (the shape of track3r-os's settings and project
  files, new code), optional for apps. The maintainer may override. Alternative: raw bytes only, each app its own format.
- **D8** Where the taps live. Decided (recommended): `RecTap`, `StemTap` and the kernels in `platform/mk66/audio` (the M2 audio-move rule);
  the record service is pure and the simulator runs it, so a take in the simulator is the unit's to the sample; take buffers are the app's
  `O3SDK_BIG` arrays. The maintainer may override. Alternative: a simulator-only fake, as `FakeMidi` was.
- **D9** The monitor. Decided (recommended): in the engine, joining the master bus before the master inserts, off by default (track3r-os:
  off, on = 70). The maintainer may override. Alternative: added after the render, with no master effects on the input.
- **D10** RDS. Decided (recommended): not this round. Reading it is a change to the ported `si4703.cpp`, made in track3r-os first and then
  ported, once the radio answers on a unit; the group decoder can be new and pure then. The maintainer may override. Alternative: an
  o3-sdk-only RDS poll, which breaks the driver's identity.
- **D11** The USB return. Decided (recommended): track3r-os's mix (after the render, at RETURN LEVEL, never captured), plus the return as the
  engine's input when the source is Usb. The maintainer may override. Alternative: track3r-os's mix only.
- **D12** Stems. Decided (recommended): `StemTap` and `StemMap` identical, o3-sdk's slots 1..8 as T1..T8, P8 at boot with the master on 7-8,
  the map in the kit settings. The maintainer may override. Alternative: the map per project, as track3r-os keeps it.
- **D13** Headphone level. Decided (recommended): it stays track3r-os's fixed, proven value; the master level is the app's. The maintainer
  may override. Alternative: an HP LEVEL row, with new codec writes and an ears check.
- **D14** Disk mode. Decided (recommended): restored as track3r-os has it, never restored at boot. The maintainer may override. Alternative:
  keep it cut; the card is then read only by taking it out.
- **D15** Crash proof. Decided (recommended): a session-only test app with app commands; no platform change. The maintainer may override.
  Alternative: a console `fault` command behind a build option.
- **D16** Install. Decided (recommended): a new `tools/o3install.py` over `o3console.py`, which stays identical. The maintainer may override.
  Alternative: a flag added to `o3console.py`.
- **D17** Toolchain. Decided (recommended): `tools/fetch_toolchain.py` with a pinned SHA-256 into `.toolchains/`. The maintainer may override.
  Alternative: the manual download only, as `NEW-APP.md` says now.
- **D18** Section order. Decided (recommended): FIRMWARE, AUDIO, MIDI, CARD, DISPLAY, INFO, then the app's pages. The maintainer may
  override. Alternative: the USB rows and the disk mode in a USB section of their own, as track3r-os has them.

### Critical files for implementation
- kit/app/src/runtime.cpp, kit/app/include/o3sdk/app/context.h
- kit/hal/include/o3sdk/hal/save_guard.h (identical; the model for app_guard.h)
- platform/mk66/src/main.cpp, platform/mk66/console/src/appimg.cpp
- platform/mk66/usb/src/usb_audio.cpp
- track3r-os: app/src/atomic_save.cpp, platform/mk66/save/src/fat_storage_writer.cpp, app/src/save_service.cpp (reference only), audio/include/o3os/audio/taps.h, platform/mk66/src/capture.cpp
