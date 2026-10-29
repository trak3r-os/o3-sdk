# o3sdk: the framework guide

Status (2026-10-29): the kit, the simulator and the MK66 platform exist, and HELLO and o3-303 were installed and run on a unit over USB the
same day (`docs/hw/2026-10-25-usb.md`). M2b's second round is host-tested, with a unit run of the system Config screen reported on 2026-10-26; section 10 says, per capability, what is verified on the unit, what is done on the host only and what is
planned. This file is the reference; `docs/NEW-APP.md` is the path for a new instrument (zero to an image on the unit), `AGENTS.md` the guide
for coding agents.

## 1. The idea

track3r-os is an operating system that hosts modules (`.tmod`): the OS owns the screen, the transport and the memory, and a module
lives inside the limits it is given. o3sdk turns that around. **An app is the product** and the kit is a library under it:

- An app is one firmware image. It owns the whole screen, all 48 pads, all 33 buttons, the encoder, the CPU and the RAM.
- The kit supplies what every app needs and nobody wants to write twice: drivers, the audio engine, the UI toolkit, the input
  conventions, storage, MIDI, the simulator.
- Everything is linked statically. There is no ABI to freeze: the compiler optimises across the app and the kit, and hot code goes to
  SRAM with an attribute. Loadable units from the SD card can come later, when someone needs to ship a unit without a rebuild.
- Switching apps means flashing another `.ptf` through the bootloader's update path (track3r-os `platform/mk66/README.md`, "What the
  bootloader needs"): today over the USB console (`o3console.py --put`, then `--update`; verified on the unit, `docs/NEW-APP.md` section 11)
  or by card. The on-screen firmware switcher (Config, FIRMWARE: host-tested, not yet run on a unit) is built; an app launcher is planned (section 10).

## 2. Layers

```
APP      apps/<name>      one image: which devices it has, its screens, its global keys          HELLO, o3-303
DEVICE   (inside apps)    a convention, not a kit type: the unit(s), sequencer, screen and keymap that belong together
UNIT     units/<name>     DSP only: a parameter table and process(); the API follows CLAP        SYNTH, DELAY
KIT      kit/             core · hal · eve · ui · dsp · app · usb; platform/ and sim/ implement the HAL
```

Link dependencies enforce the layering: `core <- hal <- eve <- ui <- app`, `core <- dsp <- app`, `core <- formats <- app`, `core <- usb` (the USB device library, linked by
the MK66 platform and never by an app); a unit depends on `dsp` only; an app on `app` and its units. `platform/<name>` and `sim` implement the HAL contracts (`kit/hal`) and run an app.

| Library | What it holds |
|---|---|
| `core` | rings, fixed containers, input events, ticks, the gesture recogniser, parameter descriptors and their maths (`param.h`), placement (`placement.h`). No heap. |
| `hal` | contracts only: `Display`, `Input`, `Leds`, `Clock`, `audio::Device` and the block callback, storage, cycles. |
| `eve` | FT81x display-list encoders and the frame builder. |
| `ui` | the immediate-mode renderer, colour tokens and themes, the four faces, the components (strip, badge, soft keys, bar, meter), the list and the on-screen keyboard, plots. |
| `dsp` | the unit contract, events, the transport, the engine. |
| `app` | the panel, inputs, screens, pad LEDs, the F-key + encoder convention, shared widgets, the runtime, the console's hooks (`diag.h`). |
| `usb` | the portable USB device library: descriptors, control pipe, CDC, the `diag` shell, the log ring, USB audio. The MK66 platform links it; an app reaches the console only through `App::describe_state` and `diag_commands`. |

## 3. Units

A unit is DSP only (`kit/dsp/include/o3sdk/dsp/unit.h`): an **instrument** (notes in, audio out) or an **effect** (audio in, audio
out).

```cpp
class Unit {
 public:
  virtual const UnitInfo& info() const noexcept = 0;         // name, kind, the parameter table, page names
  virtual void prepare(float sample_rate) noexcept = 0;      // main loop, before the engine starts: reset, take the defaults
  virtual void process(const ProcessArgs& args) noexcept = 0; // the render context, one block
};
```

- **Parameters** are a constexpr table of `core::ParamDesc`: range, default, curve (linear, log, exp, table), display (`Int`, `NormPct`,
  `Enum`, `Fixed1`, `Hz`…), encoder detents and coarse step. **The parameter id is the index in the table**: append, never insert.
- **The unit never owns the values the user sees.** The engine's main side stores them, formats them and steps them with the encoder;
  the unit receives each change as a `Param` event in `process()`, at its frame. At `Engine::start()` every unit receives all its
  stored values (a `process()` of zero frames), so both sides agree.
- **Events** (`event.h`): `NoteOn` (velocity, flags `kNoteAccent`, `kNoteSlide`), `NoteOff`, `Param`, `AllNotesOff`, each with the frame
  of the block it applies at, sorted. `for_each_run()` splits a block into runs between events: the usual shape of `process()`.
- **Big buffers** (a delay line, a sample) belong to the app, in the SDRAM (`O3SDK_BIG`, `core/placement.h`), and are handed to the unit
  at construction (see `units/delay`). They start at zero: the SDRAM section keeps no contents, so the cross compiler refuses a
  non-zero initialiser on an `O3SDK_BIG` object ("only zero initializers are allowed in section"; the host build accepts it).
- **Rules** in `process()`: no heap, no blocking, no file or SD I/O, no printf, bounded loops, no `double` (the build treats unsuffixed
  floating constants as float; the cross build is checked for the software double helpers).

Writing one: `docs/NEW-APP.md` section 3 is the step list, and `templates/app/units/voice/` the smallest worked unit (an instrument with
three parameters, a click-free level, a test against the engine). `units/synth` (an instrument) and `units/delay` (an effect with SDRAM
buffers) are the kit's; o3-303's `units/tb303` is a port with an oracle test. Add `o3sdk_unit(<name> SOURCES …)` and test the unit on the
host against the engine (`tests/units_test.cpp`).

**Smoothing is the unit's.** The engine never ramps a parameter: `set_param` and `step_param` clamp the value, keep it as the target
(`param()` always reports it) and send it as one `Param` event, a step, at frame 0 of the next block (a clocked hook's at its own frame).
A value that clicks when it jumps (a level, a cutoff) is ramped inside `process()` (`dsp/smooth.h`: `Ramp`, `OnePole`). The descriptor
flag `core::kParamSmooth` (declared in `kit/core/include/o3sdk/core/param.h`) is **advisory metadata and nothing more**: no kit code reads
it. Set, it is the unit author's statement that the unit smooths this parameter itself, so a UI or host may change it at any rate and by
any amount (an encoder turned fast, a motion lane, automation) without a click; the unit's test should prove it. Clear, the unit makes no
promise and applies the value at the event's frame (a switch, a choice, a time, a value meant to jump). A UI or host may take the flag as
the author's word only; the kit does not enforce it, and a missing flag is not a defect. (`ParamChange::ramp_samples` in the same header is
a leftover of track3r-os's engine that o3sdk's does not use.)

### Porting a plugin

Only the DSP of an open plugin ports; its GUI never does (you build the screen with the kit). The checklist:

1. **Licence.** The kit is GPL-3.0-or-later, so GPL and MIT code both fit; closed code cannot be ported at all.
2. **Precision.** Replace `double` with `float` everywhere in the render path; check the result against the original on the host (an
   oracle test, as track3r-os does for its TB-303 against Open303).
3. **Memory.** No allocation after start: size every buffer up front, big ones in the SDRAM.
4. **Parameters.** Map the plugin's parameters to a `ParamDesc` table; its `setParameter()` becomes the `Param` event.
5. **CPU.** A block is 64 frames, 261,224 cycles at 44.1 kHz. Measure on the unit before promising voices (track3r-os's TB-303 voice
   costs 15-22 % of a block).

Good candidates: Mutable Instruments' code (most of it MIT, written for Cortex-M4 at 72-168 MHz: Plaits, Rings, Clouds, Elements,
Braids, Warps), Open303, the mda plugins, DaisySP, Faust-generated C++. Out of reach: Vital, Surge, Serum-class synths, large
convolutions.

## 4. The engine

`kit/dsp/include/o3sdk/dsp/engine.h`. A fixed topology set up once:

```
clocked hooks ─events─▶ slot 0..7: source ─ insert ×4 ─ level/pan ─┬──────────────▶ master ─ insert ×4 ─ level ─ clip ─▶ out
(sequencers)                                                        ├─ send A ─ fx ─┘
main loop ──commands──▶                                             └─ send B ─ fx ─┘
```

Each slot and the master have up to `kMaxInserts` = 4 inserts in series (an unused one costs nothing).

- **Setup** (main loop, before `start()`): `attach()` units (up to 16), `set_source()`, `set_insert()`, `set_send_effect()`,
  `set_master_insert()`, `add_hook()`. A refused call (a slot or index out of range, an id never attached, an effect given as a source,
  the engine full, a call after `start()`) returns `false` (`attach`: `kNoUnit`) as it always did, and is also **recorded**:
  `Engine::setup_errors()` is the count (0 for a setup that went through), `setup_error(i)` the i-th record (`SetupError`: the call,
  the `SetupReason`, the slot or bus, the insert index and the unit id it was given; the first `kMaxSetupErrors` = 8 are kept) and
  `setup_call_name()` / `setup_reason_name()` give text. So `(void)e.set_source(...)` no longer hides a mistake: `sim::Harness::start()`
  returns `false` when the count is not 0, writes the refusals to stderr (`setup refused: set_source(slot 9, unit 0): slot, bus or index
  out of range`) and `<app>_sim` exits 1 with that message before it runs anything. Setup-time bookkeeping only: nothing in the render
  path reads it, and an app whose setup succeeds sees no change. The image on the Tracker does not show it yet (the count is
  kept and not reported: a follow-up for the console's `state`).
- **Running** (main loop): `note_on()`, `set_param()`, `step_param()`, `set_mix()`, `play()`, `stop()`, `set_tempo()`. Each queues a
  command on a lock-free ring; the render applies it at frame 0 of its next block. A full ring refuses and counts; nothing blocks.
- **Render**: `render()`, once per block, from the platform's audio callback (`Runtime::block_callback()`).
- **Clocked hooks** (`Clocked::block(BlockContext&)`) run in the render context before the units and place events at any frame:
  sequencers, arpeggiators, a MIDI clock. Timing is sample-accurate.
- **Clock follow and MIDI** (`follow()`, `inject()`, `midi_out()`, `block_frame()`, `following()`; slice K, host only). `follow(ClockFrame)`
  takes the block's transport from a master's clock, in the render context before `render()`: the engine's own advance is parked, every
  clocked hook follows with no change, and `tempo()` and `playing()` report the master's. While following, `play()` and `stop()` are requests
  for the device (`take_transport_request()`), and `set_tempo()` is kept for when the master is let go. `inject(unit, event)` places the notes
  a MIDI route sends into a unit's events for the block; `midi_out()` and `block_frame()` hand the block's outgoing messages and transport back
  to the device (`hal/midi.h`).
- **A hook writes the mixer** with `BlockContext::set_mix(slot, field, value)` (a motion lane, a slew; `field` is a `MixField`: Level,
  Pan, SendA, SendB, Mute). The value is clamped as `Engine::set_mix` clamps it, becomes the field's target at once, and the mixer
  ramps the gain to it across the block, from where the last block left it, as for a main-loop write: a jump never clicks. It costs a
  few instructions and has no queue, so a hook may call it as often as it likes, but the mixer has one ramp per block (up to 64 frames, not
  frame-accurate like an event): several writes to a field in a block collapse to the last, so a slew written every 16 frames
  lands at the block's rate. `BlockContext::mix(slot, field)` reads the target as the render side stands (a slew starts from it).
  **The rule: each block applies the main loop's queued commands first, then runs the hooks in the order added, then the mixer reads
  the targets; for a field, the last write in that order wins.** A hook's write beats a main-loop write applied in the same block, a
  later hook's beats an earlier one's, a later call beats an earlier one. The value then stays (nothing restores the main loop's)
  until another write replaces it: the main loop's next write, applied at the start of a later block, does; a hook that writes the
  field every block (a lane) holds it against the main loop, and one that stops writing hands it back at the next main-loop write.
  `Engine::mix()` keeps reporting the main loop's last value (a hook's write is the render side's, as a `Param` event is for
  `param()`). A `slot` >= `kMaxSlots` or a `field` that is not a `MixField` is refused: `false`, nothing written (a value is clamped,
  an index never). A slot with no source takes the write (it is only a target).
- **The transport** (`transport.h`) counts Q32.32 ticks at 960 PPQN, advanced by an integer increment computed from the tempo and the
  one rate constant: no drift. `for_each_step(t, period, offset, swing, fn)` walks a grid (sixteenths are 240 ticks) and calls
  `fn(frame, step)` for each step that starts in the block; `Transport::started` and `stopped` mark the first block after Play and Stop.
- **Statistics** (`stats()`): peaks, the render's cycles (with a counter: the unit), dropped events, clips, the transport position;
  read through a sequence counter, so a snapshot is always one block's.
- **The input and the stems** (M2c slice SE, host only): `set_input_gate(bool)` fades the codec input in or out over one block for the
  units, the hooks, the monitor and the peaks; `set_input_monitor(gain)` adds the gated input to the master bus before the master
  inserts (0 by default, clamped to 0..1.5); `set_input_peaks(true)` and `input_peaks()` read and clear the input's peak per channel;
  `set_stem_slots(mask)` stages slots T1..T8, the master pair and the sends as 64-frame mono int16 blocks, and `stem_block()` hands that
  table to the platform's `StemTap` (null when nothing is staged). Off, each costs a relaxed load and a branch per block, and a render
  is bit-identical to one without them (`dsp_input_test`, `dsp_stems_test`); `feature_work()` counts the frames each one did. Their
  cost when on is measured on the unit by the AI and U slices.

## 5. Apps

`kit/app/include/o3sdk/app/`. An app (`app.h`) owns its units, devices and screens as plain objects, wires them in `setup()` and
shows the first screen. Every app defines exactly one entry point:

```cpp
o3sdk::app::App& o3sdk_app() noexcept { static MyApp app; return app; }
```

- `App::input()` sees every input first (global keys: Play, the screen buttons; not Config, which is the runtime's: the system Config, below); `true`
  consumes it.
- A **screen** (`screen.h`) is what the display, the pads, the soft keys and the encoder mean right now: `input()`, `update()`,
  `draw()` into the content area, `soft_keys()` (labels, the hot key, dimmed keys; a Shift row if it wants one), `leds()` (a whole frame
  of 48 levels each UI frame). All in the main loop.
- `Context` (`context.h`): the engine, the panel's held state, time, `show()` to switch screens, the theme, `redraw()`.
- `Input` (`input.h`): presses, releases and turns, raw, with Shift's state. A press arrives at the press, so pads play at once.

### The controls convention (`param_keys.h`)

The kit's apps share one way to edit values, so the hand learns it once:

| Gesture | Effect |
|---|---|
| tap F1..F8 | the encoder drives that key's parameter from now on (latched) |
| hold an F key + turn | that one moves while held; at the release the latched one comes back |
| hold two F keys + turn | both move together |
| turn / Shift + turn | fine / coarse steps (the descriptor's `detents` and `coarse`) |
| a page button (Params on HELLO) | the next page of eight |

`knob_strip()` (`widgets.h`) draws the eight cells above the soft keys: name, value with unit, a bar, the focused cell highlighted.

### The list and the keyboard (`kit/ui`: `list_view.h`, `keyboard.h`)

For anything an app picks by name (a file list, project names) and for typing one. Both draw immediate-mode from plain arguments, hold no heap, and
are linked only by an app that uses them (they sit in the `ui` archive's own objects).

```cpp
struct Files final : ui::ListSource {                       // the caller's items: count and text at an index, nothing copied
  unsigned count() const noexcept override { return n_; }
  void name(unsigned i, char* out, unsigned cap) const noexcept override { std::snprintf(out, cap, "%s", names_[i]); }
  void value(unsigned i, char* out, unsigned cap) const noexcept override { std::snprintf(out, cap, "%u KB", kb_[i]); }  // optional
  ...
};
ui::ListView view(ui::ListView::rows_in(r.theme(), area));      // the window's height in rows
// input:  view.move(files, encoder_delta); view.pad(files, pad); view.page(files, +-1); view.home/end(files); after a rescan: view.fit(files)
// draw:   view.draw(r, area, files, {"NO FILES", "put .wav files in Samples/"});   // the cursor on the Sel bar, values right, a scroll mark, the empty state
```

The cursor clamps at the ends (no wrap) and the window scrolls the least that keeps it in view; pad `p` is the `p`-th visible row. A full window of 18 rows
of 40-glyph names with values is about 900 display-list words (`ui_list_test`).

```cpp
app::TextEntry entry;  entry.begin("PATTERN", 15, app::TextEntry::Filter::FileName);   // kit/app: the state machine that edits the name
// input:  encoder turn = entry.move(+-1, 0); press = entry.confirm_key(); pad p = entry.pad(p); up/down = entry.move(0, +-1); delete = entry.backspace()
ui::KeyboardKeys keys;                                                                  // label storage on the stack
ui::keyboard(r, {0, 0, 800, 480}, ui::keyboard_view(entry, app::kTextPadKey, keys, "PATTERN NAME"));   // a dialog, centred, about 500 words
```

The keyboard is the pads' grid (12 x 4, one cell a pad): the focused key an Accent block, refused keys (the file-name filter's `@` and `+`) dim, the caps key
inverted while on, the name selected until the first key. `ui` sits below `app`, so the widget takes what it shows as a `KeyboardView`;
`keyboard_view()` builds one from any `TextEntry`-shaped object. A file picker and a name entry that join `Files` and `TextEntry` to these come with the app Config pages.

### Diagnostics: the console's hooks (`diag.h`)

The platform's USB console (`state`, and the commands an app adds) reaches an app through two optional `App` methods, so `kit/app`
depends on no USB code. `App::describe_state(ctx, out, cap)` writes the app's `key=value` pairs (`StateWriter`; a value has no space;
a pair that does not fit is dropped whole); `Runtime::state_line()` puts the kit's pairs first, `app=<name> screen=<title>
playing=0|1 tempo=<mbpm> cpu=<pct> frames=<n> sys=0|1 section=<firmware|display|info> fw=<closed|list|ask_update|checking|ask_install|installing>`, then the app's. `App::diag_commands(rows)` returns a table of `DiagCommand` (name, help
line, `void run(const DiagArgs&, DiagOut&) noexcept`); the console takes at most 16, a row whose name is not 2-7 lowercase letters,
is a kit command's, or carries a word of the update path (`update flash boot ...`) is skipped and logged. Handlers run in the main loop
and reach their app through `o3sdk_app()`. HELLO's `describe_state` is the example.

### The system Config (the kit's, in every app)

**Status: built (M2b, second round; `kit/app` `system_config.h`, `firmware_flow.h`): FIRMWARE, DISPLAY and INFO, MIDI when `Platform::midi`
is set, and AUDIO (the audio in and the radio) when `Platform::capture` is set (M2c slice AI, host-tested, not yet on a unit); host-tested, with the simulator scenario `apps/hello/scenarios/config.txt`. On a unit: a release image with this screen (o3-303 988318f on
o3-sdk 20ccef1) was installed over USB on 2026-10-26, reported by the tester: Config's FIRMWARE, DISPLAY and INFO opened and worked over USB
injection, cpu 35.5 and 37.1 %. Not verified: the final FIRMWARE INSTALL from the screen (F1, which needs a finger).** The USB rows of AUDIO and the app's own
pages are planned. The settings on the card are built (slice FA of M2c, host only: see "Files on the card" below).
What follows is the maintainer's decision (2026-10-25) for what it is: the kit brings the machine up (clocks, SDRAM, display, input,
LEDs, audio, the crash record) and owns the **Config button**; everything else (the pads, the other buttons, the encoder, every screen) is
the app's. "Por defecto config lleva el flasheo de otro firmware": any engine built on o3sdk can change the unit to another engine.

- The runtime takes Config before `App::input()`: a press opens the system Config screen over the app's screen, Config again goes back.
  The app never sees the Config button, press or release. While the screen is open the app gets no new input (the release of a button
  or pad pressed before still reaches it, so no note is left on), its screen goes on running and sounding underneath, and the pad LEDs are
  dark. `Runtime::lock_input(bool)` locks input altogether, `Runtime::system_strip(text, progress, hold_ms)` puts one line over the bottom of
  whatever is on screen (the platform's power-off hold feeds it), `Runtime::card_changed()` is the platform's card watch.
- Sections, in this order: FIRMWARE (hidden when `Platform::firmware` is null), AUDIO (hidden when the platform has neither `Platform::capture` nor `Platform::usb_audio`: INPUT, GAIN, MONITOR and a level meter of the input; while INPUT is
  RADIO also FREQ, REGION and SEEK, with F5 / F6 seeking down and up; the radio's rows are greyed and INPUT skips the radio when the receiver does not answer, F5 then tries it again; see "Audio in and the radio"
  below; then, with a USB audio device, USB AUDIO, PROFILE, RETURN and RETURN LEVEL, and INPUT also offers USB, the computer's return; a change of the USB set asks F1 twice and the port reconnects; see "USB audio and the card as a disk"
  below), MIDI (hidden when `Platform::midi` is null: the user's
  `hal::midi::Config` and the device's status; see "MIDI" below), CARD (with a card capability: the card as the files service sees it, the volume, the job, and with a USB audio device CARD AS DISK and READ ONLY), DISPLAY (the theme), INFO (the app's name and version, the
  build id, the uptime). Keys: LEFT / RIGHT change section, F8 is BACK (one level up; the last closes the screen), CONFIG closes it.
  FIRMWARE: the `Firmware/*.ptf` list is read in one pass when the section opens (UP / DOWN or the encoder choose), F1 asks "UPDATE
  TO name?", F1 again starts the sliced check, F1 once more at "IMAGE OK, length, CRC" installs; any other control takes a question back, F8
  cancels a check, and nothing is written before INSTALL. Past INSTALL no key acts. The list stops at 12 files (track3r-os's OS-BUG-FWLIST,
  ported as it is). The path is `hal::firmware::FirmwareSink`: on the unit `update::device::sink()`, the same service `fw` uses, with its
  guards unchanged; in the simulator a fake over a folder (`--firmware-dir`, `sim/README.md`).
- An app can refuse for a moment with `App::update_blocker(ctx)` (a text, or null: asked when the list opens and at INSTALL), for
  example while it records. `App::config_pages()` (default 0) is declared for the pages an app will add; the kit draws none yet.
- The remaining plan (the maintainer, 2026-10-25): (the USB rows of AUDIO and the CARD section are built, slice U of M2c, below; the MIDI section and AUDIO's input rows too),
  brightness and the last crash record in INFO; a section whose driver does not exist yet is not shown. An app adds its own pages after
  the kit's: up to eight items each, a value (a parameter descriptor with get and set on the main loop) or an action (run after a
  confirming hold), and it reads the kit's settings (the theme, the MIDI channel) through `Context`.
- An app can also extend or change the kit's sections ("engine puede añadir o cambiar también salidas usb", the maintainer): add items
  to a section, hide one, set its defaults. For USB audio the kit streams the master by default, and an app can declare more streams
  (the dry voice, a send, stems) up to what full-speed USB carries (about eight channels at 44.1 kHz, 16 bits). FIRMWARE and INFO stay
  in every app, so any unit can always be switched to another engine and tell what it runs (confirmed by the maintainer, 2026-10-25).
- The kit keeps its settings on the card for the app (M2c, slice FA, host only until the unit session): the theme, the MIDI settings and the audio in's source, gain and monitor and the radio's region and station (keys 10 to 14, slice AI) in `Apps/<app>/system.o3s`, read at
  boot over what `App::setup` set, written 1.5 s after the last change and at once when the power-off begins (`docs/formats/o3sdk-settings.md`). The app's own Config values
  join them with the app pages. FIRMWARE says SAVING while a card job runs (`Files::busy()`).

### MIDI (`midi.h`)

`kit/app/include/o3sdk/app/midi.h`. The device is `hal::midi::Device`, given to the runtime as `Platform::midi`; null on a platform or a test rig
without MIDI, and then nothing arrives and nothing leaves. Slice K, host only.

- **Notes**: `ctx.midi().route(unit, channel)` makes a unit play the notes of a channel (0 is the channel the user chose in the system Config,
  omni when that is 0). A note reaches the unit at the frame its arrival maps to, one block after it came in (about 1.5 ms, no jitter). At most
  four routes, one unit each. All sound off and all notes off end the unit's notes.
- **Everything else**: every channel message no route took (controllers, program changes, pitch bend, the notes of another channel) reaches
  `App::midi(Context&, const core::MidiMessage&)` in the main loop, in order.
- **Clock**: nothing to write. With FOLLOW on in the system Config, the engine's transport is the master's clock (section 4) and every clocked
  hook follows. With the engine playing, the clock and the transport go out on the ports the user chose. `ctx.midi().send(m)` sends one message
  from the main loop.
- **Settings**: `ctx.midi().config()` and `set_config()` are the user's `hal::midi::Config`; an app that sets them in `setup()` sets the defaults.

### Audio in and the radio (`audio_in.h`, `radio.h`)

`kit/app/include/o3sdk/app/audio_in.h` and `radio.h`, `Context::audio_in()` and `Context::radio()`. **Status: built (M2c, slice AI), host-tested and run in the simulator (`apps/hello/scenarios/audio_in.txt`, `audio_in_absent.txt`); not yet run on a unit
(line in was never seen on one, the microphone never measured, and the radio may not be on this board: see `docs/M2C-PLAN.md` section 13).** They exist when the platform gives the runtime a `hal::capture::CaptureHw` (`Platform::capture`); without one every call is refused
(`available()` is false), the Config has no AUDIO section and the render is untouched.

```cpp
app::AudioIn& in = ctx.audio_in();
in.select(hal::capture::Source::MicHigh, 40);   // the codec's input and its gain (0..100 per cent); the planes are silent for 200 ms while it settles
in.set_gain(55);                                // the gain only, no settling
in.monitor(app::AudioIn::kMonitorOn);           // the input into the master bus (70 %); monitor(0) is off and free
const dsp::InputPeaks p = in.peaks();           // peak of each channel since the last call; the engine measures only while you keep asking
ctx.radio().tune(9370);                         // 93.70 MHz (10 kHz units), snapped to a channel of the region
in.select(hal::capture::Source::Radio, 20);     // powers the receiver and points the codec at it
ctx.radio().seek(true);                         // returns at once; radio().status() says Seeking, then Ready with the station in .freq
```

- The input reaches the render as it always did (`in_l` / `in_r` of a unit, `BlockContext::input_l()` of a hook). `select()` shuts the engine's input gate (`Engine::set_input_gate`, a one-block fade) and the runtime opens it again 200 ms later
  (`AudioIn::kSettleMs`); `LineMix` is the mean of the two line channels in both planes, `LineLeft` / `LineRight` one channel in both, the other sources reach the planes as they are. **Until an app or the user selects a source nothing here touches
  the codec, the gate or the planes** (an app that never calls it renders what it did, bit for bit). The board's power-up input is what the app has then; the analogue switch (PTE26) boots in the radio's position, so the line jack is heard only after a `select` of a line source.
- A codec write that is not acknowledged is kept and retried every 500 ms from the main loop (`failed()`); the gate stays shut meanwhile. A retry of the radio input waits while the radio is Absent or Failed: only `radio().power(true)` (AUDIO's F5, `radio retry`) probes the chip again. The radio is a state machine in `hal::capture::CaptureHw::service()`, which the platform's main loop calls once a pass (one I2C transfer at most);
  `radio().status()` is Off, Starting (about 0.7 s), Ready, Tuning, Seeking, Absent (the chip did not answer: the board may have no radio) or Failed. The kit never asks a dead receiver again by itself; `radio().power(true)` is the retry
  (the Config's F5, the console's `radio retry`).
- The kit keeps source, gain, monitor, region and station with its settings (`settings.h` keys 10 to 14) and applies the saved ones over the app's defaults at boot, only what differs from the defaults (so a card with the defaults costs the codec nothing).
- Console: `radio [retry]` (the driver's state, last failure, registers, pin levels) and `mon [off|on|0..100]`; `state` gains `in= gain= mon= gate= radio= freq=` when there is a capture device. The USB return (`Source::Usb`) is a later slice of M2c; recording is the next section.
- The simulator's fake codec and radio: `platform/host/fake_capture.h` (`sim/README.md`: `input tone|wav|off`, `radio stations|absent`, `--audio-in`).

### Recording (`recorder.h`, `hal/record.h`)

`Context::recorder()` records the audio into a buffer the app owns (an `O3SDK_BIG` array in the SDRAM: the kit allocates none) and `save()` puts the take on the card as a WAV.

```cpp
O3SDK_BIG TakeMem g_take;                       // struct alignas(4) { std::int16_t pcm[44100 * 10]; }: ten seconds, mono
hal::record::Take t;  t.buf = g_take.pcm;  t.capacity_frames = 44100 * 10;
t.align = hal::record::Align::Bar;              // Free (next block), Step, Bar, Pattern (on the transport's grid, to the sample) or Level (threshold + preroll_frames)
ctx.recorder().set_tap(hal::record::Tap::Master);   // Input: what the codec delivers; Master: what the main output plays (a resample)
ctx.recorder().arm(t);                          // returns at once; state() Waiting / Listening / Recording / Done, frames(), overrun()
ctx.recorder().stop_at(hal::record::Align::Bar);    // the take ends AT THE SAMPLE of the next bar (or disarm() at the next block)
ctx.recorder().save(ctx.files(), "", "take");   // Apps/<app>/take_001.wav, 16-bit, 44.1 kHz; once Files::status() is Done: release()
```

- The service is `platform/mk66/audio/record_service.h`, pure: the unit and the simulator run the same code (D8), so a take in the simulator is the unit's to the sample. It wraps the render callback (`record_service::wrap(rt.block_callback(), engine)` in `main.cpp`, the simulator's harness the same way);
  `RecTap` (taps.h) does the writing, its hot kernels in SRAM_L. **A recorder that is not armed costs one acquire load and a branch a block** (`active_blocks()` counts the blocks that did more; a test holds it at 0 and compares the output with a recording run: not one sample differs).
- Aligned starts and stops use the transport's own sixteenths (`Engine::block_frame()` through `dsp::for_each_step`), a straight grid: a sequencer's swing is not applied. A Bar is 16 sixteenths, a Pattern is `set_pattern_steps()` (16 by default). **While the transport is stopped an aligned take waits** (`Waiting`) and starts when it plays: nothing falls back to Free.
  A Master take starts on exactly the step's sample; an Input take is what the codec delivers from that sample on, which lags by the loop's round trip (not compensated). `auto_steps` ends a take that many sixteenths after its first.
- A Level take listens first (`Listening`): the first `ring_frames(preroll)` frames of the buffer are the pre-roll ring, the trigger is the first sample at or above `threshold` on the tapped bus, `frames()` then counts from the buffer's start and `clip()` (or `settle()`) gives the audio without the lead.
- A full buffer ends the take (`Done`); the frames that did not fit are `overrun()` and never overwrite anything. The Input tap hears the line's channel map as the engine does (`AudioIn`'s map reaches the recorder through `set_line_map`, which the runtime calls every pass: a LineLeft take is the left channel in both planes, to within a rounding of the last bit), but not the input gate: the 200 ms an input switch settles are recorded as they are. `stop_at(Level)` is refused (`false`): a level starts a take, it ends none. The service hands RecTap a 0 or a 1 per sixteenth for the boundary being asked, so a Bar is a real bar and a Pattern a pattern boundary for any `set_pattern_steps()` (12, 24, ...).
- Console: `rec` (state, frames, overrun, tap). HELLO's keys: REC (arm / stop), SAMPLE REC (start), SAMPLE PLAY (length), SAMPLE EDIT (tap), SHIFT + FILE (save the take), SHIFT + REC (the input monitor).
- Not on a unit yet: the cost while recording, the Input take's lag, a 60 s take, the card's write of a long WAV (`apps/hello/scenarios/record.txt` is the host rehearsal).

### USB audio and the card as a disk (`usb_audio.h`, `hal/usb_audio.h`)

`Context::usb_audio()` (M2c, slice U) is the app's face of the USB port beyond the console and MIDI: the audio the unit sends to the computer (the master and, per slot, stems), the stream the computer sends back, the USB profile, and the card shared to the computer as a disk. **Status: built, host-tested and run in the simulator (`apps/hello/scenarios/usb.txt`); the image carries it, but on a unit only the boot layout (P8, the master on channels 7-8) is verified; everything below it is a unit check (`platform/mk66/tools/hil/hello_usb.json`).**

```cpp
auto& usb = ctx.usb_audio();                                           // available() is false on a platform with no USB device: every call is refused
usb.request_profile(hal::usb_audio::Profile::P10, /*duplex=*/true);    // the port re-enumerates (about a second); P10's factory slots: S1..S8 = T1..T8, S9-S10 the master
usb.set_stem(1, hal::usb_audio::track_source(1) | hal::usb_audio::track_source(2));   // slot S2 = the sum of T1 and T2 (the engine's slots 1..8 are T1..T8: post-fader, pre-pan, mono)
usb.set_return_level(60);                                              // per cent: what the computer plays is added to the output after the render, never captured
ctx.audio_in().select(hal::capture::Source::Usb, 0);                   // ... and it is also the engine's input: a unit, a hook or the recorder hears it
if (usb.share_disk() == hal::usb_audio::DiskRefusal::None) { }         // the card goes to the computer; eject_disk() or the computer's own eject gives it back
```

- The layout is AUDIO (profile P2, P8, P10 or P11; the master pair is always the LAST two channels; `duplex` adds the stereo return) or STANDARD. The image boots in AUDIO, P8, duplex with the master on 7 and 8 and silence on 1..6, as it always did; the kit keeps the layout, the return level and an edited map with its settings (keys 15 to 30) and starts the port with them at the next boot. A profile change gives that profile's factory slots (P8: none, o3-sdk's own; the others StemMap's) and drops the edited map.
- The service is `platform/mk66/audio/usb_audio_service.h`, pure: the unit and the simulator run the same code over the real `StemTap`, `StemMap` and the usb library's rings (`platform/host/fake_usb_audio.h` is a fake port and computer: `usb listen`, `usb return tone`, `usbwav`). The render is `usb_audio::wrap(record_service::wrap(runtime), engine)`: **while no host listens and the audio in's source is not Usb it costs two relaxed loads and a branch a block** (`active_blocks()` is 0 and a test holds it); with the capture stream open the engine stages only the sources the slots name (`Engine::set_stem_slots`) and the tap sums them (about 130 cycles a slot, estimated); the return pulls 64 frames a block (about 1,100 cycles, estimated) and mixes (about 1,000). P10 duplex with every slot used is about 3.5 to 4 per cent of a block [est]: measured on the unit later.
- The stem pool (about 15.8 KB) is in SRAM_L's free half (`.tapmem_l`, `O3SDK_TAP_MEM_L`, budget `kSramLDataBudget`, 20 KiB).
- CARD AS DISK is track3r-os's OS-USBDISK running mode (`DiskShare`, `msc_bridge`; never at boot): F1 twice in the Config's CARD section, or `disk share [ro]` on the console. The port re-enumerates with a mass-storage function, the card is the computer's until the eject, `Files::card()` is `OnComputer` (every save and load says CARD ON COMPUTER - EJECT FIRST), the kit's settings stay dirty until the eject and are then written, and the previous layout comes back. Refused while a files job runs (`Busy`) or no card is in. READ ONLY (the default) is the policy of the next share and is not kept.
- Console: `usb profile <2|8|10|11> [out|duplex]`, `usb return [on|off]`, `usb map [reset | <slot> <sources>]`, `usb ep0`, `disk [share [ro] | eject]`. The Config's AUDIO rows are USB AUDIO, PROFILE, RETURN and RETURN LEVEL (F1 twice for the first three).
- Not on a unit yet: everything above P8's master, P11 (never measured), P10 duplex on a direct port and behind a hub (it failed behind a hub in track3r-os), the CPU with stems and the return on, the disk against a real computer.

### Files on the card (`files.h`)

`kit/app/include/o3sdk/app/files.h`, `Context::files()`. **Status: built (M2c, slice FA), host-tested and run in the simulator (`apps/hello/scenarios/files.txt`); not yet run on a unit.**
An app saves and loads its own files in `Apps/<id>/` of the SD card (`<id>` is the CMake id, `o3sdk_app(<id> ...)`, at most 15 bytes), with one level of project folders:

```cpp
app::Files& files = ctx.files();
files.save("", "riff.o3a", buf, n);                    // returns at once; `buf` stays as it is until Done or Failed
// ... a later pass:
const app::Files::Status st = files.status();          // Idle, Running (permille), Done, Failed; st.text: "SAVED", "CARD IS FULL", ...
files.load("Song 1", "pat.o3a", dst, sizeof dst);      // the whole file (the .bak when the file is gone); files.loaded() bytes when Done
files.list("", entries, 32);                           // files and project folders, sorted, with sizes; files.listed() when Done
files.save_wav("Samples", pcm, frames, 2, 44100, "take");   // NewOnly take_001.wav, take_002.wav ...
files.remove_folder("Song 1");                         // DELETE: a `.deleted` marker, nothing is removed, the listing hides the folder
```

- **Names.** `folder` is "" (the app's folder) or one project folder (`valid_name` with no `.`); `name` is `<stem>.<ext>` with the extension one of `o3a` (the app's own data), `o3s`,
  `wav`, `mid`, `syx`, `txt`, `json`, `csv`. The guard (`hal/app_guard.h`) refuses anything else: no firmware, no foreign project, nothing outside `Apps/<id>/` and `Samples/`.
  Settings are `system.o3s` in the app's folder (below). A call that is refused returns false and sets `status()` (and the strip) with the reason: BAD NAME, NO CARD, CARD WRITE PROTECTED,
  CARD ON COMPUTER - EJECT FIRST, FIRMWARE UPDATE RUNNING, NOTHING TO SAVE. A call made while a job runs is refused with no change of the status.
- **Never blocking.** A call only records a request. The runtime's `poll()` runs `Files::poll` once a pass: it starts the job and gives it `kJobBudgetUs` (2000) of the pass; a write
  is 512 .. 8192 bytes a step, a read-back 2 KiB, a load 16 KiB, a directory operation one step (`core::JobRunner`). One card job runs at a time. The render never sees the card.
  A FAT sync can take longer than the budget: `files` on the console prints the longest step.
- **Never a half-written file.** A save writes `_o3save.tmp`, reads it back and checks its size and CRC, and only then renames it over the old file, which stays as `<name>.bak`
  (`kit/app/atomic_save.h`, ported from track3r-os, has the sequence and what a cut at each step leaves). A power cut, a pulled card or a failing write leaves the old file or the new
  one. A card pulled and put back fails the save (the writer pins the card the save began on); the next save of that file heals the name. `NewOnly` never replaces.
- **Cards.** `card()` is None, Ready, ReadOnly (write protected: loads and listings work), Unsupported (exFAT), OnComputer or InUse (the firmware update path holds it);
  `generation()` moves when it changes, a load or listing in progress then fails with CARD CHANGED, and `Runtime::card_changed()` is the platform's card watch. `set_quiet(true)`
  silences the strip for the app's jobs. `cancel()` repairs at once.
- **Streams.** `save(folder, name, SaveSource&, mode)` writes from a source of your own (`atomic_save.h`; `save_sources.h` has the buffer and the WAV ones), `load(..., ByteSink&)`
  pushes a file into a sink 2 KiB at a time. `formats/chunk_file.h` is an optional container (an IFF-style `FORM`, chunks, a CRC: `docs/formats/o3sdk-chunk-file.md`) for an app that
  wants versioned data.
- **The kit's settings** are kept in `Apps/<id>/system.o3s` by the same service (`docs/formats/o3sdk-settings.md`): read at boot by `main.cpp` (`Runtime::load_settings`) and again whenever a card is put in,
  written only for a card whose file was read (or found missing), after 1.5 s of quiet and at once at the power-off, silent, and stepping aside for an app's save. A file of a newer major version
  is never overwritten (the session keeps its values, the strip says so).
- **No card.** `Platform::storage`, `writer` or `files_work` null (a test rig, an image without a card, `O3SDK_MK66_CONSOLE=OFF`): `available()` is false, every call is refused with
  NO CARD, nothing is kept, and the app runs. An app that never calls `files()` pays nothing in the render. The jobs' memory is a `Files::Work` (about 16 KB, `files_work.h`) the
  platform places in the SDRAM (`O3SDK_BIG`); the `Files` object in the runtime is a few hundred bytes.
- **The writers.** The card has two writers that never run at once: the update path (`Firmware/`, `update/path_guard.h`, nothing in common with the other) and the files service (`Apps/<id>/`,
  `Samples/`). The writer's guard (`hal/app_guard.h`) is a superset of track3r-os's (it also admits `Projects/`, `Settings/`, `instruments/`, `AppExport/`), so the service goes through a `ScopedWriter` that refuses any change outside `Apps/<id>/` and `Samples/` (`system.o3s` of the app's folder is the kit's: an app's save of it is RESERVED NAME). `platform/mk66/tools/check_writers.py` keeps the list of who may name the write calls and include the write contract. The service refuses while the
  update path holds the card (`Platform::card_claim`), and the update path refuses while the service has a job (`appimg::storage_in_use`).
- **Console**: `files` prints the card, the last job, the jobs' cost (`max-step`, the write speed of the last save) and what the settings read found; `files crc <path>` is the CRC-32 of
  a card file, to compare with the simulator's; `state` has `card=<state> files=<phase>`.

### Pad LEDs (`pad_leds.h`)

Monochrome, one level a pad. Shared levels give the same meaning on every screen: `kLedDim` (something is here), `kLedLow` (a second
kind), `kLedMid` (selected), `kLedOn` (now: the playhead, a held pad). `PadLeds::blink()` gives a blink phase.

## 6. The runtime

`runtime.h`. The same main loop on every platform:

```cpp
Runtime rt(o3sdk_app(), engine, platform);
rt.start();                                // the UI's bitmaps, App::setup, Engine::start
audio_device.start(rt.block_callback());   // the render context calls the engine once per block (int16 in and out)
for (;;) rt.poll();                        // input -> app -> screen; the card's job a slice; a frame every 33 ms (or sooner after redraw()); LEDs
```

A frame is the header strip (app name, screen title, tempo, transport, the render's load on the unit), the screen's content and the
soft-key row, as one display list (2048 words at most; HELLO peaks under 900).

## 7. The simulator

`sim/README.md`. Each app gets `<name>_sim`: the app on the host with a fake clock, scripted input, the FT81x interpreter for the
screen, a drawing of the pads with their LED levels, and the audio as a WAV. Scenarios are text (`apps/hello/scenarios/tour.txt`). Use
it for every screen change and every sound change before a unit sees it; it is not a model of a particular FT812, and its timing is
not the unit's. Its PNGs are 800 x 697 (the screen above the pads); the unit's `o3console.py --shot` is the 800 x 480 screen alone. The
`--out` directory must exist. The same app runs in tests through `sim::Harness` (`sim/include/o3sdk/sim/harness.h`: `start`, `run_ms`,
`press`, `pad_down`, `turn`, `audio`, `leds`, `runtime().state_line()`), as `templates/app/tests/app_test.cpp` and `tests/sim_hello_test.cpp` do.
`Harness::start()` and the `<app>_sim` runner fail on an app whose setup had a call the engine refused (section 4, "Setup").
`--midi` gives the app a fake MIDI device (`platform/host/fake_midi.h`, `hal::midi::Device`); a scenario with a `midi <din|usb> <hex...>` or a
`clock <bpm> start|stop|continue|off` line gets one without the option (`sim/README.md`). `--card DIR` gives it an SD card (`platform/host/dir_card.h` over the directory):
`Context::files()` works, the kit keeps its settings there, and a scenario may `card out`, `card in`, `card protect on|off`, `reboot` and `expect file|nofile|state`; `Harness` takes
any `HostCard` (a `MemCard` cut at every write, in the tests).

## 8. Rules

- Real-time paths (the render, ISRs, everything they call): no allocation, no SD I/O, no blocking, bounded loops, no `double`.
- No heap anywhere in the kit, the units or the apps; no RTTI, no exceptions.
- The CPU is the binding constraint: measure every per-block feature on the unit before relying on it.
- Warning-free under `-Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion`, with `-Werror`.
- Pure logic (DSP maths, sequencer arithmetic, formats) gets host tests.

## 9. The MK66 platform

`platform/mk66`, ported from track3r-os `platform/mk66` (whose README is the long account of every hardware fact and its source): the reset
sequence (clocks, SDRAM, caches, the crash record), the FT812 driver, the two TCA8418 key scanners and the encoder, the IS31FL3731 pad LEDs,
the I2S + eDMA + SGTL5000 audio pipeline behind `hal::audio::Device`, the linker script (with `.sdram_bss` placed in the SDRAM, NOLOAD, cleared
before the constructors), the USB console, USB audio, the SD card read path and the firmware update path (M2b-R), and `src/main.cpp`, the
composition root that wires them to `Runtime`. The MIDI core (`platform/mk66/midi`, ported from track3r-os, slice MC) and its service (`MidiService`,
the DIN driver and the USB binding, slice MD, 5937691) implement `hal::midi::Device`; the Power switch (`platform/mk66/power`, slice P)
is ported from track3r-os. Neither has run on a unit. `o3sdk_app()` links an app into an image (`<name>.elf`, `.hex`, `.bin`, `.ptf` behind the
static checks; ctests `<name>_image`, `_flashconfig`, `_ramfunc`, `_usb`). Its boot path, memory map, blink codes, options and what is verified:
`platform/mk66/README.md`; the host tools and static checks: `platform/mk66/tools/README.md`; the plans and who owned what: `docs/M2-PLAN.md`,
`docs/M2B-PLAN.md`. The drivers are the ones the unit proved in track3r-os and stay as they are there (`AGENTS.md`, "Rules").

## 10. Capabilities: the whole hardware, for every app

The kit's promise is that an app can use everything the unit has, through a contract, without touching a register: an engine or
machine (the 303, HOOKS, a port) picks what it needs. Each capability has a driver in `platform/mk66`, a contract in `kit/hal`
(or the kit layer that owns it), an API an app reaches through its `Context`, and a fake on the host so the simulator runs it.

**State, as of 2026-10-29.** Three values, and nothing in between:

- **Verified on the unit**: run on a Tracker and read back or recorded over USB by the test session (`docs/hw/2026-10-25-usb.md`: HELLO and
  o3-303, installed over the console). Where a person is still needed (eyes, hands, ears) the row says so: that part is not verified.
- **Host only**: code and host tests exist (against fakes); it has not run on a unit, or the unit has not exercised it.
- **Planned**: not built, or a contract header with no driver.

| Capability | Hardware | Contract | What an app gets | State |
|---|---|---|---|---|
| Display | FT812, 800×480, SPI + eDMA | `hal::Display` | screens, widgets, themes (`kit/ui`, `kit/app`) | **Verified on the unit** (M2): boots, draws every frame, `--shot` returns a clean 800×480 picture. Not looked at by eye; the shot's colours differ from the simulator's (the theme or the shot's colour order: open); the FT812's per-line fill rate is untested (`apps/fillrate`) |
| Buttons, pads, encoder | 2× TCA8418, FTM2 quadrature | `hal::Input` | `Input` events, `Panel` state, F-key + encoder (`ParamKeys`) | **Verified on the unit** as far as the console reaches (M2): no blink code at boot (13 would be a scanner that did not answer), no ghost input in 10 s, injected `key`/`pad`/`enc` drive the app. Real presses by hand: not verified |
| Pad LEDs | IS31FL3731, 48 monochrome | `hal::Leds` | `PadLeds` per screen | **Verified on the unit** by readback (M2): online, 0 errors, in sync, a pad's level follows its press. By eye: not verified |
| Audio out | SGTL5000, I2S0 + eDMA, 44.1 kHz | `hal::audio::Device` | the engine: units, slots, sends, master, transport | **Verified on the unit** (M2): the pipeline runs with 0 underruns and 0 faults over a 10-minute soak; the engine's master was recorded over USB (HELLO a pad at 523 Hz; o3-303 playing). The analogue outputs by ear: not verified |
| CPU and memory | M4F 180 MHz, 256 KiB SRAM, 16 MiB SDRAM | `hal::cycles`, `O3SDK_BIG` | the render's load in the header and `o3console.py cpu`; SDRAM buffers; later an SDRAM arena | **Verified on the unit** (M2): HELLO renders 19.6 % of a block at idle; o3-303 at most 39.8 % playing; SDRAM test and `.sdram_bss` buffers run |
| Crash record, status LED | noinit SRAM, PTC19 | platform | a blink code and the record of the last crash | **Host only** (M2): the images boot without a blink code, but a crash or a code was never provoked on the unit, so the record's survival of a reset is not verified |
| USB console and update | USB0 device, CDC | `hal::firmware` | `diag` over USB: version, state, cpu, log, leds, screenshot, input injection, `fw put/arm/update`; an app adds its own state and commands (`App::describe_state`, `diag_commands`) | **Verified on the unit** (M2b-R): images put, installed and read back; the way back to track3r-os v1.0.0 in about 45 s |
| USB audio out | USB0 device, isochronous, 44.1 kHz, 16-bit, composite with the console | `hal::usb_audio::Device`, `Context::usb_audio()`, every app | the master streamed to the computer as a class-compliant interface (channels 7-8 of 8; 1-6 are silent until an app or the user maps a stem), per-slot stems, the computer's return mixed into the output and as the audio in's source, the USB profile, the card as a disk | **Verified on the unit** (M2b-R): AUDIO set, profile 8, 8 out / 2 in, configured, recorded. **Host only** (M2c slice U): the stems, the return, the profiles, the disk mode and the Config rows (`usb.txt`, `sim_usb_test`); P11, P10 duplex, `cpu` with stems and the disk against a computer: on the unit |
| SD card | SDHC, FAT (FatFs) | `hal::storage` (read), `Context::files()` (`app::Files`) | the card is mounted and read; `fw put` writes `Firmware/`; an app saves, loads and lists its files in `Apps/<id>/` and the kit keeps its settings there | **Verified on the unit** for the read path and for the update path's writes (M2b-R). **The app file API** (M2c slice FA) is **host only**: built and tested on the host (an in-memory card cut at every write, a FAT32 image through the unit's FatFs writer, the simulator's directory card), the image carries it, but no save has run on a unit. `hal::storage` stays read-only by contract; the writer is `hal::storage_write` through `platform/mk66/save` and the guard `hal/app_guard.h`; the card has two writers with disjoint paths, the update path (`Firmware/`) and the files service (`Apps/<id>/`, `Samples/`), kept apart by the card claims (`check_writers.py`). Still to check on a unit: `files` after a HELLO save (bytes, `max-step`, write speed), `files crc` against the simulator's file, 0 underruns and 0 sheds over 20 scripted saves, `fw put` refused during a save, the theme kept after `reset`, and by hand a card pulled mid-save, the write-protect switch and the Power switch held during a save |
| Firmware switcher | the bootloader (`.ptf` from `Firmware/`), track3r-os's update module | kit system screen, every app | Config, FIRMWARE: every app lists the `.ptf` images in a card folder, checks one, asks twice (F1 each), stages it and asks the bootloader to flash it, so the unit changes engine by changing image (the maintainer's requirement, 2026-10-25) | **Verified on the unit** in part (M2b round 2, slice F): the FIRMWARE section opened and worked over USB injection on 2026-10-26 (reported by the tester). **Host only** for the flow and its two questions (a fake sink, `config.txt`). Not verified: the final F1 INSTALL from the screen (it needs a finger); the path behind it is the USB console row |
| System Config screen | — | kit, every app | the Config button and the kit's pages (section 5) | **Verified on the unit** in part (M2b round 2, slices F and K): FIRMWARE, DISPLAY and INFO opened and worked over USB injection on 2026-10-26 (reported by the tester; no `docs/hw` file yet). **Host only**: the MIDI section (when `Platform::midi` is set). **Planned**: AUDIO and an app's pages |
| MIDI | DIN in/out (UART), USB MIDI | kit `hal::midi::Device`, `app::Midi`, console `midi` | notes into the engine through routes, every other message to `App::midi`, clock followed and sent on the transport; MIDI learn is planned | **Host only** (M2b round 2, slices MC, K, MD): the MIDI core is ported from track3r-os (`platform/mk66/midi`, 1104b85); the kit API, routes, clock follow and `FakeMidi` are host-tested (slice K); the MK66 service, the DIN and USB drivers and console `midi` are built and host-tested (slice MD, 5937691: host ctest 147/147, ASan 147/147, mk66 25/25). HELLO's image grows by 19.4 KB of flash. Not run on a unit. Owed: the MD row of `docs/M2B2-PLAN.md` section 6, last column; the ISR latency and the render cost of MIDI |
| Power switch | PTC3 | `core::PowerHold`, `platform/mk66/power` | a clean shutdown: silent, dark, card released | **Host only** (M2b round 2, slice P): the pure logic and the shutdown are ported from track3r-os's `platform/mk66/power` (11 files identical after the rename) and host-tested (`mk66_power_*`); console `power`, `power press/release/pin`, `power off [dry]`; `O3SDK_POWER_VLPR_OFF` ON as in track3r-os. Never run on a unit: the pin's pull and polarity, and the VLPR wake, are open |
| Audio in | SGTL5000 ADC: line in, microphone | `hal::capture` | input planes in every block (`in_l`, `in_r` of a unit; `BlockContext::input_l()` of a hook), `Context::audio_in()`: input select, gain, gate, monitor, peaks; the Config's AUDIO section; recording into SDRAM (`Context::recorder()`, see Recording) | **Built, host only** (M2c slice AI: `capture.cpp`, `audio_input_apply`, the kit's `AudioIn`, the fake codec in the simulator; the image builds and passes its static checks). Not verified on a unit: line in was never seen on one, the microphone never measured, the analogue switch's polarity is assumed. Recording: **built, host only** (M2c slice RC: the record service, `Context::recorder()`, HELLO's REC, console `rec`; the cost and the take's lag are not measured on a unit) |
| FM radio | Si4703 into the codec input | `hal::capture` | `Context::radio()`: tune, seek, region, status; RDS is not in this round; a source to sample | **Built, host only** (M2c slice AI: the ported Si4703 driver behind `capture.cpp`, the fake in the simulator). The first answer on a unit decides whether FM exists on this board (OS-RAD1 failed on its first run in track3r-os; the board's radio wiring is unconfirmed): Absent shows as "not on this unit" |
| Computer return stream | USB0 isochronous (UAC2) | platform | the computer's audio as an engine source | **Planned** (later) |

Not a hardware capability but part of the promise: the **engine** (units, slots, sends, the transport, clocked hooks) is verified on the unit
through HELLO's riff and o3-303's 303 voice and sequencer; the **simulator** and the **gate** (`fw-gate-check`) are host tools.

Rules for adding one: the contract is designed from what an app needs (not from the register map), it has a host fake and a
simulator command, and an app that does not use a capability pays nothing in the render and as little flash as possible.

## 11. Using o3sdk from another project

An app can live in its own repository (o3-303 is the first) and use o3sdk as a sibling checkout: no copy, and no edit of o3sdk from
there. **Start from the template**: `python3 -I tools/new_app.py <dir> <app-id>` writes a repository laid out like o3-303's (`templates/app`:
CMake, one app and screen, one unit, tests, a scenario, `CLAUDE.md`, `AGENTS.md`, the CI scripts and the agent harness); `docs/NEW-APP.md` is the
tutorial. What the template's top-level `CMakeLists.txt` does, in its essentials:

```cmake
cmake_minimum_required(VERSION 3.24)
set(O3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../o3-sdk" CACHE PATH "The o3sdk framework")
include("${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")   # BEFORE project(): O3SDK_PLATFORM, the toolchain, the compile settings
project(myapp LANGUAGES C CXX)
enable_testing()                                    # the project's own root, for its ctests
add_subdirectory(${O3SDK_DIR} o3sdk)
add_subdirectory(units)                             # o3sdk_unit(...)
add_subdirectory(app)                               # o3sdk_app(myapp SOURCES ... DEPENDS ...)
add_subdirectory(tests)                             # o3sdk_app_test_suite(myapp ...), host only
```

- **The platform file** decides what has to be decided before `project()`: `-DO3SDK_PLATFORM=host|mk66`, the Arm toolchain file
  (`-DARM_GNU_ROOT=...`, CLAUDE.md), and the compile settings every target shares (C++17, no RTTI, no exceptions, the audio clock
  define, the cross flags, `O3SDK_SANITIZE`). Included after `project()` it stops the configure with a message.
- **What gets built** from the outside: the kit, o3sdk's units (`unit_synth`, `unit_delay`) and the platform (the HAL fakes and the
  simulator on the host; the drivers and the image pipeline on mk66). o3sdk's own apps (HELLO), its tests and its tools' self-tests are
  built only when o3sdk is the top-level project: `O3SDK_BUILD_APPS` and `O3SDK_BUILD_TESTS` default to that, and `-D...=ON` runs them
  from the outside too.
- **The helpers** (`cmake/O3sdkTargets.cmake`) run in the caller's directory, so sources and `include/` are the project's own:
  `o3sdk_unit`, `o3sdk_library`, `o3sdk_app` (the library `app_<name>`; on the host the simulator `<name>_sim` in the app's binary
  directory; on mk66 `<name>.elf`, `.hex`, `.bin`, `.ptf` there too, and the ctests `<name>_image`, `<name>_flashconfig`,
  `<name>_ramfunc` and, for an image with the console, `<name>_usb`; `LOGO`, below), `o3sdk_test_suite` (doctest: one executable, one ctest per
  `*_test.cpp`; a test that names `o3sdk_app()` links the app library whole, `DEPENDS "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<name>>"`) and
  `o3sdk_app_test_suite(<name> SOURCES ... [DEPENDS ...])`, the same with an app's wiring done: the executable `<name>_tests` linked with
  `app`, `sim`, `platform_host`, the DEPENDS and the app library whole, and `../app/scenarios` as the macro `APP_SCENARIO_DIR`
  (`SCENARIO_DIR`, `SCENARIO_DEFINE` and `EXE` change that). The explicit `o3sdk_test_suite` form keeps working.
- **The boot logo** (`LOGO <png> [LOGO_COLOR 0xRRGGBB]` in `o3sdk_app`; OS-003f): the first frame the display shows is the logo centred on black with
  the app's name and build line under it (`ui/boot_screen.h`). The kit's default is the O3 logo (`logo/o3.png`, a halftone: 684 x 420, one bit a pixel
  at its own size, 36,120 bytes of flash). An app gives its own: the build runs `tools/logo_convert.py` on the PNG (a path from the app's directory)
  into the app's build directory and links that bitmap instead; without `LOGO` the app has the default. The converter chooses the native size, the scale
  and the format: the finest native size whose bitmap, padding and palette fit `kBootLogoMaxBytes` (64 KiB, `ui/boot_logo.h`: the RAM_G region of the map
  and the most flash a logo costs), shown at most 800 x 420 px, at an integer scale (1 up to 16); a nearly black-and-white picture (a halftone, line art)
  is **L1**, any other grey picture **L2 or L4**, and a picture with colour (any pixel with chroma over 32) **PALETTED565**: an 8-bit index a pixel and a
  palette of at most 256 colours (index 0 black, median cut, no dithering) after it in the same blob, drawn in one pass with `PALETTE_SOURCE`, which costs
  512 bytes more and one more display-list word. A grey logo is drawn white, or in `LOGO_COLOR` (`COLOR_RGB`; refused for a picture with colour). A
  picture over a limit fails the build with the converter's message, and so does a header that disagrees with the tool. The draw code reads width, height,
  format, stride, scale, palette and tint from the linked data (`ui::BootLogo`, `boot_logo()`): the generated file defines `boot_logo()` strongly and the kit's
  default is weak, so there is one definition in an image and no duplicate symbol. `python3 -I tools/logo_convert.py --png <png> --report --preview <out.png>`
  says what it chose and writes the native bitmap at the size it is shown; the simulator's `boot <name>` command draws the boot frame (`sim/README.md`).
  Hardware facts a unit still has to show: the 565 palette's channel widening, that `COLOR_RGB` white leaves a palette untouched, and the block edges of a
  scale that is not a power of two.
- **Where o3sdk is**: `-DO3SDK_DIR=<path>` or the environment variable `O3SDK_DIR` (the template reads it; `tools/ci.sh` too), default the
  sibling `../o3-sdk`. A cache entry keeps the value of the first configure.
- **A path inside o3sdk** is `O3SDK_ROOT` (set by the platform file), never `PROJECT_SOURCE_DIR`, which belongs to the project that
  calls the function. A variable a helper reads must be a cache entry (`O3SDK_PYTHON`, `O3SDK_SINGLE_PRECISION_LAYERS`).
- **The build identity** in the image (`gen_build_info.cmake`) is two git states: the kit's (o3sdk's own directory, wherever the project
  keeps it) and the app's (the top-level project's; the same tree when the app is one of o3sdk's own). `O3SDK_IMAGE_LABEL` joins them,
  "o3sdk d8c4ede2c609" or "<app label> / o3sdk <kit label>", and is the boot screen's build line and the image's `g_build_label`
  (`strings` on the `.bin` or the `.elf` shows it; the HEX and the `.ptf` store hex digits).
