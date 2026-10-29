# The kit's settings file (`system.o3s`, type `O3SK`), version 1

Status: built with slice FA of M2c, 2026-10-29 (`docs/M2C-PLAN.md` section 4, decision D5) · Code: `kit/app/include/o3sdk/app/settings.h`, `src/settings.cpp` (the codec), `files.h` (when it is read and written), `runtime.h` (`load_settings`) · Tests: `tests/app_settings_test.cpp`, `app_files_test.cpp`, `sim_files_test.cpp`

What the system Config screen lets the user change, and the app did not ask for, kept on the card **for each app**: `Apps/<app>/system.o3s` (`<app>` is the app's CMake id, `o3sdk_app(<id> ...)`; two o3sdk apps on one card do not share it). The container is the chunk file of `o3sdk-chunk-file.md`, type `O3SK`.

```
"FORM" u32 length "O3SK"  SETS  [unknown chunks]  "CRC "
```

At most 2,048 bytes (`settings::kMaxBytes`); the file of this version with only `SETS` is 230 bytes (32 keys: 4 + 32 x 6 bytes in `SETS`); `APPV` and `LRNB` add to it only when there is something in them (at most 32 values and 16 bindings: about 600 bytes in all).

## 1. `SETS`

`u16 version` (`0x0100`: major 1, minor 0), `u16 count`, then `count` entries of `u16 key` and `f32 value` (6 bytes each). Every value is a small whole number; a value that is not one, or is out of the range of its key, is skipped.

| Key | Setting | Values |
|---|---|---|
| 1 | theme (`ui::ThemeId`) | 0 Amiga, 1 Black, 2 White |
| 2 | MIDI notes-in channel (`hal::midi::Config::channel`) | 0 (omni) .. 16 |
| 3 | MIDI out channel (`out_channel`) | 1 .. 16 |
| 4 | MIDI out ports (`out_ports`) | a mask of 1 (DIN) and 2 (USB): 0 .. 3 |
| 5 | clock out (`clock_out`) | 0, 1 |
| 6 | follow (`follow`) | 0, 1 |
| 7 | follow port (`follow_port`) | 0 DIN, 1 USB |
| 8 | transport in (`transport_in`) | 0, 1 |
| 9 | transport out (`transport_out`) | 0, 1 |
| 10 | audio in source (`hal::capture::Source`, slice AI) | 0 line mix, 1 line left, 2 line right, 3 mic low, 4 mic high, 5 radio, 6 USB |
| 11 | audio in gain, per cent | 0 .. 100 |
| 12 | audio in monitor, per cent | 0 (off) .. 100 |
| 13 | radio region (`hal::capture::Region`) | 0 Europe, 1 USA, 2 Australia, 3 Japan |
| 14 | radio station, 10 kHz units (the one value that is not a byte) | 7600 .. 10800 |
| 15 | USB audio: the AUDIO set (1) or STANDARD (0) (slice U) | 0, 1 (the boot set is 1) |
| 16 | USB audio profile (`hal::usb_audio::Profile`) | 0 P2, 1 P8 (the boot profile), 2 P10, 3 P11 |
| 17 | USB audio: the return stream is part of the set | 0, 1 (the boot set has it) |
| 18 | RETURN LEVEL, per cent | 0 .. 100 (100) |
| 19 | the edited stem slots below apply (else the profile's factory slots) | 0, 1 |
| 31 | DISPLAY brightness (slice CP, `app/brightness.h`) | 0 low (backlight duty 20), 1 medium (50), 2 high (128, the hardware's own and the default) |
| 32 | PAD LIGHTS | 0 low (a third of every pad level), 1 medium (two thirds), 2 high (as drawn); a lit pad never drops below level 16 |
| 20 .. 30 | stem slot 1 .. 11 of the profile, a mask of the 16 stem sources (bit 0 T1 .. bit 11 T12, 12 Master L, 13 Master R, 14 Send A, 15 Send B; the second value that is not a byte) | 0 .. 65535 |

**A key is never reused and a new one is appended**, like a unit's parameter id; the next setting takes key 33. The writer writes every key it knows, in key order; the chunk is `4 + 6 count` bytes. The audio keys are applied at boot only where they differ from the defaults (source line mix, gain 0, monitor off, Europe, 87.50 MHz): a card of an app that never used audio in costs the codec and the input gate nothing.

## 2. `APPV` and `LRNB`

Both are written only when they have something to say, and both start `u16 version` (`0x0100`) and `u16 count`. A chunk of a newer major version, one whose count does not fit its length, a second one of the same id, or any other chunk, is skipped and counted (`unknown_chunks`); **saving over a file that has such a chunk drops it**, as track3r-os's settings file does with the chunks it does not know.

- **`APPV`**, the values of the app's own Config pages (`App::config_pages`, `kit/app/config_page.h`): `count` entries of `u16 key` (the item's own, never reused) and `f32 value`, in page order, at most 32. The runtime hands each back to `App::config_set` after `App::setup()`, clamped into the item's `ParamDesc` and only where it differs from what the app has; a key the running app has no item for is dropped at the next save; a NaN or a zero key is not a value.
- **`LRNB`**, the MIDI learn bindings (`Midi::learn`, `core/learn.h`): `count` entries of 8 bytes, `u32 core::ParamId` (kind Unit, the unit's id as `index`, its parameter id as `field`), `u8` controller 0 .. 127, `u8` channel (0: every, else 1 .. 16), `u8` mode (0 pickup, 1 jump, 2 relative), `u8` flags (0), at most 16. They are in the kit's file and not in the app's pattern files because they are about the user's controller, not the music; a binding is a unit slot and a parameter id, so it follows the build. An entry that is not valid, names a kind other than Unit, or listens to a message or names a parameter that an earlier one has, is dropped (counted in `skipped_values`); a binding whose unit the running build does not have is kept in the file and ignored.

## 3. Reading is tolerant and says so

The container is checked first (the CRC included); then the chunks are decoded into a copy, and the values change only when all of it went through.

| Result | When | Effect |
|---|---|---|
| `Ok` | the container is whole and `SETS` is of major version 1 | each entry applies; an entry whose key this version does not know is skipped and counted (`skipped_keys`); one whose value is out of range, not whole or NaN is skipped and counted (`skipped_values`); a key the file lacks keeps the value the app set; a chunk other than `SETS` and `CRC ` is counted (`unknown_chunks`); a `SETS` of a **newer minor** is read as far as this version knows it (`newer_minor`: entries it appended are skipped, bytes after the entries are ignored) |
| `NewerMajor` | `SETS` of major 2 or more | nothing is applied: the defaults stay, and the card is **protected** (section 4) |
| `Malformed` | a count that does not fit the chunk, extra bytes after the entries of this minor, a second `SETS`, major 0, a container error | the backup is tried (section 4), else the defaults stay |
| `Truncated`, `BadMagic`, `BadCrc` | the container | the backup is tried, else the defaults stay |

A file that is missing, damaged or newer therefore never leaves the unit unusable: the app runs with what its `setup()` set.

## 4. When it is read and written

- **Read** at boot, in `main.cpp`, after the card is mounted and before USB starts (`Runtime::load_settings`), and again by the files service whenever a card is put in (or the same card comes back): one read of at most 2 KiB, in an idle pass, applied by the runtime as at boot. The values it gives win over what `App::setup` set and over a change made while there was no card. If `system.o3s` is gone, or is damaged (CRC, cut, malformed), and `system.o3s.bak` is good (a save cut between its renames, `kit/app/atomic_save.h`), the backup is read.
- **Never written before it is read.** Nothing is written to a card whose file has not been read or found missing: a card put in later never has its settings overwritten by defaults. If the file is of a newer major version, past 2 KiB, or the card cannot give it, the card is *protected*: the session keeps its values in memory, nothing is written to that card (the file and its `.bak` are the user's newer ones), the strip says SETTINGS NOT SAVED once, and the console's `files` shows `read refused`. A damaged file with no good backup is not protected: the next change writes a good file over it. CARD AS DISK is never restored at boot (D14, when the disk mode exists).
- **Written** by `Files`, in the main loop's job pass, as any save is (a temporary file, a read-back with its CRC, then the rename; the old file stays as `system.o3s.bak`):
  - 1.5 s after the last change of a setting (track3r-os's `kSettingsDebounceMs`);
  - at once when the power-off begins;
  - later, when the card is not writable now (out, protected, exFAT, the computer's, held by the update path): the settings stay due and go when it is.
  - Never for what the app set in `setup()`, nor for what was just read: a read counts as saved. A failed write (card full, an I/O error) is not tried again until a setting changes.
- **Yields.** An app's own save never waits for the settings: a settings write in progress steps aside (it is cancelled and repaired at once, and asked again afterwards), unless it is already reading back or renaming.
- **Silent.** The settings write shows nothing on the strip and does not touch `Files::status()`.

The console's `files` command prints what the last read found (`read loaded|backup|no-file|refused|no-card`, the counts) and whether the settings are due.
