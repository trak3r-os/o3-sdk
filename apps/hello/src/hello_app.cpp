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
//   CONFIG        the system Config screen (the kit's, not the app's): FIRMWARE, AUDIO (the input's source, gain and monitor; the radio), MIDI, DISPLAY (the theme), INFO
//   MIDI          notes on the channel the user chose in the Config's MIDI section (any, by default) play SYNTH, frame-accurate; with FOLLOW on
//                 there, the riff and the tempo follow a MIDI clock on DIN or USB (the riff is a clocked hook; it needs nothing to do so)
//   INSERT        the riff up a semitone (SHIFT: down), so that there is something to save; DELETE puts the default riff back
//   FILE          SAVE the riff to the card, Apps/hello/riff.o3a (Context::files(): a job of the main loop, never a pause of the sound or the screen)
//   SAMPLE LOAD   LOAD it back (a riff that is not there says so on the strip). The card, the file and the last job show on the screen.
//   REC           the audio in through the master: the monitor on (the usual 70 %) and off. Where the input comes from (line, microphone, radio) is the Config's AUDIO
//                 section; the screen's IN meter shows what arrives. With no capture device (a platform without audio in) neither exists.
//
// One instrument (SYNTH) on slot 0, a DELAY on send A.

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/param_keys.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/app/widgets.h>
#include <o3sdk/core/placement.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/formats/chunk_file.h>
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
  void init(dsp::UnitId synth, dsp::UnitId delay, const Riff* riff) noexcept {
    synth_ = synth;
    riff_ = riff;
    pages_[0] = app::ParamPage{"SYNTH", synth, {0, 1, 2, 3, 4, 5, 6, 7}, 8};
    pages_[1] = app::ParamPage{"DELAY", delay, {0, 1, 2, 3, 0, 0, 0, 0}, 4};
    keys_.set_pages(pages_, 2);
    keys_.set_page(0);
    for (auto& n : held_) {
      n = -1;
    }
  }

  const char* title() const noexcept override { return "PLAY"; }

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
    // The audio in (Context::audio_in()): its peak, while the monitor plays it ("MON"). Asking for the peaks is what makes the engine measure them; nothing of this exists without a capture device.
    if (ctx.audio_in().available()) {
      const dsp::InputPeaks in_peak = ctx.audio_in().peaks();
      r.text(rx, area.y + 84, ctx.audio_in().monitor() != 0u ? "MON" : "IN", ctx.audio_in().monitor() != 0u ? Color::Play : Color::TextDim, Face::Label);
      app::level_meter(r, Rect{rx + 40, area.y + 86, rw - 40, 10}, in_peak.l > in_peak.r ? in_peak.l : in_peak.r);
    }

    // What the keys do.
    const int hy = map_y + 4 * cell_h + 18;
    r.text(16, hy, "PADS play   F1-F8 pick a value   ENCODER turn it (SHIFT coarse)   hold F + turn: for a moment", Color::TextDim,
           Face::Label);
    r.text(16, hy + 20, "PARAMS page   PLAY riff   UP/DOWN tempo   LEFT/RIGHT octave   CONFIG system", Color::TextDim, Face::Label);
    r.text(16, hy + 40, "INSERT riff +1 (SHIFT -1)   DELETE default riff   FILE save the riff   SAMPLE LOAD load it", Color::TextDim, Face::Label);
    // The card and the last job of the files service (Context::files()): polled, never waited for.
    const app::Files& files = ctx.files();
    const app::Files::Status fs = files.status();
    r.text(rx, area.y + 100, "CARD", Color::TextDim, Face::Label);
    r.text(rx + 48, area.y + 100, !files.available() ? "NONE" : card_name(files.card()), files.card() == app::Files::Card::Ready ? Color::Play : Color::Text, Face::Label);
    r.text(rx, area.y + 120, "FILE", Color::TextDim, Face::Label);
    r.text(rx + 48, area.y + 120, note_ != nullptr ? note_ : (fs.phase == app::Files::Status::Idle ? "-" : fs.text),
           note_ != nullptr || fs.phase == app::Files::Status::Failed ? Color::Accent : Color::Text, Face::Label);

    app::knob_strip(r, Rect{0, area.y + area.h - app::kKnobStripH, ui::kScreenW, app::kKnobStripH}, ctx.engine(), keys_);
  }

  void soft_keys(app::Context& ctx, app::SoftKeys& out) noexcept override {
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
  // Bottom row C3..B3 (+ the octave shift), one octave higher each row up.
  int note_for(unsigned pad) const noexcept {
    const int n = 48 + 12 * octave_ + 12 * (static_cast<int>(app::kPadRows) - 1 - static_cast<int>(app::pad_row(pad))) +
                  static_cast<int>(app::pad_col(pad));
    return n >= 0 && n <= 127 ? n : -1;
  }

  dsp::UnitId synth_ = dsp::kNoUnit;
  const Riff* riff_ = nullptr;
  app::ParamPage pages_[2] = {};
  app::ParamKeys keys_;
  int octave_ = 0;
  const char* note_ = nullptr;
  int held_[app::kPadCount] = {};
};

// ---- The app ----------------------------------------------------------------------------------------------------------------

constexpr std::uint32_t kDelayCapacity = 88'200;  // 2 s at 44.1 kHz, each side
O3SDK_BIG float g_delay_l[kDelayCapacity];
O3SDK_BIG float g_delay_r[kDelayCapacity];

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
    play_.init(synth, delay, &riff_);
    ctx.show(play_);
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
    return w.size();
  }

  // The riff's file jobs end here: a load that is Done puts the riff in place (the file is checked first: a damaged or foreign one changes nothing and says so).
  void update(app::Context& ctx) noexcept override {
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
    op_ = Op::None;
    ctx.redraw();
  }

  bool input(app::Context& ctx, const app::Input& in) noexcept override {
    dsp::Engine& e = ctx.engine();
    if (in.press(Button::Play)) {
      (void)(e.playing() ? e.stop() : e.play());
      ctx.redraw();
      return true;
    }
    if (in.press(Button::Rec) && ctx.audio_in().available()) {
      app::AudioIn& a = ctx.audio_in();
      a.monitor(a.monitor() == 0u ? app::AudioIn::kMonitorOn : 0u);
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
  enum class Op : std::uint8_t { None, Saving, Loading };

  units::Synth synth_;
  units::Delay delay_{g_delay_l, g_delay_r, kDelayCapacity};
  Riff riff_;
  PlayScreen play_;
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
