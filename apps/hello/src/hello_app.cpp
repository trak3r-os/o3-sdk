// SPDX-License-Identifier: GPL-3.0-or-later
//
// HELLO: an example whole app on the kit (docs/FRAMEWORK.md, "Apps"). A new app starts from templates/app (tools/new_app.py, docs/NEW-APP.md).
//
//   pads          a chromatic keyboard, one octave a row (bottom row C3..B3); LEFT / RIGHT move it an octave
//   F1..F8        point the encoder at a value (tap: latched; hold + turn: for a moment; two held: both move)
//   encoder       change it (SHIFT: coarse steps)
//   PARAMS        next page of values (SYNTH, DELAY); SHIFT + PARAMS: the previous one
//   PLAY          the demo riff on and off (a clocked hook of the engine plays it, sample-accurate)
//   UP / DOWN     tempo +-1 BPM (SHIFT: +-10)
//   CONFIG        the system Config screen (the kit's, not the app's): FIRMWARE, AUDIO (the input's source, gain and monitor; the radio), MIDI, CARD, DISPLAY (the theme, BRIGHTNESS and PAD LIGHTS),
//                 INFO, and HELLO's own page after them: TEMPO and RIFF SHIFT (values the kit keeps in the settings file) and DEFAULT RIFF (an action: F1 asks, F1 again runs it)
//   SHIFT+CONFIG  HELP, the kit's overlay: what each button and each pad does on this screen (PlayScreen::help() gives the texts; F1 BUTTONS, F2 PADS, F8 or CONFIG closes)
//   PATTERN       a list of the riffs on the card (Apps/hello/*.o3a, app::FilePicker): the encoder or UP / DOWN choose, F1 loads it, F8 leaves
//   MASTER        MIDI LEARN (Context::midi().learn()): the next control change moves the value the encoder points at, from then on and after a restart (the kit keeps the binding); MASTER
//                 again, before one arrives, leaves learn. The LEARN line on the screen says what it waits for or which controller moves the value.
//   COPY          SAVE AS: the on-screen keyboard (app::NameEntry) asks for a name and saves the riff as Apps/hello/<name>.o3a (F1 types, DELETE erases, F7 OK, F8 cancels)
//   MIDI          notes on the channel the user chose in the Config's MIDI section (any, by default) play SYNTH, frame-accurate; with FOLLOW on
//                 there, the riff and the tempo follow a MIDI clock on DIN or USB (the riff is a clocked hook; it needs nothing to do so)
//   INSERT        the riff up a semitone (SHIFT: down), so that there is something to save; DELETE puts the default riff back
//   FILE          SAVE the riff to the card, Apps/hello/riff.o3a (Context::files(): a job of the main loop, never a pause of the sound or the screen)
//   SAMPLE LOAD   LOAD it back (a riff that is not there says so on the strip). The card, the file and the last job show on the screen.
//   REC           records a take (Context::recorder(): into a buffer of the app's own, ten seconds by default) and stops it; REC again after a take discards it and starts the next.
//                 Where the take starts is START (SAMPLE REC cycles FREE, STEP, BAR, LEVEL: on the transport's sixteenth or bar, to the sample, or when the input reaches -30 dB, with
//                 50 ms before it), how long it can be is LEN (SAMPLE PLAY: 1, 10, 30 s) and what it records is TAP (SAMPLE EDIT: the audio INPUT, or the MASTER).
//   SHIFT + FILE  saves the take to the card as a WAV, Apps/hello/take_NNN.wav.
//   SHIFT + REC   the audio in through the master: the monitor on (the usual 70 %) and off. Where the input comes from (line, microphone, radio) is the Config's AUDIO
//                 section; the screen's IN meter shows what arrives. With no capture device (a platform without audio in) neither exists.
//
// One instrument (SYNTH) on slot 0, a DELAY on send A.

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/file_picker.h>
#include <o3sdk/app/name_entry.h>
#include <o3sdk/app/param_keys.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/app/widgets.h>
#include <o3sdk/core/placement.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/formats/chunk_file.h>
#include <o3sdk/hal/record.h>
#include <o3sdk/ui/components.h>
#include <o3sdk/units/delay/delay.h>
#include <o3sdk/units/synth/synth.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using namespace o3sdk;
using app::Button;
using ui::Color;
using ui::Face;
using ui::Rect;

// ---- The demo riff: a clocked hook ------------------------------------------------------------------------------------------

constexpr unsigned kRiffSteps = 16;
constexpr std::uint8_t kDefaultRiff[kRiffSteps] = {36, 36, 48, 36, 0, 39, 36, 46, 36, 0, 43, 36, 48, 0, 41, 43};  // 0: a rest
constexpr std::uint16_t kRiffAccents = (1u << 0) | (1u << 6) | (1u << 12);

class Riff final : public dsp::Clocked {
 public:
  Riff() noexcept { reset(); }
  void set_unit(dsp::UnitId unit) noexcept { unit_ = unit; }
  int step() const noexcept { return step_.load(std::memory_order_relaxed); }
  int note() const noexcept { return note_.load(std::memory_order_relaxed); }
  // The note of step `s` (0: a rest). The render reads the notes while the main loop changes them (transpose, load): each is one atomic byte, so a block sees a step as it was or as it is now.
  std::uint8_t note_at(unsigned s) const noexcept { return notes_[s % kRiffSteps].load(std::memory_order_relaxed); }

  // The riff up or down by semitones (rests stay rests; a note stays inside 1..127). Main loop.
  void transpose(int semitones) noexcept {
    for (unsigned i = 0; i < kRiffSteps; ++i) {
      const int n = notes_[i].load(std::memory_order_relaxed);
      if (n != 0) {
        const int t = n + semitones;
        notes_[i].store(static_cast<std::uint8_t>(t < 1 ? 1 : (t > 127 ? 127 : t)), std::memory_order_relaxed);
      }
    }
  }
  // The default riff again. Main loop.
  void reset() noexcept {
    for (unsigned i = 0; i < kRiffSteps; ++i) {
      notes_[i].store(kDefaultRiff[i], std::memory_order_relaxed);
    }
  }
  // How far the riff is from the default, as a transposition: the first note against the default's (the console's `state`).
  int shift() const noexcept { return static_cast<int>(note_at(0)) - static_cast<int>(kDefaultRiff[0]); }

  // The riff as a file (formats/chunk_file.h, type "HRIF"): one chunk RIFF of u16 version (0x0100), u8 steps, the notes and u16 accents. The size, or 0 when `cap` is too small.
  std::size_t write(std::uint8_t* out, std::size_t cap) const noexcept {
    std::uint8_t notes[kRiffSteps];
    for (unsigned i = 0; i < kRiffSteps; ++i) {
      notes[i] = note_at(i);
    }
    formats::chunk::Writer w(out, cap, "HRIF");
    if (!w.begin("RIFF") || !w.u16(0x0100) || !w.u8(kRiffSteps) || !w.bytes(notes, sizeof notes) || !w.u16(kRiffAccents) || !w.end()) {
      return 0;
    }
    return w.finish();
  }
  // A riff from such a file: false (nothing changed) when it is damaged, of a newer major version or not 16 steps. A newer minor is read as far as this version knows it.
  bool read(const std::uint8_t* data, std::size_t n) noexcept {
    formats::chunk::Reader r;
    formats::chunk::Chunk c{};
    if (r.open(data, n, "HRIF") != formats::chunk::Error::None) {
      return false;
    }
    while (r.next(c)) {
      if (!formats::chunk::id_is(c, "RIFF")) {
        continue;  // (a chunk of a newer version of the file)
      }
      formats::chunk::Fields f(c);
      const std::uint16_t version = f.u16();
      const unsigned steps = f.u8();
      std::uint8_t notes[kRiffSteps] = {};
      if (steps != kRiffSteps || (version >> 8) != 1u) {
        return false;
      }
      for (std::uint8_t& note : notes) {
        note = f.u8();
        if (note > 127u) {
          return false;
        }
      }
      if (!f.ok()) {
        return false;
      }
      for (unsigned i = 0; i < kRiffSteps; ++i) {
        notes_[i].store(notes[i], std::memory_order_relaxed);
      }
      return true;
    }
    return false;
  }

  // Render context.
  void block(dsp::BlockContext& ctx) noexcept override {
    const dsp::Transport& t = ctx.transport();
    // A stop, or a restart (Play while playing), ends the note this hook started: nobody else will.
    if ((t.stopped || t.started) && pending_off_ >= 0) {
      (void)ctx.emit(unit_, dsp::note_off(static_cast<std::uint8_t>(pending_off_)));
      pending_off_ = -1;
    }
    if (t.stopped) {
      step_.store(-1, std::memory_order_relaxed);
      note_.store(-1, std::memory_order_relaxed);
      return;
    }
    // A 50 % gate: each note ends half a sixteenth after it starts.
    dsp::for_each_step(t, dsp::kTicksPerSixteenth, dsp::kTicksPerSixteenth / 2u, 0u, [&](std::uint32_t frame, std::uint64_t k) {
      const std::uint8_t n = note_at(static_cast<unsigned>(k % kRiffSteps));
      if (n != 0u) {
        (void)ctx.emit(unit_, dsp::note_off(n, static_cast<std::uint16_t>(frame)));
        pending_off_ = -1;
        note_.store(-1, std::memory_order_relaxed);
      }
    });
    dsp::for_each_step(t, dsp::kTicksPerSixteenth, 0u, 0u, [&](std::uint32_t frame, std::uint64_t k) {
      const auto s = static_cast<unsigned>(k % kRiffSteps);
      const std::uint8_t n = note_at(s);
      if (n != 0u) {
        const std::uint8_t vel = (kRiffAccents >> s & 1u) != 0u ? 127u : 96u;
        (void)ctx.emit(unit_, dsp::note_on(n, vel, 0u, static_cast<std::uint16_t>(frame)));
        pending_off_ = n;
        note_.store(n, std::memory_order_relaxed);
      }
      step_.store(static_cast<int>(s), std::memory_order_relaxed);
    });
  }

 private:
  dsp::UnitId unit_ = dsp::kNoUnit;
  int pending_off_ = -1;
  std::atomic<int> step_{-1};
  std::atomic<int> note_{-1};
  std::atomic<std::uint8_t> notes_[kRiffSteps];  // written by the main loop (transpose, load), read by the render: one byte each
};

// ---- The recorder: what the REC key arms ------------------------------------------------------------------------------------

constexpr std::uint32_t kRate = 44'100;
constexpr unsigned kTakeLens = 3;
constexpr std::uint32_t kTakeSeconds[kTakeLens] = {1, 10, 30};
constexpr std::uint32_t kTakeCapacity = 30 * kRate;  // the longest take: half a minute, mono, 2.6 MB of the SDRAM
constexpr std::int32_t kLevelThreshold = 1036;       // -30 dBFS
constexpr std::uint32_t kPreRoll = kRate / 20;       // 50 ms
// The take's buffer: the app's own, in the SDRAM (the kit allocates none). 4-byte aligned for the recorder's word writes.
struct alignas(4) TakeMem {
  std::int16_t pcm[kTakeCapacity];
};
static_assert(alignof(TakeMem) >= 4, "the take's buffer is word aligned");
O3SDK_BIG TakeMem g_take;
static_assert(hal::record::ring_frames(kPreRoll) < kTakeSeconds[0] * kRate, "the shortest take holds a level take's ring");

enum class Start : std::uint8_t { Free, Step, Bar, Level };
constexpr unsigned kStarts = 4;
constexpr const char* kStartNames[kStarts] = {"FREE", "STEP", "BAR", "LEVEL"};

struct RecUi {
  Start start = Start::Free;
  std::uint8_t len = 1;  // an index of kTakeSeconds
  hal::record::Tap tap = hal::record::Tap::Input;
  const char* note = nullptr;  // a refusal of the last key
};

const char* rec_state_name(hal::record::State s) noexcept {
  switch (s) {
    case hal::record::State::Idle: return "IDLE";
    case hal::record::State::Waiting: return "WAITING";
    case hal::record::State::Listening: return "LISTENING";
    case hal::record::State::Recording: return "RECORDING";
    case hal::record::State::Done: return "DONE";
  }
  return "?";
}

// Appends `v` in decimal; returns the new end.
char* put_uint(char* p, std::uint32_t v) noexcept {
  char digits[10];
  unsigned n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < sizeof digits);
  while (n != 0u) {
    *p++ = digits[--n];
  }
  return p;
}
char* put_str(char* p, const char* s) noexcept {
  while (*s != '\0') {
    *p++ = *s++;
  }
  return p;
}
// Frames as seconds with one decimal ("3.2").
char* put_seconds(char* p, std::uint32_t frames) noexcept {
  const std::uint32_t tenths = frames / (kRate / 10u);
  p = put_uint(p, tenths / 10u);
  *p++ = '.';
  *p++ = static_cast<char>('0' + tenths % 10u);
  return p;
}

// ---- The screen -------------------------------------------------------------------------------------------------------------

constexpr bool is_natural(unsigned note) noexcept {
  const unsigned pc = note % 12u;
  return pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11;
}

const char* card_name(app::Files::Card c) noexcept {
  switch (c) {
    case app::Files::Card::None: return "NO CARD";
    case app::Files::Card::Ready: return "READY";
    case app::Files::Card::ReadOnly: return "WRITE PROTECTED";
    case app::Files::Card::Unsupported: return "NOT SUPPORTED";
    case app::Files::Card::OnComputer: return "ON COMPUTER";
    case app::Files::Card::InUse: return "UPDATE RUNNING";
  }
  return "?";
}

class PlayScreen final : public app::Screen {
 public:
  void init(dsp::UnitId synth, dsp::UnitId delay, const Riff* riff, const RecUi* rec) noexcept {
    synth_ = synth;
    riff_ = riff;
    rec_ = rec;
    pages_[0] = app::ParamPage{"SYNTH", synth, {0, 1, 2, 3, 4, 5, 6, 7}, 8};
    pages_[1] = app::ParamPage{"DELAY", delay, {0, 1, 2, 3, 0, 0, 0, 0}, 4};
    keys_.set_pages(pages_, 2);
    keys_.set_page(0);
    for (auto& n : held_) {
      n = -1;
    }
  }

  const char* title() const noexcept override { return "PLAY"; }

  // The file picker and the name entry the app opens over this screen (drawn last, with their own soft keys).
  void set_overlays(app::FilePicker* picker, app::NameEntry* names) noexcept {
    picker_ = picker;
    names_ = names;
  }

  // For the console's `state` (HelloApp::describe_state).
  int octave() const noexcept { return octave_; }
  // A line of the app's own about the last file job, shown in place of the files service's text ("BAD FILE": the load worked and the file was not a riff). nullptr: none.
  void set_note(const char* note) noexcept { note_ = note; }
  const app::ParamKeys& keys() const noexcept { return keys_; }

  void input(app::Context& ctx, const app::Input& in) noexcept override {
    if (keys_.input(ctx, in)) {
      return;
    }
    if (in.press(Button::Params)) {
      if (in.shift) {
        keys_.prev_page();
      } else {
        keys_.next_page();
      }
      ctx.redraw();
    } else if (in.press(Button::Left) || in.press(Button::Right)) {
      const int d = in.press(Button::Left) ? -1 : 1;
      octave_ = octave_ + d < -3 ? -3 : (octave_ + d > 2 ? 2 : octave_ + d);
      ctx.redraw();
    } else if (in.pad_press()) {
      const int n = note_for(in.pad());
      if (n >= 0) {
        held_[in.pad()] = n;
        (void)ctx.engine().note_on(synth_, static_cast<std::uint8_t>(n), 110);
      }
    } else if (in.pad_release()) {
      if (held_[in.pad()] >= 0) {
        (void)ctx.engine().note_off(synth_, static_cast<std::uint8_t>(held_[in.pad()]));
        held_[in.pad()] = -1;
      }
    }
  }

  void draw(app::Context& ctx, ui::Renderer& r, const Rect& area) noexcept override {
    // The keyboard map: the pads as they are laid out, note names on the C keys.
    const int cell_w = 30;
    const int cell_h = 26;
    const int map_x = 16;
    const int map_y = area.y + 34;
    r.text(map_x, area.y + 12, "PADS", Color::TextDim, Face::Label);
    char oct[12] = "OCT +0";
    oct[4] = octave_ < 0 ? '-' : '+';
    oct[5] = static_cast<char>('0' + (octave_ < 0 ? -octave_ : octave_));
    r.text(map_x + 12 * cell_w - r.text_width(oct, Face::Label), area.y + 12, oct, Color::Text, Face::Label);
    const int riff_note = riff_ != nullptr ? riff_->note() : -1;
    for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
      const Rect c{map_x + static_cast<int>(app::pad_col(pad)) * cell_w, map_y + static_cast<int>(app::pad_row(pad)) * cell_h,
                   cell_w - 3, cell_h - 3};
      const int n = note_for(pad);
      const bool held = ctx.panel().pad_held(pad);
      if (held) {
        r.panel(c, Color::Accent);
      } else if (n >= 0 && n == riff_note) {
        r.panel(c, Color::Play);
      } else if (n >= 0 && is_natural(static_cast<unsigned>(n))) {
        r.panel(c, Color::Empty);
      } else {
        r.frame(c, Color::Empty);
      }
      if (n >= 0 && n % 12 == 0) {
        char name[4] = {'C', static_cast<char>('0' + (n / 12 - 1) % 10), '\0', '\0'};
        r.text(c.x + 4, c.y + 6, name, held ? Color::AccentText : Color::Text, Face::Micro);
      }
    }

    // The riff and the output.
    const int rx = 420;
    const int rw = ui::kScreenW - rx - 16;
    r.text(rx, area.y + 12, "RIFF", Color::TextDim, Face::Label);
    r.text(rx + 48, area.y + 12, ctx.engine().playing() ? "PLAYING" : "PRESS PLAY", ctx.engine().playing() ? Color::Play : Color::Text,
           Face::Label);
    std::uint32_t on = 0;
    for (unsigned s = 0; s < kRiffSteps; ++s) {
      on |= riff_ != nullptr ? (riff_->note_at(s) != 0u ? 1u << s : 0u) : (kDefaultRiff[s] != 0u ? 1u << s : 0u);
    }
    app::step_row(r, Rect{rx, area.y + 34, rw, 22}, kRiffSteps, on, riff_ != nullptr ? riff_->step() : -1, -1);
    r.text(rx, area.y + 70, "OUT", Color::TextDim, Face::Label);
    const dsp::EngineStats s = ctx.engine().stats();
    app::level_meter(r, Rect{rx + 40, area.y + 72, rw - 40, 10}, s.peak_l > s.peak_r ? s.peak_l : s.peak_r);
    // The audio in (Context::audio_in()): its peak, while the monitor plays it ("MON"). Asking for the peaks is what makes the engine measure them, so they are asked for only while
    // there is something to see: a source was chosen or the monitor is on (the Config's AUDIO section meters on its own). Nothing of this exists without a capture device.
    if (ctx.audio_in().available()) {
      const bool watching = ctx.audio_in().selected() || ctx.audio_in().monitor() != 0u;
      const dsp::InputPeaks in_peak = watching ? ctx.audio_in().peaks() : dsp::InputPeaks{};
      r.text(rx, area.y + 84, ctx.audio_in().monitor() != 0u ? "MON" : "IN", ctx.audio_in().monitor() != 0u ? Color::Play : Color::TextDim, Face::Label);
      app::level_meter(r, Rect{rx + 40, area.y + 86, rw - 40, 10}, in_peak.l > in_peak.r ? in_peak.l : in_peak.r);
    }

    // What the keys do.
    const int hy = map_y + 4 * cell_h + 18;
    r.text(16, hy, "PADS play   F1-F8 pick a value   ENCODER turn it (SHIFT coarse)   hold F + turn: for a moment", Color::TextDim,
           Face::Label);
    r.text(16, hy + 20, "PARAMS page   PLAY riff   UP/DOWN tempo   LEFT/RIGHT octave   CONFIG system", Color::TextDim, Face::Label);
    r.text(16, hy + 40, "INSERT riff +1 (SHIFT -1)   DELETE default riff   FILE save the riff   SAMPLE LOAD load it", Color::TextDim, Face::Label);
    r.text(16, hy + 60, "PATTERN pick a riff   COPY save it as...   MASTER learn the CC that moves the value (then turn a knob)", Color::TextDim, Face::Label);
    draw_rec(ctx, r, 16, hy + 90);
    // The card and the last job of the files service (Context::files()): polled, never waited for.
    const app::Files& files = ctx.files();
    const app::Files::Status fs = files.status();
    r.text(rx, area.y + 100, "CARD", Color::TextDim, Face::Label);
    r.text(rx + 48, area.y + 100, !files.available() ? "NONE" : card_name(files.card()), files.card() == app::Files::Card::Ready ? Color::Play : Color::Text, Face::Label);
    r.text(rx, area.y + 120, "FILE", Color::TextDim, Face::Label);
    r.text(rx + 48, area.y + 120, note_ != nullptr ? note_ : (fs.phase == app::Files::Status::Idle ? "-" : fs.text),
           note_ != nullptr || fs.phase == app::Files::Status::Failed ? Color::Accent : Color::Text, Face::Label);

    {
      // MIDI LEARN (MASTER): what it waits for, or the controller that moves the value the encoder points at.
      const app::ParamPage* page = keys_.page();
      std::uint8_t cc = 0;
      std::uint8_t ch = 0;
      char line[40];
      char* p = line;
      if (ctx.midi().learning()) {
        p = put_str(line, "MOVE A KNOB");
      } else if (page != nullptr && keys_.focus() < page->count && ctx.midi().bound(page->unit, page->ids[keys_.focus()], cc, ch)) {
        p = put_str(line, "CC ");
        p = put_uint(p, cc);
      } else {
        p = put_str(line, "MASTER binds");
      }
      *p = '\0';
      r.text(rx, area.y + 140, "LEARN", Color::TextDim, Face::Label);
      r.text(rx + 48, area.y + 140, line, ctx.midi().learning() ? Color::Accent : Color::Text, Face::Label);
    }
    app::knob_strip(r, Rect{0, area.y + area.h - app::kKnobStripH, ui::kScreenW, app::kKnobStripH}, ctx.engine(), keys_);
    if (picker_ != nullptr && picker_->active()) {
      r.panel(area, Color::Pane);
      picker_->draw(r, area);
    }
    if (names_ != nullptr && names_->active()) {
      names_->draw(r, area);
    }
  }

  void soft_keys(app::Context& ctx, app::SoftKeys& out) noexcept override {
    if (picker_ != nullptr && picker_->active()) {
      picker_->soft_keys(out);
      return;
    }
    if (names_ != nullptr && names_->active()) {
      names_->soft_keys(out);
      return;
    }
    const app::ParamPage* p = keys_.page();
    if (p == nullptr) {
      return;
    }
    for (unsigned k = 0; k < p->count; ++k) {
      const core::ParamDesc* d = ctx.engine().param_desc(p->unit, p->ids[k]);
      out.label[k] = d != nullptr ? d->name : "";
    }
    out.hot = static_cast<std::uint8_t>(1u << keys_.focus());
  }

  // HELP (Shift + Config, help.h): the texts are static tables; the pads' labels are this screen's own (the note a pad plays moves with the octave), filled when HELP opens.
  void help(const app::Context&, app::Help& h) const noexcept override {
    static constexpr app::HelpButton kButtons[] = {
        {Button::Params, "NEXT PAGE: SYNTH, DELAY", "PREVIOUS PAGE"},
        {Button::File, "SAVE THE RIFF TO THE CARD", "SAVE THE TAKE AS A WAV"},
        {Button::Master, "MIDI LEARN THE VALUE", nullptr},
        {Button::SampleLoad, "LOAD THE RIFF FROM THE CARD", nullptr},
        {Button::SamplePlay, "TAKE LENGTH: 1, 10, 30 S", nullptr},
        {Button::SampleEdit, "TAKE SOURCE: INPUT, MASTER", nullptr},
        {Button::SampleRec, "HOW A TAKE STARTS", nullptr},
        {Button::Pattern, "PICK A RIFF FROM THE CARD", nullptr},
        {Button::Play, "START / STOP THE DEMO RIFF", nullptr},
        {Button::Rec, "RECORD A TAKE", "AUDIO IN THROUGH MASTER"},
        {Button::Copy, "SAVE THE RIFF UNDER A NAME", nullptr},
        {Button::Insert, "RIFF UP A SEMITONE", "RIFF DOWN A SEMITONE"},
        {Button::Delete, "THE DEFAULT RIFF AGAIN", nullptr},
        {Button::Up, "TEMPO UP 1 BPM", "TEMPO UP 10 BPM"},
        {Button::Down, "TEMPO DOWN 1 BPM", "TEMPO DOWN 10 BPM"},
        {Button::Left, "KEYBOARD AN OCTAVE DOWN", nullptr},
        {Button::Right, "KEYBOARD AN OCTAVE UP", nullptr},
    };
    static constexpr app::HelpPadGroup kGroups[] = {
        {"NATURALS", "THE WHITE KEYS OF THE SCALE", Color::Sel},
        {"SHARPS", "THE SEMITONES BETWEEN THEM", Color::Instr},
    };
    h.summary = "A SYNTH AND A DELAY. PLAY THE PADS, POINT THE ENCODER AT A VALUE WITH F1-F8 (A TAP LATCHES IT) AND TURN IT, START THE DEMO RIFF WITH PLAY.";
    h.buttons = kButtons;
    h.button_count = sizeof kButtons / sizeof kButtons[0];
    h.encoder = "CHANGE VALUE (SHIFT COARSE)";
    h.groups = kGroups;
    h.group_count = sizeof kGroups / sizeof kGroups[0];
    h.pads = "ONE OCTAVE A ROW, THE BOTTOM ROW LOWEST. LEFT / RIGHT MOVE THE WHOLE KEYBOARD.";
    app::help_pads(h, 0, 0, app::kPadRows - 1, app::kPadCols - 1, 1);
    for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
      const int n = note_for(pad);
      if (n < 0) {
        h.pad_group[pad] = 0;
        continue;
      }
      if (!is_natural(static_cast<unsigned>(n))) {
        h.pad_group[pad] = 2;
      }
      static const char kNames[] = "C C#D D#E F F#G G#A A#B ";
      char* s = pad_names_[pad];
      const unsigned pc = static_cast<unsigned>(n) % 12u;
      *s++ = kNames[pc * 2u];
      if (kNames[pc * 2u + 1u] != ' ') {
        *s++ = kNames[pc * 2u + 1u];
      }
      s = put_uint(s, static_cast<std::uint32_t>(n / 12 - 1 < 0 ? 0 : n / 12 - 1));
      *s = '\0';
      h.pad_label[pad] = pad_names_[pad];
    }
  }

  void leds(app::Context& ctx, app::PadLeds& leds) noexcept override {
    const int riff_note = riff_ != nullptr ? riff_->note() : -1;
    for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
      const int n = note_for(pad);
      if (n < 0) {
        continue;
      }
      std::uint8_t level = is_natural(static_cast<unsigned>(n)) ? app::kLedDim : app::kLedOff;
      if (n == riff_note) {
        level = app::kLedMid;
      }
      if (ctx.panel().pad_held(pad)) {
        level = app::kLedOn;
      }
      leds.set(pad, level);
    }
  }

 private:
  // The recorder (Context::recorder()): its state and the take's length so far, what REC will arm next, and what the keys are. Nothing here exists without a recorder.
  void draw_rec(app::Context& ctx, ui::Renderer& r, int x, int y) const noexcept {
    const app::Recorder& rec = ctx.recorder();
    if (!rec.available() || rec_ == nullptr) {
      return;
    }
    const hal::record::State st = rec.state();
    char line[96];
    char* p = put_str(line, rec_state_name(st));
    if (st == hal::record::State::Recording || st == hal::record::State::Done) {
      *p++ = ' ';
      p = put_seconds(p, rec.frames());
      p = put_str(p, " S");
    }
    if (rec.overrun() != 0u) {
      p = put_str(p, "  OVERRUN ");
      p = put_uint(p, rec.overrun());
    }
    *p = '\0';
    r.text(x, y, "REC", Color::TextDim, Face::Label);
    r.text(x + 48, y, line, st == hal::record::State::Recording ? Color::Accent : Color::Text, Face::Label);
    p = put_str(line, "START ");
    p = put_str(p, kStartNames[static_cast<unsigned>(rec_->start)]);
    p = put_str(p, "   LEN ");
    p = put_uint(p, kTakeSeconds[rec_->len]);
    p = put_str(p, " S   TAP ");
    p = put_str(p, rec_->tap == hal::record::Tap::Master ? "MASTER" : "INPUT");
    *p = '\0';
    r.text(x + 280, y, line, Color::Text, Face::Label);
    r.text(x, y + 20, rec_->note != nullptr ? rec_->note : "REC take/stop   SAMPLE REC start   SAMPLE PLAY length   SAMPLE EDIT tap   SHIFT+FILE save", rec_->note != nullptr ? Color::Accent : Color::TextDim, Face::Label);
  }

  // Bottom row C3..B3 (+ the octave shift), one octave higher each row up.
  int note_for(unsigned pad) const noexcept {
    const int n = 48 + 12 * octave_ + 12 * (static_cast<int>(app::kPadRows) - 1 - static_cast<int>(app::pad_row(pad))) +
                  static_cast<int>(app::pad_col(pad));
    return n >= 0 && n <= 127 ? n : -1;
  }

  dsp::UnitId synth_ = dsp::kNoUnit;
  const Riff* riff_ = nullptr;
  const RecUi* rec_ = nullptr;
  app::ParamPage pages_[2] = {};
  app::ParamKeys keys_;
  int octave_ = 0;
  const char* note_ = nullptr;
  app::FilePicker* picker_ = nullptr;
  app::NameEntry* names_ = nullptr;
  int held_[app::kPadCount] = {};
  mutable char pad_names_[app::kPadCount][4] = {};  // help(): the pads' labels, "C3", "F#4"
};

// ---- The app ----------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kDelayCapacity = 88'200;  // 2 s at 44.1 kHz, each side
O3SDK_BIG float g_delay_l[kDelayCapacity];
O3SDK_BIG float g_delay_r[kDelayCapacity];

// HELLO's page of the system Config screen (app/config_page.h): two values the kit keeps for it and one action. A key is never reused (a settings file keeps meaning what it meant).
constexpr std::uint16_t kCfgTempo = 1;
constexpr std::uint16_t kCfgShift = 2;
constexpr std::uint16_t kCfgReset = 3;
constexpr core::ParamDesc kTempoDesc = {"TEMPO", "BPM", nullptr, nullptr, 40.0f, 240.0f, 120.0f, 200, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
constexpr core::ParamDesc kShiftDesc = {"RIFF SHIFT", "st", nullptr, nullptr, -12.0f, 12.0f, 0.0f, 24, 12, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
constexpr app::ConfigItem kCfgItems[] = {{kCfgTempo, "TEMPO", &kTempoDesc}, {kCfgShift, "RIFF SHIFT", &kShiftDesc}, {kCfgReset, "DEFAULT RIFF", nullptr}};

class HelloApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"HELLO", "0.1.0"};
    return kInfo;
  }

  void setup(app::Context& ctx) noexcept override {
    dsp::Engine& e = ctx.engine();
    const dsp::UnitId synth = e.attach(synth_);
    const dsp::UnitId delay = e.attach(delay_);
    (void)e.set_source(0, synth);
    (void)ctx.midi().route(synth, 0);  // MIDI notes play the synth, on the user's channel (the Config's MIDI section; omni by default)
    (void)e.set_send_effect(0, delay);
    (void)e.set_mix(0, dsp::MixField::SendA, 0.3f);
    riff_.set_unit(synth);
    (void)e.add_hook(riff_);
    play_.init(synth, delay, &riff_, &rec_);
    play_.set_overlays(&picker_, &names_);
    ctx.show(play_);
  }

  unsigned config_pages() const noexcept override { return 1; }
  bool config_page(unsigned index, app::ConfigPage& out) const noexcept override {
    out = app::ConfigPage{"HELLO", kCfgItems, sizeof kCfgItems / sizeof kCfgItems[0]};
    return index == 0;
  }
  float config_get(const app::Context& ctx, std::uint16_t key) const noexcept override {
    if (key == kCfgTempo) {
      return static_cast<float>(ctx.engine().own_tempo()) / 1000.0f;
    }
    return key == kCfgShift ? static_cast<float>(riff_.shift()) : 0.0f;
  }
  void config_set(app::Context& ctx, std::uint16_t key, float value) noexcept override {
    if (key == kCfgTempo) {
      (void)ctx.engine().set_tempo(static_cast<std::uint32_t>(value * 1000.0f + 0.5f));
    } else if (key == kCfgShift) {
      riff_.transpose(static_cast<int>(value) - riff_.shift());
    }
  }
  void config_run(app::Context& ctx, std::uint16_t key) noexcept override {
    if (key == kCfgReset) {
      riff_.reset();
      ctx.redraw();
    }
  }

  // The console's `state` after the kit's pairs (docs/FRAMEWORK.md section 5): the keyboard octave, the page of values and the one the
  // encoder drives now, with its value as the knob strip shows it. A key past the page's last (blank) has the value "-".
  std::size_t describe_state(const app::Context& ctx, char* out, std::size_t cap) const noexcept override {
    app::StateWriter w(out, cap);
    const app::ParamKeys& keys = play_.keys();
    const app::ParamPage* page = keys.page();
    const unsigned focus = keys.focus();
    char value[16] = {};
    if (page != nullptr && focus < page->count) {
      (void)ctx.engine().format_param(page->unit, page->ids[focus], value, sizeof value);
    }
    w.sint("octave", play_.octave());
    w.str("page", page != nullptr ? page->name : nullptr);
    w.num("focus", focus);
    w.str("value", value);
    w.sint("riff", riff_.shift());
    w.num("theme", static_cast<std::uint32_t>(ctx.theme()));
    w.num("midich", ctx.midi().config().channel);
    w.str("learn", ctx.midi().learning() ? "armed" : "idle");
    w.num("bound", ctx.midi().bindings());
    {
      std::uint8_t cc = 0;
      std::uint8_t ch = 0;
      const bool has = page != nullptr && focus < page->count && ctx.midi().bound(page->unit, page->ids[focus], cc, ch);
      w.sint("cc", has ? static_cast<int>(cc) : -1);  // the controller bound to the value the encoder points at
    }
    if (ctx.recorder().available()) {  // the recorder: its state, the frames of the take and those that did not fit, and what REC arms next
      const hal::record::State st = ctx.recorder().state();
      w.str("rec", st == hal::record::State::Idle ? "idle" : (st == hal::record::State::Waiting ? "waiting" : (st == hal::record::State::Listening ? "listening" : (st == hal::record::State::Recording ? "recording" : "done"))));
      w.num("take", ctx.recorder().frames());
      w.num("over", ctx.recorder().overrun());
      w.str("start", rec_.start == Start::Free ? "free" : (rec_.start == Start::Step ? "step" : (rec_.start == Start::Bar ? "bar" : "level")));
      w.num("len", kTakeSeconds[rec_.len]);
      w.str("tap", rec_.tap == hal::record::Tap::Master ? "master" : "input");
    }
    return w.size();
  }

  // The riff's file jobs end here: a load that is Done puts the riff in place (the file is checked first: a damaged or foreign one changes nothing and says so).
  void update(app::Context& ctx) noexcept override {
    picker_.update(ctx);
    if (op_ == Op::None) {
      return;
    }
    const app::Files::Status st = ctx.files().status();
    if (st.phase != app::Files::Status::Done && st.phase != app::Files::Status::Failed) {
      return;
    }
    if (op_ == Op::Loading && st.phase == app::Files::Status::Done) {
      play_.set_note(riff_.read(load_buf_, ctx.files().loaded()) ? nullptr : "BAD FILE");
    }
    if (op_ == Op::SavingTake && st.phase == app::Files::Status::Done) {
      ctx.recorder().release();  // the take is on the card: the buffer is free for the next one (a failed save keeps the take, to save again)
    }
    op_ = Op::None;
    ctx.redraw();
  }

  bool input(app::Context& ctx, const app::Input& in) noexcept override {
    dsp::Engine& e = ctx.engine();
    if (picker_.active() || names_.active()) {  // a list or the keyboard is up: it takes every control (a release goes by: nothing here waits for one)
      if (picker_.active() && picker_.input(ctx, in) == app::FilePicker::Result::Chosen && !ctx.files().busy()) {
        op_ = ctx.files().load("", picker_.chosen().name, load_buf_, sizeof load_buf_) ? Op::Loading : Op::None;
        play_.set_note(nullptr);
      } else if (names_.active() && names_.input(in) == app::NameEntry::Result::Done && !ctx.files().busy()) {
        char file[40];
        std::size_t n = 0;
        for (const char* c = names_.text(); *c != '\0' && n + 5u < sizeof file; ++c) {
          file[n++] = *c;
        }
        std::memcpy(file + n, ".o3a", 5);
        const std::size_t size = riff_.write(save_buf_, sizeof save_buf_);
        play_.set_note(nullptr);
        op_ = size != 0u && ctx.files().save("", file, save_buf_, size) ? Op::Saving : Op::None;
      }
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Master) && !in.shift) {  // MIDI LEARN on the value the encoder points at
      app::Midi& midi = ctx.midi();
      const app::ParamPage* page = play_.keys().page();
      if (midi.learning()) {
        midi.learn_cancel();
      } else if (page != nullptr && play_.keys().focus() < page->count) {
        (void)midi.learn(page->unit, page->ids[play_.keys().focus()]);
      }
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Pattern) && !in.shift) {
      (void)picker_.open(ctx, "LOAD A RIFF (Apps/hello)", "", "o3a");
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Copy) && !in.shift) {
      names_.open("SAVE THE RIFF AS", "riff");
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Play)) {
      (void)(e.playing() ? e.stop() : e.play());
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Rec)) {
      if (!in.shift) {
        rec_key(ctx);
        ctx.redraw();
        return true;
      }
      if (ctx.audio_in().available()) {  // SHIFT + REC: the monitor
        app::AudioIn& a = ctx.audio_in();
        a.monitor(a.monitor() == 0u ? app::AudioIn::kMonitorOn : 0u);
        ctx.redraw();
        return true;
      }
      return false;
    }
    if (in.press(Button::SampleRec) || in.press(Button::SamplePlay) || in.press(Button::SampleEdit)) {
      if (ctx.recorder().state() != hal::record::State::Idle) {
        rec_.note = "FINISH THE TAKE FIRST (REC)";  // the settings are what the NEXT take arms
      } else if (in.press(Button::SampleRec)) {
        rec_.start = static_cast<Start>((static_cast<unsigned>(rec_.start) + 1u) % kStarts);
      } else if (in.press(Button::SamplePlay)) {
        rec_.len = static_cast<std::uint8_t>((rec_.len + 1u) % kTakeLens);
      } else {
        rec_.tap = rec_.tap == hal::record::Tap::Input ? hal::record::Tap::Master : hal::record::Tap::Input;
      }
      ctx.redraw();
      return true;
    }
    if (in.press(Button::File) && in.shift) {  // SHIFT + FILE: the take as a WAV
      app::Files& files = ctx.files();
      rec_.note = nullptr;
      if (ctx.recorder().state() != hal::record::State::Done) {
        rec_.note = "NO TAKE YET (REC)";
      } else if (!files.busy() && ctx.recorder().save(files, "", "take")) {
        play_.set_note(nullptr);
        op_ = Op::SavingTake;
      } else if (!files.busy()) {
        rec_.note = "TAKE NOT SAVED (NO CARD OR EMPTY)";
      }
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Insert) || in.press(Button::Delete)) {
      if (in.press(Button::Delete)) {
        riff_.reset();
      } else {
        riff_.transpose(in.shift ? -1 : 1);
      }
      ctx.redraw();
      return true;
    }
    if (in.press(Button::File) || in.press(Button::SampleLoad)) {
      app::Files& files = ctx.files();
      if (!files.busy()) {  // (the buffers stay as they are while a job reads or writes them)
        play_.set_note(nullptr);
        if (in.press(Button::File)) {
          const std::size_t n = riff_.write(save_buf_, sizeof save_buf_);
          op_ = n != 0u && files.save("", "riff.o3a", save_buf_, n) ? Op::Saving : Op::None;
        } else {
          op_ = files.load("", "riff.o3a", load_buf_, sizeof load_buf_) ? Op::Loading : Op::None;
        }
        ctx.redraw();
      }
      return true;
    }
    if (in.press(Button::Up) || in.press(Button::Down)) {
      const int step = (in.shift ? 10'000 : 1'000) * (in.press(Button::Up) ? 1 : -1);
      const int t = static_cast<int>(e.own_tempo()) + step;  // (the engine's own: tempo() is the master's while one is followed)
      (void)e.set_tempo(static_cast<std::uint32_t>(t > 0 ? t : 0));
      ctx.redraw();
      return true;
    }
    return false;
  }

 private:
  enum class Op : std::uint8_t { None, Saving, Loading, SavingTake };

  // REC: arms a take as START, LEN and TAP say; stops one that is waiting or running; on a finished take, throws it away and arms the next. (A take the files service is writing stays.)
  void rec_key(app::Context& ctx) noexcept {
    app::Recorder& r = ctx.recorder();
    rec_.note = nullptr;
    if (!r.available()) {
      rec_.note = "NO RECORDER IN THIS BUILD";
      return;
    }
    const hal::record::State st = r.state();
    if (st == hal::record::State::Waiting || st == hal::record::State::Listening || st == hal::record::State::Recording) {
      r.disarm();
      return;
    }
    if (st == hal::record::State::Done) {
      if (op_ == Op::SavingTake) {
        rec_.note = "THE TAKE IS BEING SAVED";
        return;
      }
      r.release();
    }
    hal::record::Take t;
    t.buf = g_take.pcm;
    t.capacity_frames = kTakeSeconds[rec_.len] * kRate;
    t.channels = hal::record::Channels::Mix;
    switch (rec_.start) {
      case Start::Free: t.align = hal::record::Align::Free; break;
      case Start::Step: t.align = hal::record::Align::Step; break;
      case Start::Bar: t.align = hal::record::Align::Bar; break;
      case Start::Level:
        t.align = hal::record::Align::Level;
        t.threshold = kLevelThreshold;
        t.preroll_frames = kPreRoll;
        break;
    }
    r.set_tap(rec_.tap);
    if (!r.arm(t)) {
      rec_.note = "THE RECORDER REFUSED THE TAKE";
    }
  }

  app::BasicFilePicker<16> picker_;
  app::NameEntry names_;
  units::Synth synth_;
  units::Delay delay_{g_delay_l, g_delay_r, kDelayCapacity};
  Riff riff_;
  PlayScreen play_;
  RecUi rec_;
  Op op_ = Op::None;
  // The riff's file (formats/chunk_file.h: about 80 bytes) and the buffer a load reads into: word aligned, declared.
  alignas(4) std::uint8_t save_buf_[96] = {};
  alignas(4) std::uint8_t load_buf_[96] = {};
};

static_assert(alignof(HelloApp) >= 4, "the riff's file buffers are word aligned");

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept {
  static HelloApp app;
  return app;
}
