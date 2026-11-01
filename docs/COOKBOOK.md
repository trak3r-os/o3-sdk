# Cookbook: short recipes for an app on o3-sdk

Each recipe is one task: the goal, the files to touch, a minimal sketch with the kit's real names, the test or the scenario that proves it,
and where the reference is. The paths are those of a repository made by `tools/new_app.py` (here with the id `myapp`): `app/`, `units/`,
`tests/`, with o3-sdk checked out next to it; the USB tools are `O3=../o3-sdk/platform/mk66/tools`, as in NEW-APP. The tutorial that builds such a repository from nothing is [NEW-APP.md](NEW-APP.md); the reference is
[FRAMEWORK.md](FRAMEWORK.md); [TOUR.md](TOUR.md) shows the results. The sketches leave out includes and the parts that do not change; the template's files have them whole.

Two rules hold in every recipe: the render (a unit's `process()`, a hook's `block()`) never allocates, blocks, touches the card or uses
`double`; and a screen or sound change is not done until a scenario shows it (`FRAMEWORK.md` [section 8](FRAMEWORK.md#8-rules)).

1. [A new unit with three parameters, end to end](#1-a-new-unit-with-three-parameters-end-to-end)
2. [Add a parameter](#2-add-a-parameter)
3. [Add a screen](#3-add-a-screen)
4. [Give a screen HELP](#4-give-a-screen-help)
5. [Add a Config page for the app](#5-add-a-config-page-for-the-app)
6. [Add a console command](#6-add-a-console-command)
7. [Draw with the Canvas and your own bitmaps](#7-draw-with-the-canvas-and-your-own-bitmaps)
8. [Save and load a file on the card](#8-save-and-load-a-file-on-the-card)
9. [React to MIDI notes](#9-react-to-midi-notes)
10. [Record a take](#10-record-a-take)
11. [Write a scenario and pin its digests](#11-write-a-scenario-and-pin-its-digests)
12. [Measure the CPU on the unit](#12-measure-the-cpu-on-the-unit)

## 1. A new unit with three parameters, end to end

**Goal.** An effect, FUZZ, with DRIVE, TONE and LEVEL, on slot 0 after the template's voice, edited with F1..F3 on a second page.

**Touch.** `units/fuzz/CMakeLists.txt`, `units/fuzz/include/myapp/units/fuzz/fuzz.h`, `units/fuzz/src/fuzz.cpp` (new); `app/CMakeLists.txt`
and `tests/CMakeLists.txt` (`unit_fuzz` in `DEPENDS`); `app/src/app.cpp` (attach and route); the screen (a `ParamPage`); `tests/fuzz_test.cpp`.

```cmake
o3sdk_unit(fuzz SOURCES src/fuzz.cpp)        # units/fuzz/CMakeLists.txt: the library unit_fuzz, which links dsp and nothing else
```

```cpp
// fuzz.h
class Fuzz final : public o3sdk::dsp::Unit {
 public:
  enum Param : std::uint16_t { Drive, Tone, Level, kParamCount };  // the id is the index in the table: append, never insert
  const o3sdk::dsp::UnitInfo& info() const noexcept override;
  void prepare(float sample_rate) noexcept override;              // main loop, before Engine::start()
  void process(const o3sdk::dsp::ProcessArgs& args) noexcept override;  // the render, one block of at most 64 frames
 private:
  void set(std::uint16_t id, float v) noexcept;
  float fs_ = 44100.0f, drive_ = 1.0f, tone_ = 0.5f, lp_l_ = 0.0f, lp_r_ = 0.0f;
  o3sdk::dsp::Ramp gain_;                                          // the level is ramped: a fast turn does not click
};

// fuzz.cpp
using o3sdk::core::ParamCurve;
using o3sdk::core::ParamUnit;
// name, unit text, names, knots, min, max, default, detents, coarse, list count, display, curve, flags, group
constexpr o3sdk::core::ParamDesc kParams[Fuzz::kParamCount] = {
    {"DRIVE", "%", nullptr, nullptr, 0.0f, 100.0f, 30.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},
    {"TONE", "Hz", nullptr, nullptr, 500.0f, 12000.0f, 4000.0f, 120, 10, 0, ParamUnit::Hz, ParamCurve::Log, 0, 0},
    {"LEVEL", "dB", nullptr, nullptr, -48.0f, 6.0f, -6.0f, 108, 12, 0, ParamUnit::Fixed1, ParamCurve::Linear, 0, 0},
};
constexpr o3sdk::dsp::UnitInfo kInfo{"FUZZ", o3sdk::dsp::UnitKind::Effect, kParams, Fuzz::kParamCount, nullptr, 0};

const o3sdk::dsp::UnitInfo& Fuzz::info() const noexcept { return kInfo; }

void Fuzz::prepare(float sample_rate) noexcept {
  fs_ = sample_rate;
  lp_l_ = lp_r_ = 0.0f;
  gain_.set_length(static_cast<std::uint32_t>(0.005f * fs_));
  for (std::uint16_t p = 0; p < kParamCount; ++p) set(p, kParams[p].def);
  gain_.jump(gain_.target_value());
}

void Fuzz::set(std::uint16_t id, float v) noexcept {   // once per event, never per sample: libm stays out of the sample loop
  if (id == Drive) drive_ = 1.0f + 0.2f * v;
  if (id == Tone) tone_ = 1.0f - std::exp(-6.2831853f * v / fs_);
  if (id == Level) gain_.target(std::exp(0.11512925f * v));    // dB to linear
}

void Fuzz::process(const o3sdk::dsp::ProcessArgs& a) noexcept {
  o3sdk::dsp::for_each_run(
      a,
      [&](std::uint32_t from, std::uint32_t to) {               // render up to the next event...
        for (std::uint32_t i = from; i < to; ++i) {               // (in place is allowed: in and out may be the same buffer)
          const float g = gain_.next();
          const float l = a.in_l[i] * drive_, r = a.in_r[i] * drive_;
          lp_l_ += tone_ * (l / (1.0f + std::fabs(l)) - lp_l_);
          lp_r_ += tone_ * (r / (1.0f + std::fabs(r)) - lp_r_);
          a.out_l[i] = lp_l_ * g;
          a.out_r[i] = lp_r_ * g;
        }
      },
      [&](const o3sdk::dsp::Event& e) {                           // ...apply it, go on
        if (e.kind == o3sdk::dsp::EventKind::Param) set(e.param, e.value);
      });
}
```

```cpp
// app.cpp, setup(): the engine's setup phase
const dsp::UnitId fuzz = e.attach(fuzz_);     // fuzz_ is a member of the app: no heap
(void)e.set_insert(0, 0, fuzz);               // slot 0, first of its four inserts; a refused call fails the simulator and app_test
play_.init(voice, fuzz);                      // the template's init(voice) gains the second id

// play_screen.cpp: the template's one `page_` becomes a member `app::ParamPage pages_[2]` (the table must outlive ParamKeys);
// ParamKeys::next_page() on a button of your choice turns to the second page
void PlayScreen::init(dsp::UnitId voice, dsp::UnitId fuzz) noexcept {
  // ... the template's resets, as before ...
  pages_[0] = app::ParamPage{"VOICE", voice, {Voice::Wave, Voice::Decay, Voice::Level, 0, 0, 0, 0, 0}, Voice::kParamCount};
  pages_[1] = app::ParamPage{"FUZZ", fuzz, {Fuzz::Drive, Fuzz::Tone, Fuzz::Level, 0, 0, 0, 0, 0}, Fuzz::kParamCount};
  keys_.set_pages(pages_, 2);
}
```

**Proof.** `tests/fuzz_test.cpp`, the shape of the template's `tests/voice_test.cpp`: `core::param_desc_valid()` on every row, then an
`dsp::Engine` with the voice as slot 0's source and FUZZ as its insert, `e.start()`, `e.note_on(...)`, `e.render(nullptr, nullptr, l, r, 64)`
for a few hundred blocks: every sample finite and below 1, LEVEL at -48 dB quieter than at 0, a step of LEVEL with no click. Then a scenario
(`record on`, a pad, `wav fuzz`) and listen to the WAV. State the cost in the header as an estimate and measure it on the unit (recipe 12).

**Read.** `NEW-APP.md` [section 3](NEW-APP.md#3-add-a-unit-dsp), `FRAMEWORK.md` [Units](FRAMEWORK.md#3-units),
[The engine](FRAMEWORK.md#4-the-engine); the smallest complete unit is the template's `units/voice/`, an effect with SDRAM buffers is
`units/delay`.

## 2. Add a parameter

**Goal.** A fourth value on the template's voice, TUNE in cents, without breaking anything saved or bound.

**Touch.** `units/voice/include/myapp/units/voice/voice.h` and `src/voice.cpp`; `app/src/play_screen.cpp` (the page and HELP).

```cpp
enum Param : std::uint16_t { Wave, Decay, Level, Tune, kParamCount };   // at the end: Wave, Decay, Level keep their ids
// kParams, the new row last:
    {"TUNE", "ct", nullptr, nullptr, -100.0f, 100.0f, 0.0f, 200, 10, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0},
// Voice::set(): case Tune: keep it, and use it where the note's increment is computed (in apply(), at NoteOn)
// PlayScreen::init(): F4 drives it; the soft key's label and the knob cell come from the descriptor
page_ = app::ParamPage{"VOICE", voice, {Voice::Wave, Voice::Decay, Voice::Level, Voice::Tune, 0, 0, 0, 0}, Voice::kParamCount};
// PlayScreen::help(): h.fkey[3] = "FINE TUNE IN CENTS";
```

Why append: a saved project, a MIDI learn binding (the kit keeps unit and parameter ids on the card) and a test name a parameter by its id.
Inserting a row in the middle silently moves every value after it. A parameter you no longer want stays in the table (hide it from the pages).

**Proof.** The template's `voice_test.cpp` checks every row with `param_desc_valid()` (the new one too); `app_test.cpp` runs `help_check()`;
a scenario `tap f4`, `turn 10`, `snap 01_tune` shows the fourth cell.

**Read.** `NEW-APP.md` [section 6](NEW-APP.md#6-parameters); the descriptor's fields are in `kit/core/include/o3sdk/core/param.h`.

## 3. Add a screen

**Goal.** A second screen, SONG, that the SONG button opens and NOTE leaves.

**Touch.** `app/src/song_screen.h` and `.cpp` (new), `app/CMakeLists.txt` (`SOURCES`), `app/src/app.cpp`, `app/scenarios/song.txt` (new).

```cpp
class SongScreen final : public o3sdk::app::Screen {
 public:
  const char* title() const noexcept override { return "SONG"; }      // the header strip shows it
  void input(o3sdk::app::Context& ctx, const o3sdk::app::Input& in) noexcept override;  // what the app did not take
  void draw(o3sdk::app::Context& ctx, o3sdk::ui::Renderer& r, const o3sdk::ui::Rect& area) noexcept override {
    r.text(16, area.y + 12, "SONG", o3sdk::ui::Color::Text, o3sdk::ui::Face::Display);  // tokens and faces, never RGB
  }
  void soft_keys(o3sdk::app::Context&, o3sdk::app::SoftKeys& keys) noexcept override { keys.label[0] = "NEW"; }
  void leds(o3sdk::app::Context&, o3sdk::app::PadLeds& leds) noexcept override { leds.set(0, o3sdk::app::kLedDim); }
};

// app.cpp: screens are plain members; the screen buttons are global keys, so App::input switches
bool input(app::Context& ctx, const app::Input& in) noexcept override {
  if (in.press(app::Button::Song)) { ctx.show(song_); return true; }   // leave() of the old, enter() of the new, right after this event
  if (in.press(app::Button::Note)) { ctx.show(play_); return true; }
  return false;                                                        // the screen gets it
}
```

All of it runs in the main loop: a screen talks to the sound only through `ctx.engine()` and reads it through `engine().stats()`.

**Proof.** `app/scenarios/song.txt`: `wait 100`, `tap song`, `snap 01_song`, `tap note`, `snap 02_back`. The template's `app_test.cpp` runs
every scenario in the directory and fails on a wrong command or a frame over 2,048 words; open the PNGs. Then recipe 4.

**Read.** `NEW-APP.md` [section 4](NEW-APP.md#4-add-a-screen) and [section 5](NEW-APP.md#5-controls-and-pads), `FRAMEWORK.md`
[Apps](FRAMEWORK.md#5-apps).

## 4. Give a screen HELP

**Goal.** Shift + Config on SONG shows what its buttons and pads do.

**Touch.** The screen (`help()`), `tests/app_test.cpp`, `app/scenarios/help_song.txt` (new).

```cpp
void help(const o3sdk::app::Context&, o3sdk::app::Help& h) const noexcept override {   // data only: the kit draws it
  static constexpr o3sdk::app::HelpButton kButtons[] = {
      {o3sdk::app::Button::Note, "BACK TO PLAY", nullptr},
      {o3sdk::app::Button::Play, "PLAY THE SONG", "SHIFT FROM THE START"},
  };
  static constexpr o3sdk::app::HelpPadGroup kGroups[] = {{"ROWS", "PICK A ROW OF THE SONG", o3sdk::ui::Color::Sel}};
  h.summary = "THE ROWS OF THE SONG, ONE A PAD. F1 ADDS A ROW.";           // upper case, cut at its limits
  h.fkey[0] = "ADD A ROW";
  h.buttons = kButtons;
  h.button_count = sizeof kButtons / sizeof kButtons[0];
  h.groups = kGroups;
  h.group_count = 1;
  o3sdk::app::help_pads(h, 0, 0, 3, 11, 1, nullptr);                    // the whole grid in group 1
  h.pads = "ONE PAD A ROW, FROM THE TOP LEFT.";
}
```

**Proof.** In a test, open it and check it with the overlay's own limits, as the template's `app_test.cpp` does:
`h->press(app::Button::Shift)`, a tap of `app::Button::Config`, `h->release(app::Button::Shift)`, then
`app::help_check(h->runtime().help_overlay().help(), &problem)` with `app::help_field_name(problem.field)` in the message. And a scenario:
`press shift`, `tap config`, `release shift`, `snap 01_buttons`, `tap f2`, `snap 02_pads`, `tap f8`; look at both pictures.

**Read.** `FRAMEWORK.md` [Help](FRAMEWORK.md#help-shift--config-helph-help_overlayh) (every limit) and
[Give your app HELP](FRAMEWORK.md#give-your-app-help); the template's `PlayScreen::help()` is the smallest complete one.

## 5. Add a Config page for the app

**Goal.** A tab of the app's own in the system Config, with two values the kit keeps on the card and one action.

**Touch.** `app/src/app.cpp`.

```cpp
constexpr std::uint16_t kCfgTempo = 1, kCfgShift = 2, kCfgReset = 3;  // keys 1..65535, never reused: the settings file keeps them
constexpr core::ParamDesc kTempoDesc = {"TEMPO", "BPM", nullptr, nullptr, 40.0f, 240.0f, 120.0f, 200, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
constexpr core::ParamDesc kShiftDesc = {"RIFF SHIFT", "st", nullptr, nullptr, -12.0f, 12.0f, 0.0f, 24, 12, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
constexpr app::ConfigItem kCfgItems[] = {{kCfgTempo, "TEMPO", &kTempoDesc}, {kCfgShift, "RIFF SHIFT", &kShiftDesc}, {kCfgReset, "DEFAULT RIFF", nullptr}};

unsigned config_pages() const noexcept override { return 1; }                       // at most kMaxConfigPages (4)
bool config_page(unsigned index, app::ConfigPage& out) const noexcept override {
  out = app::ConfigPage{"HELLO", kCfgItems, sizeof kCfgItems / sizeof kCfgItems[0]};  // at most kMaxConfigItems (8)
  return index == 0;
}
float config_get(const app::Context& ctx, std::uint16_t key) const noexcept override;   // the value the row shows
void config_set(app::Context& ctx, std::uint16_t key, float value) noexcept override;   // the encoder or F1 changed it
void config_run(app::Context& ctx, std::uint16_t key) noexcept override;                // an action: F1, then F1 again
```

The kit gives the kept values back after `setup()` has set the defaults, clamped into each descriptor; a row with a zero or repeated key or a
descriptor `param_desc_valid()` refuses is left out.

**Proof.** HELLO's `apps/hello/scenarios/config_pages.txt` (run with `--card` and `--audio-in`, as its header says: without the AUDIO tab the
`tap right` counts land on the wrong tab) walks the tabs to HELLO's page, changes a value, reboots and
finds it again; `tests/app_config_pages_test.cpp` covers the rules. Picture: `TOUR.md`, [the app's own page](TOUR.md#the-apps-own-page).

**Read.** `FRAMEWORK.md` [The system Config](FRAMEWORK.md#the-system-config-the-kits-in-every-app), "The app's own pages";
`kit/app/include/o3sdk/app/config_page.h`; the whole example is `apps/hello/src/hello_app.cpp`.

## 6. Add a console command

**Goal.** `o3console.py riff` prints the app's riff over USB.

**Touch.** `app/src/app.cpp`.

```cpp
MyApp& instance() noexcept;                                     // the app object, which o3sdk_app() also returns

void riff_command(const app::DiagArgs& args, app::DiagOut& out) noexcept {   // main loop, between two passes of poll()
  const MyApp& a = instance();
  out.text("riff shift ").sdec(a.shift()).nl();
  if (args.arg(1)[0] != '\0') out.text("no arguments: ").text(args.arg(1)).nl();
}
constexpr app::DiagCommand kCommands[] = {
    {"riff", "riff: the riff's steps and its shift", &riff_command},   // a name of 2-7 lower-case letters, not a kit command's
};

std::size_t diag_commands(const app::DiagCommand*& rows) const noexcept override {
  rows = kCommands;
  return sizeof kCommands / sizeof kCommands[0];                  // the console takes at most 16
}
std::size_t describe_state(const app::Context& ctx, char* out, std::size_t cap) const noexcept override {
  app::StateWriter w(out, cap);                                   // `state` prints the kit's pairs, then these
  w.sint("shift", shift_);
  return w.size();
}
```

A row whose name breaks the rule, or carries a word of the update path, is skipped and logged; the app reaches the console only through these
two methods.

**Proof.** On the host, call the handler with a `DiagOut` over a `DiagSink` that appends to a string (`tests/app_diag_test.cpp` has the sink)
and check the text; `describe_state` shows in every `Harness` test through `runtime().state_line()` and in a scenario through
`expect state shift=0`. On the unit, after an install: `python3 -I $O3/o3console.py riff state`.

**Read.** `FRAMEWORK.md` [Diagnostics](FRAMEWORK.md#diagnostics-the-consoles-hooks-diagh), `NEW-APP.md`
[section 12](NEW-APP.md#12-reference-the-console-hooks-an-app-can-add); a complete example is `apps/crashtest/src/crashtest_app.cpp`
(`fault`).

## 7. Draw with the Canvas and your own bitmaps

**Goal.** A sprite sheet of the app's own in the display's RAM_G, drawn with rounded boxes on a screen.

**Touch.** A header with the bitmap and its tables (made by your own converter), `app/src/app.cpp` (`assets()`), the screen's `draw()`.

```cpp
alignas(4) constexpr std::uint8_t kKnobs[8 * 16 * 2] = { /* 8 cells of 16 x 16, L1: 2 bytes a row */ };
constexpr app::AppAsset kRows[] = {{0, kKnobs, sizeof kKnobs}};          // offset from kRamGApp; offset and size on the 4-byte grid
static_assert(app::app_assets_valid(kRows, 1), "inside the app's region, aligned, disjoint");
constexpr ui::plot::BitmapDesc kKnobDesc{ui::plot::kRamGApp + 0, eve::BitmapFormat::L1, 2, 16, 16, 16, 16};
static_assert(ui::plot::bitmap_desc_valid(kKnobDesc), "a desc the chip can draw");

// the app: Runtime::start() uploads the rows after the kit's bitmaps, before setup()
std::size_t assets(const app::AppAsset*& rows) const noexcept override { rows = kRows; return 1; }

// Screen::draw(): one raw section, by layer and colour; text outside it
if (ctx.app_assets_ok()) {                                               // false: the upload failed, run without the bitmaps
  const ui::Theme& t = r.theme();
  ui::plot::Canvas cv(r);
  cv.open();
  cv.round_rect(ui::Rect{90, 190, 36, 36}, 6 * 16, t.rgb(ui::Color::Empty));   // radius in 1/16 px
  cv.bind(kKnobDesc);                                                    // bitmap handle 0: the app's
  cv.sprite(3, 100, 200, t.rgb(ui::Color::Accent));                      // cell 3, tinted by a theme token
  cv.close();
}
r.text(90, 232, "CUT", ui::Color::Text, ui::Face::Micro);
```

A Renderer call between `open()` and `close()`, an invalid desc or a call outside a section aborts on the host and in a debug build.

**Proof.** A scenario that shows the screen, and its PNG; the simulator's second summary line counts `handle 0 used N times` (more than 0 now,
which a `--run` of `digest_check.py` refuses: pin this app's frames with `--have`, recipe 11). The kit's rules are tested in
`tests/canvas_test.cpp`, `tests/app_assets_test.cpp` and `tests/handle_app_test.cpp`. The simulator draws a rounded box as a plain one: the
corners are the unit's to show.

**Read.** `FRAMEWORK.md` [The app's bitmaps and shapes](FRAMEWORK.md#the-apps-bitmaps-and-shapes-app_assetsh-uiplotcanvash) (the word
cost of each call); a full skin built this way is o3-303's `app/src/skin/` and its `App::assets()` in `app/src/o3_303_app.cpp`.

## 8. Save and load a file on the card

**Goal.** FILE saves the pattern to `Apps/myapp/pattern.o3a`; SAMPLE LOAD loads it back. The sound and the screen never wait.

**Touch.** `app/src/app.cpp` (the template already does exactly this; read its header comment).

```cpp
// App::input: a call only records the request and returns
app::Files& files = ctx.files();
if (op_ == Op::None && !files.busy()) {                                    // one job at a time
  files.set_quiet(false);                                                  // the user asked: the kit's strip says SAVED, NO CARD, ...
  const std::size_t n = write_pattern(play_.pattern(), save_buf_, sizeof save_buf_);
  op_ = n != 0u && files.save("", "pattern.o3a", save_buf_, n) ? Op::SavePattern : Op::None;   // save_buf_ stays untouched until the job ends
}
// App::update: poll the job, every pass
const app::Files::Status st = files.status();
if (st.phase == app::Files::Status::Done || st.phase == app::Files::Status::Failed) {
  // a load that is Done: files.loaded() bytes in load_buf_; check them before using them (the template's read_pattern)
  op_ = Op::None;
}
// a load: files.load("", "pattern.o3a", load_buf_, sizeof load_buf_)
```

The kit writes a temporary file, reads it back, checks it, then renames it over the old one, which stays as `pattern.o3a.bak`. Names are
`<stem>.<ext>` with the extension `o3a`, `o3s`, `wav`, `mid`, `syx`, `txt`, `json` or `csv`, in `Apps/<id>/` or one project folder under it.

**Proof.** The template's `app/scenarios/files.txt`, run with `--card <dir>`: `tap file`, `wait 60`, `expect state files=done`,
`expect file Apps/myapp/pattern.o3a`, then a second save and `expect file Apps/myapp/pattern.o3a.bak`. Its `app_test.cpp` saves, restarts on
the same card and finds the pattern again. The kit's own: `tests/app_files_test.cpp` (a card cut at every write).

**Read.** `FRAMEWORK.md` [Files on the card](FRAMEWORK.md#files-on-the-card-filesh), `kit/app/include/o3sdk/app/files.h`.

## 9. React to MIDI notes

**Goal.** MIDI notes on the user's channel play the voice; one controller moves its DECAY.

**Touch.** `app/src/app.cpp`.

```cpp
// setup(): the route; notes then reach the unit in the render, at their frame, one block after they came in
voice_id_ = e.attach(voice_);
(void)ctx.midi().route(voice_id_, 0);          // 0: the channel the user chose in Config, MIDI (omni while that is 0); at most 4 routes

// every channel message no route took: main loop, in order, before update()
bool midi(app::Context& ctx, const core::MidiMessage& m) noexcept override {
  if (core::midi_kind(m.status) == core::kMidiControl && m.data1 == 74) {
    (void)ctx.engine().set_param(voice_id_, Voice::Decay, 20.0f + static_cast<float>(m.data2) * (3980.0f / 127.0f));
    return true;
  }
  return false;
}
```

Or let the user bind any controller to any parameter: `ctx.midi().learn(voice_id_, Voice::Decay)` arms MIDI learn, and the kit keeps the
binding on the card. The clock needs nothing: with FOLLOW CLOCK on in Config, every clocked hook follows the master.

**Proof.** A scenario: the simulator gives the app a fake MIDI device when a `midi` line is in it. `record on`, `midi din 90 3c 64`,
`wait 300`, `midi din 80 3c 00`, `midi usb b0 4a 7f`, `wait 200`, `wav notes`: the WAV has the note, and `expect state` can check an app pair.
HELLO's `apps/hello/scenarios/midi.txt` is the long version; `tests/app_midi_test.cpp` and `tests/sim_midi_test.cpp` test the kit.

**Read.** `FRAMEWORK.md` [MIDI](FRAMEWORK.md#midi-midih), `kit/app/include/o3sdk/app/midi.h`.

## 10. Record a take

**Goal.** REC records the master from the next bar, stops at a bar, and saves the take as a WAV.

**Touch.** `app/src/app.cpp` (or the screen that has the key).

```cpp
struct alignas(4) TakeMem { std::int16_t pcm[44100 * 10]; };   // ten seconds, mono
O3SDK_BIG TakeMem g_take;                                        // the SDRAM (core/placement.h); zero-initialised only: the kit allocates none

hal::record::Take t;
t.buf = g_take.pcm;
t.capacity_frames = 44100 * 10;
t.align = hal::record::Align::Bar;                               // Free, Step, Bar, Pattern or Level (threshold + preroll_frames)
                                                                 // a Step, Bar or Pattern take waits while the transport is stopped:
                                                                 // the template never calls play(), so start it first or use Free
ctx.recorder().set_tap(hal::record::Tap::Master);                // or Input: what the codec delivers
(void)ctx.recorder().arm(t);                                     // returns at once: Waiting, Recording, ... Done
// REC again:
(void)ctx.recorder().stop_at(hal::record::Align::Bar);           // ends at the sample of the next bar (disarm(): at the next block)
// once state() is Done and the card is free:
if (ctx.recorder().state() == hal::record::State::Done && !ctx.files().busy()) (void)ctx.recorder().save(ctx.files(), "", "take");
// once files().status() is Done: ctx.recorder().release(), and the buffer may be armed again
```

While a take runs, `App::update_blocker()` can return `"RECORDING"` so that Config, FIRMWARE refuses an update. A full buffer ends the take
(`overrun()` counts what did not fit, nothing is overwritten). An unarmed recorder costs the render one load and a branch a block.

**Proof.** HELLO's `apps/hello/scenarios/record.txt`, run with `--card`: `expect state rec=recording`, `expect state rec=done`,
`expect file Apps/hello/take_001.wav`; `tests/sim_record_test.cpp` reads the WAVs back and checks them to the sample. The cost while recording
is a unit check (recipe 12).

**Read.** `FRAMEWORK.md` [Recording](FRAMEWORK.md#recording-recorderh-halrecordh), `kit/app/include/o3sdk/app/recorder.h`.

## 11. Write a scenario and pin its digests

**Goal.** A scripted run that shows a screen as pictures and the sound as a WAV, and a ctest that fails when a frame changes.

**Touch.** `app/scenarios/<name>.txt` (new), `tests/CMakeLists.txt`, `tests/digests/myapp.txt` (generated).

```text
# app/scenarios/song.txt: SONG, its HELP, and back (commands: o3-sdk sim/README.md)
wait 100
snap 00_play
tap song
snap 01_song
press shift
tap config
release shift
snap 02_help
tap f8
tap note
record on
pad 36 200
wait 300
wav notes
```

```sh
mkdir -p /tmp/song && build-host/app/myapp_sim --scenario app/scenarios/song.txt --out /tmp/song
# the PNGs (800 x 697: the screen and the 48 pads under it), notes.wav, digests.txt, and a summary with `peak list n/2048`
```

The template's `app_test.cpp` already runs every scenario on every ctest. To pin the frames as well (the FNV-1a of each frame's display-list
words, not its pixels), register the kit's checker and write the pin file once:

```cmake
add_test(NAME digest_myapp COMMAND ${O3SDK_PYTHON} -I ${O3SDK_DIR}/tools/digest_check.py
  --sim $<TARGET_FILE:myapp_sim> --pins ${PROJECT_SOURCE_DIR}/tests/digests/myapp.txt
  "--run=boot|${PROJECT_SOURCE_DIR}/app/scenarios/boot.txt"
  "--run=song|${PROJECT_SOURCE_DIR}/app/scenarios/song.txt")
```

```sh
mkdir -p tests/digests
python3 -I ../o3-sdk/tools/digest_check.py --sim build-host/app/myapp_sim --pins tests/digests/myapp.txt \
  --run 'boot|app/scenarios/boot.txt' --run 'song|app/scenarios/song.txt' --update   # (re)writes the pins: review them with the change
```

`{card}` in a run's arguments is a fresh card directory (`'files|app/scenarios/files.txt|--card {card}'`). A `--run` fails when the app used
bitmap handle 0; an app with its own bitmaps (recipe 7) runs its scenarios itself and passes the simulator's files with
`--have <out>/digests.txt`.

**Proof.** `ctest --test-dir build-host -R digest_myapp`; change a colour on purpose and watch it fail.

**Read.** `sim/README.md` ("Scenarios", and "Frame digests, line load, handle 0"), `NEW-APP.md`
[section 8](NEW-APP.md#8-a-scenario-and-tests), `tools/digest_check.py --help`.

## 12. Measure the CPU on the unit

**Goal.** The real cost of a unit or a feature, as a percentage of the 261,224 cycles a 64-frame block has at 44.1 kHz.

**Touch.** Nothing in the code: a gate-checked image of the app, installed only through the documented path (`NEW-APP.md`
[section 10](NEW-APP.md#10-the-gate) and [section 11](NEW-APP.md#11-install-over-usb-and-the-way-back)), and a fixed thing to play.

```sh
python3 -I $O3/o3console.py "cpu reset"                 # the maxima and the fault counters back to zero
python3 -I $O3/o3console.py "key play" "pad 36 down"   # the same fixed input every time (or by hand)
python3 -I $O3/o3console.py cpu state "pad 36 up" "key play"
# cpu: the render's cycles per stage, the underruns and the jitter;  state: cpu=<percent> among the kit's pairs
```

Measure with the feature off, then on, with the same input: the difference is its cost. The header strip shows the render's load on the unit,
and a screen that draws its own header reads `Context::cpu_meter()` and `Context::cpu_percent()`; `Engine::stats()` has `cycles` and
`cycles_max` (0 on the host: the simulator has no cycle counter, and its timing is not the unit's). Until it is measured, a cost in a header
is written as an estimate, `[est]`.

**Proof.** The numbers, dated, in the app's docs. Known: HELLO renders 19.6 % of a block at idle and o3-303 at most 39.8 % playing
(`docs/hw/2026-10-25-usb.md`). A host test is never this measurement.

**Read.** `FRAMEWORK.md` [Rules](FRAMEWORK.md#8-rules) and [Capabilities](FRAMEWORK.md#10-capabilities-the-whole-hardware-for-every-app)
(CPU and memory); the skills `rt-optimize` (what to try, in which order) and `hw-session` (a batched unit session) in `.claude/skills/`.
