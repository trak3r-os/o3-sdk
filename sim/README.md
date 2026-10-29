# sim/: run an app on the host

Every app built with `o3sdk_app()` gets an executable `<name>_sim`: the app's own code, unchanged, on the HAL fakes.

```sh
build-host/apps/hello/hello_sim                                    # 500 ms, then ./hello.png
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello
build-host/apps/hello/hello_sim --ms 2000 --wav /tmp/hello.wav     # the whole run's audio
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/config.txt --firmware-dir apps/hello/scenarios/firmware --out /tmp/hello
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/midi.txt --out /tmp/hello    # notes and a master clock in (a fake MIDI device)
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/files.txt --card /tmp/hello-card --out /tmp/hello   # the file API on a card directory
build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/audio_in.txt --out /tmp/hello --wav /tmp/hello.wav  # a tone in, the monitor, the radio (a fake codec input and radio)
```

`--firmware-dir DIR` gives the system Config screen a fake update path (`platform/host/fake_firmware.h`) over the `*.ptf` files of `DIR`, in name
order, the first being the reset target; without it the screen has no FIRMWARE section. Nothing is read from the files and nothing is written or
flashed, there is no flash. A file whose first line is one decimal number is listed with that size (a fixture is a few bytes; a `.ptf` is hundreds
of KB); a name that contains `bad` fails its check at byte 1024 ("bad vector table"); a check takes 300 ms of simulated time; an install that the
check allowed never ends, as on the unit. In a test, `sim::Harness(app, &sink)` takes any `hal::firmware::FirmwareSink`, and `load_firmware_dir()` fills a `FakeFirmwareSink` as the option does.

`--midi` gives the app a fake MIDI device (`platform/host/fake_midi.h`, `hal::midi::Device`): the MIDI section of the system Config, the app's routes
of notes to units, `App::midi()`, the clock followed or sent. A scenario that has a `midi` or `clock` line gets one without the option. Without a device
(the default) the platform is the one that existed before MIDI: no MIDI section, no traffic.
In a test, `sim::Harness(app, firmware, &fake_midi)` takes any `hal::midi::Device`; `Harness::midi()` is the fake, `due_samples()` the timeline sample "now".
The fake is also the reference model of what a real device promises the kit (`hal/midi.h`): input stamped with a frame, an ideal master clock (`clock_start`,
`clock_continue`, `clock_stop`, `clock_tempo`, `clock_off`: Start at the next block, a quiet master flywheeled for 48 clocks), and the clock and transport
the app sends captured with their sample (`FakeMidi::sent`).

`--audio-in` gives the app a fake codec input and FM radio (`platform/host/fake_capture.h`, `hal::capture::CaptureHw`): the AUDIO section of the system Config,
`Context::audio_in()` and `radio()`. A scenario that has an `input` or `radio` line gets one without the option. Without one (the default) the app has no audio in: no AUDIO section, every call
of `audio_in()` and `radio()` is refused and the input of every block is silence, so the render is the one an app without audio in always had. The scenario's `input tone <hz> [<dbfs>]`,
`input wav <file>` (16-bit PCM, looped) and `input off` set what the codec hears at the line jack and the microphone; the fake scales it by the gain the kit set (line 0..22.5 dB, microphone
0..25 or 0..44 dB, as the platform maps it) and delivers it as the engine's input, so a unit's `in_l` / `in_r` and a hook's `input_l()` see it, and the monitor plays it in the master (`wav`).
`radio stations <f...>` is what the air holds (MHz, `93.7`, or 10 kHz units, `9370`): a seek finds the next of them, and a tuned one is a sine at freq / 10 Hz in the input while the source is
Radio; `radio absent` is a board with no receiver (Absent after the power-up: the AUDIO section says so). The fake follows the driver's states and its times (Starting 0.7 s, Tuning 80 ms,
Seeking 0.4 s). In a test, `sim::Harness(app, firmware, midi, card, app_id, &fake_capture)` takes any `hal::capture::CaptureHw`; `Harness::capture()` is the fake (`refuse_writes(n)` makes the codec
not acknowledge, to test the retry).

`--card DIR` gives the app an SD card: the directory `DIR` (made when it is not there) is its root (`platform/host/dir_card.h`: a `hal::storage::Storage` and
`StorageWriter` that applies the real write guard, `hal/app_guard.h`, to every change). `Context::files()` saves and loads under `DIR/Apps/<app>/` (`<app>` is the CMake id: `hello`), the
kit keeps the theme and the MIDI settings in `DIR/Apps/<app>/system.o3s` and reads them again at start, and a scenario can pull the card, protect it and restart the app on it (`card`,
`reboot`, `expect`, below). Use a scratch directory: nothing outside `Apps/<app>/` and `Samples/` is written in it, but a file of yours there is replaced by a save of the same name.
Without the option the app has no card: `Context::files()` says NO CARD and the kit keeps nothing. In a test, `sim::Harness(app, firmware, midi, &card, app_id)` takes any
`platform::host::HostCard` (`DirCard`, `FatFsStorage`, or a wrapper over the in-memory `MemCard` of `tests/support`), behind a write-protect switch (`ProtectCard`); `Harness::card()`,
`card_out()`, `card_in()`, `card_protect()` and `reboot()` are the commands' functions. `reboot()` makes the engine and the runtime again, power-cycles the card (a file being written stays
torn, as after a power cut) and starts the app as the unit's `main` does: `App::setup`, then the settings read from the card. The app object is the same one (it keeps what it holds in
memory), as every test starts a fresh simulator on the one `o3sdk_app()`.

The executable is built in the binary directory of the CMake directory that calls `o3sdk_app()` (`build-host/apps/hello/` here;
`build-host/app/o3_303_sim` in the sibling o3-303, which builds o3sdk from outside).

The `--out` directory must exist (`mkdir -p`). In a test the same app runs through `sim::Harness` (`include/o3sdk/sim/harness.h`: `start`, `run_ms`, `press`,
`pad_down`, `turn`, `audio`, `leds`, `runtime().state_line()`; `templates/app/tests/app_test.cpp` is the smallest example). `o3sdk_app()` returns
one object, so every Harness of a test run drives the same app: reset your members in `setup()`.

It prints each file it writes and a summary line (frames drawn and refused, the largest display list, audio blocks, dropped events
and commands). Exit 1 when the scenario fails or a frame did not fit, so a script can gate on it. Exit 1 also, before anything runs, when
the app's `setup()` had a call the engine refused (a route to a slot that does not exist, an effect as a source, a hook past the fourth:
`Engine::setup_errors()`); stderr then names each one (`setup refused: set_source(slot 9, unit 0): slot, bus or index out of range`).
`Harness::start()` returns `false` for the same reason and writes the same lines, so a test that `REQUIRE`s it shows why it failed;
`Harness::setup_report()` returns the text.

## What it is

- **Time** moves only when the simulator moves it (`Harness::run_ms`): the main loop is polled every millisecond and the audio is
  rendered in lockstep, one 64-frame block each time one is due at the engine's rate. A run is deterministic.
- **The screen** is `EveRaster`, a software interpreter of the FT81x display-list words the UI emits through `hal::Display`, from
  track3r-os. It is a tool for looking at screens and catching regressions, not a model of a particular FT812 (track3r-os
  `sim/README.md`, "Fidelity").
- **The picture** is the screen above a drawing of the 48 pads: each pad lit to its LED level (warm white, the LEDs are monochrome),
  a held pad framed in blue.
- **The audio** is the engine's output as 16-bit stereo, written as a WAV at the engine's rate.
- **Recording** (`Context::recorder()`) is the unit's own record service (platform/mk66/audio `record_service.h`) around every rendered block, over the fake codec's input: a take of the input
  or the master, started on the spot, on a step or on a level, is the unit's to the sample (`apps/hello/scenarios/record.txt`; no scenario command of its own: the app's keys arm it, `expect state rec=...` reads it).
- **The CPU figure** is not shown: the host is not the unit. Measure on the unit.

## Scenarios

One command a line, `#` starts a comment (`sim/include/o3sdk/sim/scenario.h`):

| Command | Effect |
|---|---|
| `wait <ms>` | let time pass |
| `press <button>` / `release <button>` | a button down / up (names: `f1`..`f8`, `play`, `rec`, `shift`, `params`, `sampleload`, …) |
| `tap <button> [<ms>]` | down, hold (default 60 ms), up |
| `pad <n> [<ms>]` | pad n (0..47, row by row from the top left) down, hold (default 150 ms), up |
| `paddown <n>` / `padup <n>` | |
| `turn <detents>` | the encoder, + clockwise |
| `snap <name>` | `<out>/<name>.png` |
| `boot <name>` | the boot screen of this image (its logo: the kit's O3, or the app's own `LOGO`) as the first frame, `<out>/<name>.png`; the runtime's next frame replaces it |
| `record on\|off` | keep the rendered audio |
| `wav <name>` | `<out>/<name>.wav`, what has been recorded so far |
| `midi <din\|usb> <hex...>` | MIDI bytes arriving on a port now, `midi din 90 3c 64` (a note on, channel 1, middle C); channel messages only, with running status, so `midi din 90 30 64 34 64` is two notes; they land in the block that renders next |
| `clock <bpm> start\|stop` | a MIDI master at `<bpm>` (20 .. 300, decimals allowed) sends Start or Stop; `clock <bpm> continue <beats>` is a song position (sixteenths) then Continue; `clock off` takes the master away. The engine follows it when FOLLOW is on in the Config's MIDI section |
| `input tone <hz> [<dbfs>]`, `input wav <file>`, `input off` | what the fake codec hears (a capture device: `--audio-in`, or a scenario that has the line): a sine at -12 dBFS by default, a looped 16-bit WAV, or silence |
| `radio stations <f...>`, `radio absent`, `radio present` | the stations the fake receiver finds (MHz or 10 kHz units); a board with no receiver, or with one |

| `card out` / `card in` | the card out of its slot (every card call answers NO CARD, a file being written stays torn) and back (mounted at once, the runtime told as the platform's card watch does) |
| `card protect on\|off` | the card's write-protect switch |
| `card disk on\|off` | the card as a USB disk (`Context::usb_audio().share_disk()`, the Config's CARD AS DISK): the port re-enumerates and the card is the computer's after about 2.5 s of simulated time (`card=computer`, every save refused with CARD ON COMPUTER - EJECT FIRST); `off` is the eject. A refusal fails the scenario |
| `usb listen on\|off` | the computer opens (closes) the capture stream of the USB port (a USB device: `--usb`, or a scenario with a `usb`, `usbwav` or `card disk` line); nothing streams until it does |
| `usb profile <2\|8\|10\|11> [out\|duplex]`, `usb audio on\|off` | the USB layout (the port re-enumerates, about a second; the profile's factory map comes back) |
| `usb map <slot> <sources>`, `usb map reset`, `usb level <0..100>` | a stem slot (`T1+T3`, `ML`, `MR`, `SA`, `SB`, `-`), the factory slots, RETURN LEVEL |
| `usb return tone <hz> [<dbfs>]`, `usb return wav <file>`, `usb return off` | what the computer plays to the unit (the return stream) |
| `usbwav <name>` | `<out>/<name>.wav`: what the computer captured since its stream opened, one channel per channel of the profile (16 bit, 44.1 kHz) |
| `reboot` | the unit restarted on the same card: the engine and the runtime made again, `App::setup`, the kit's settings read from the card |
| `expect file <path>` / `expect nofile <path>` | the card holds (does not hold) the file `path`, relative to its root: `expect file Apps/hello/riff.o3a`. A wrong expectation fails the scenario with its line |
| `expect state <text>` | the console's `state` line contains `text`: `expect state files=done`, `expect state card=none`, `expect state riff=3` (an app's pair) |

`card` and `expect file` need a card (`--card`); `reboot` works without one. `usb`, `usbwav` and `card disk` need a USB audio device (`platform/host/fake_usb_audio.h`: the unit's own `UsbAudioService` over a fake port and computer; `--usb` or a scenario that uses one makes it; in a test `sim::Harness(..., &fake_usb)`, `Harness::usb()`, `card_disk(bool)`), and `card disk` a card too.

Each command that touches the panel is followed by one millisecond, so the runtime sees the events in order.
