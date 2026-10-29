// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/runtime.h>

#include <o3sdk/app/firmware_flow.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/components.h>

namespace o3sdk::app {

namespace {

using ui::Color;
using ui::Face;
using ui::Rect;

// At most one frame this often when a screen asks for redraws (encoder turns): the display's swap paces it anyway.
constexpr core::Tick kMinFrameUs = 16'667;

// The strip over the bottom of the screen (system_config.h): a Data-face line in the theme's row pitch.
constexpr int kStripH = 28;

// MIDI. The render takes at most this many notes from the device in a block (the rest wait there); the main loop hands App::midi() at most
// this many messages in a pass.
constexpr unsigned kMidiInPerBlock = 32;
constexpr unsigned kMidiPerPass = 32;

// "120.0" from milli-BPM.
void format_tempo(std::uint32_t mbpm, char* out) noexcept {
  const std::uint32_t whole = mbpm / 1000u;
  const std::uint32_t tenth = (mbpm % 1000u) / 100u;
  char digits[8];
  unsigned n = 0;
  std::uint32_t v = whole;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < sizeof digits);
  unsigned o = 0;
  while (n != 0u) {
    out[o++] = digits[--n];
  }
  out[o++] = '.';
  out[o++] = static_cast<char>('0' + tenth);
  out[o] = '\0';
}

void format_percent(std::uint32_t pct, char* out) noexcept {
  unsigned o = 0;
  out[o++] = 'C';
  out[o++] = 'P';
  out[o++] = 'U';
  out[o++] = ' ';
  if (pct >= 100u) {
    out[o++] = static_cast<char>('0' + (pct / 100u) % 10u);
  }
  if (pct >= 10u) {
    out[o++] = static_cast<char>('0' + (pct / 10u) % 10u);
  }
  out[o++] = static_cast<char>('0' + pct % 10u);
  out[o++] = '%';
  out[o] = '\0';
}

const char* card_name(Files::Card c) noexcept {
  switch (c) {
    case Files::Card::None: return "none";
    case Files::Card::Ready: return "ready";
    case Files::Card::ReadOnly: return "readonly";
    case Files::Card::Unsupported: return "unsupported";
    case Files::Card::OnComputer: return "computer";
    case Files::Card::InUse: return "inuse";
  }
  return "none";
}

const char* files_phase_name(Files::Status::Phase p) noexcept {
  switch (p) {
    case Files::Status::Idle: return "idle";
    case Files::Status::Running: return "running";
    case Files::Status::Done: return "done";
    case Files::Status::Failed: return "failed";
  }
  return "idle";
}

const char* source_name(hal::capture::Source s) noexcept {
  switch (s) {
    case hal::capture::Source::LineMix: return "line";
    case hal::capture::Source::LineLeft: return "lineleft";
    case hal::capture::Source::LineRight: return "lineright";
    case hal::capture::Source::MicLow: return "miclow";
    case hal::capture::Source::MicHigh: return "michigh";
    case hal::capture::Source::Radio: return "radio";
    case hal::capture::Source::Usb: return "usb";
    case hal::capture::Source::Count: break;
  }
  return "line";
}

const char* radio_state_name(hal::capture::RadioStatus::State s) noexcept {
  using State = hal::capture::RadioStatus::State;
  switch (s) {
    case State::Off: return "off";
    case State::Starting: return "starting";
    case State::Ready: return "ready";
    case State::Tuning: return "tuning";
    case State::Seeking: return "seeking";
    case State::Absent: return "absent";
    case State::Failed: return "failed";
  }
  return "off";
}

// The render's load of the last block in percent (the header's number).
std::uint32_t cpu_percent(const dsp::EngineStats& s) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(s.cycles) * 100u / hal::audio::kCyclesPerBlock);
}

}  // namespace

Runtime::Runtime(App& app, dsp::Engine& engine, Platform& platform) noexcept
    : app_(app), engine_(engine), platform_(platform), sys_(platform.firmware, platform.build, platform.midi != nullptr, platform.capture != nullptr) {
  ctx_.engine_ = &engine_;
  ctx_.app_ = &app_;
  ctx_.midi_.bind(platform_.midi, &engine_);
  ctx_.audio_in_.bind(platform_.capture, &engine_);
  ctx_.radio_.bind(platform_.capture);
  ctx_.files_.bind(platform_.storage, platform_.writer, platform_.files_work, platform_.app_id != nullptr ? platform_.app_id : o3sdk_app_id(), platform_.card_claim);
}

// What the kit keeps for the app (settings.h) as it is now, and putting loaded ones in place.
KitValues Runtime::kit_values() const noexcept {
  KitValues v;
  v.theme = ctx_.theme_;
  v.midi = ctx_.midi_.config();
  v.in_source = static_cast<std::uint8_t>(ctx_.audio_in_.source());
  v.in_gain = static_cast<std::uint8_t>(ctx_.audio_in_.gain());
  v.in_monitor = static_cast<std::uint8_t>(ctx_.audio_in_.monitor());
  v.radio_region = static_cast<std::uint8_t>(ctx_.radio_.region());
  v.radio_freq = static_cast<std::uint16_t>(ctx_.radio_.freq());
  return v;
}

// The loaded audio choice over the app's: only what differs from what is in place is applied, so that a card that holds the defaults (an app that never used audio in) costs the codec nothing
// and the gate nothing. The radio's region and station go first: selecting the radio powers the chip up on them.
void Runtime::apply_kit_audio(const KitValues& v) noexcept {
  const KitValues now = kit_values();
  if (v.radio_region != now.radio_region || v.radio_freq != now.radio_freq) {
    ctx_.radio_.restore(static_cast<hal::capture::Region>(v.radio_region), v.radio_freq);
  }
  if (v.in_source != now.in_source || v.in_gain != now.in_gain || v.in_monitor != now.in_monitor) {
    ctx_.audio_in_.restore(static_cast<AudioIn::Source>(v.in_source), v.in_gain, v.in_monitor);
  }
}

Files::SettingsLoad Runtime::load_settings() noexcept {
  KitValues v = kit_values();  // (the app's defaults: App::setup has run)
  const Files::SettingsLoad result = ctx_.files_.load_settings(v);
  if (result == Files::SettingsLoad::Loaded || result == Files::SettingsLoad::FromBackup) {
    ctx_.theme_ = v.theme;
    ctx_.midi_.set_config(v.midi);
    apply_kit_audio(v);
    ctx_.redraw_ = true;
  }
  return result;
}

bool Runtime::start() noexcept {
  ctx_.now_ = platform_.clock.now();
  ctx_.ms_ = platform_.clock.uptime_ms();
  const bool assets = ui::upload_assets(platform_.display) == hal::UploadResult::Ok;
  ctx_.midi_.push_config();  // (the device starts from the kit's defaults; App::setup may change them)
  app_.setup(ctx_);
  engine_.start();
  switch_screen();
  started_ = true;
  next_frame_ = ctx_.now_;
  return assets;
}

void Runtime::switch_screen() noexcept {
  if (ctx_.next_ == nullptr) {
    return;
  }
  Screen* next = ctx_.next_;
  ctx_.next_ = nullptr;
  if (next == ctx_.screen_) {
    return;
  }
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->leave(ctx_);
  }
  ctx_.screen_ = next;
  ctx_.screen_->enter(ctx_);
  ctx_.redraw_ = true;
}

// One input: the system layer's or the app's. The Config button is the runtime's and never reaches the app (press or release). While the
// system screen is open (or the input is locked) a press or a turn is the system's, and a press it took is not released to the app
// either; the release of something the app saw pressed does reach it, so a pad held when the screen opened does not leave a note on.
void Runtime::route(const Input& in) noexcept {
  if (in.is(Button::Config)) {
    if (in.type == Input::Type::Press && !locked_) {
      if (sys_.open()) {
        sys_.close(ctx_);  // (not past INSTALL)
      } else {
        sys_.show(ctx_);
      }
    }
    return;
  }
  const bool key = in.kind == core::InputKind::Button || in.kind == core::InputKind::Pad;
  std::uint64_t* taken = in.kind == core::InputKind::Pad ? &taken_pads_ : &taken_buttons_;
  const std::uint64_t bit = key && in.id < 64u ? std::uint64_t{1} << in.id : 0u;
  if (in.type == Input::Type::Release) {
    if ((*taken & bit) != 0u) {
      *taken &= ~bit;  // its press was the system's
      return;
    }
  } else if (locked_ || sys_.open()) {
    if (in.type == Input::Type::Press) {
      *taken |= bit;
    }
    if (!locked_) {
      sys_.input(ctx_, in);
    }
    return;
  }
  if (!app_.input(ctx_, in) && ctx_.screen_ != nullptr) {
    ctx_.screen_->input(ctx_, in);
  }
}

void Runtime::poll() noexcept {
  ctx_.now_ = platform_.clock.now();
  ctx_.ms_ = platform_.clock.uptime_ms();

  core::InputEvent e;
  for (unsigned n = 0; n < 32u && platform_.input.poll(e); ++n) {
    ctx_.panel_.apply(e);
    route(make_input(e, ctx_.panel_.shift()));
    switch_screen();
  }

  poll_midi();
  ctx_.audio_in_.poll(ctx_.ms_);
  ctx_.radio_.poll();
  app_.update(ctx_);
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->update(ctx_);
  }
  switch_screen();
  sys_.update(ctx_);

  // The card's jobs (files.h): a slice of this pass, the kit's settings after their quiet, and the strip line of a save or a load.
  ctx_.files_.poll(platform_.clock, Files::kJobBudgetUs, kit_values());
  KitValues loaded;
  if (ctx_.files_.take_loaded(loaded)) {  // the settings of a card put in after the boot, read by the service: applied as load_settings() applies them
    ctx_.theme_ = loaded.theme;
    ctx_.midi_.set_config(loaded.midi);
    apply_kit_audio(loaded);
    ctx_.redraw_ = true;
  }
  Files::Strip line;
  if (!locked_ && ctx_.files_.strip(line)) {
    system_strip(line.text, line.progress, line.result ? SystemConfig::kNoticeMs : SystemConfig::kStripMs);
  }

  const std::uint32_t since = core::tick_elapsed(ctx_.now_, next_frame_ - kFrameUs);
  if (core::tick_reached(ctx_.now_, next_frame_) || (ctx_.redraw_ && since >= kMinFrameUs)) {
    // A frame the display was too busy to take (its last swap still pending, up to about 18.5 ms on the FT812) is tried again on
    // the next pass, the request kept; any other outcome waits for the next period.
    if (draw() || !busy_) {
      next_frame_ = ctx_.now_ + kFrameUs;
    }
  }
  if (platform_.leds != nullptr) {
    (void)platform_.leds->flush();
  }
}

bool Runtime::draw() noexcept {
  update_leds();
  eve::DisplayListBuilder builder(platform_.display, list_.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, ui::theme(ctx_.theme_));
  busy_ = !r.begin(Color::Pane);
  if (busy_) {
    ++frames_busy_;
    return false;
  }
  ctx_.redraw_ = false;
  draw_header(r);
  if (sys_.open()) {
    sys_.draw(ctx_, r, kContentArea);
  } else if (ctx_.screen_ != nullptr) {
    ctx_.screen_->draw(ctx_, r, kContentArea);
  }
  sys_.draw_strip(r, Rect{0, ui::kScreenH - kKeysH - kStripH, ui::kScreenW, kStripH});
  draw_keys(r);
  last_words_ = static_cast<std::uint32_t>(r.words());
  if (!r.end()) {
    ++frame_failures_;
    return false;
  }
  ++frames_;
  return true;
}

void Runtime::draw_header(ui::Renderer& r) noexcept {
  const Rect area{0, 0, ui::kScreenW, kHeaderH};
  ui::strip(r, area, ui::Edge::Bottom);
  const ui::Theme& t = r.theme();
  const int data_y = ui::caps_y(t, Face::Data, 0, kHeaderH);
  const int label_y = ui::caps_y(t, Face::Label, 0, kHeaderH);

  const char* name = app_.info().name;
  r.text(8, data_y, name, Color::ChromeText, Face::Data);
  const char* title = sys_.open() ? "CONFIG" : (ctx_.screen_ != nullptr ? ctx_.screen_->title() : nullptr);
  if (title != nullptr) {
    r.text(8 + r.text_width(name, Face::Data) + 14, label_y, title, Color::ChromeDim, Face::Label);
  }

  const dsp::EngineStats s = engine_.stats();
  char tempo[12];
  format_tempo(engine_.tempo(), tempo);
  const int tempo_x = 500;
  r.text(tempo_x, data_y, tempo, Color::ChromeText, Face::Data);
  r.text(tempo_x + r.text_width(tempo, Face::Data) + 4, label_y, "BPM", Color::ChromeDim, Face::Label);
  (void)ui::transport_badge(r, 610, 5, kHeaderH - 10, s.playing);
  if (platform_.cpu_meter) {
    char cpu[12];
    format_percent(cpu_percent(s), cpu);
    r.text(ui::kScreenW - 8 - r.text_width(cpu, Face::Label), label_y, cpu, Color::ChromeDim, Face::Label);
  }
}

void Runtime::draw_keys(ui::Renderer& r) noexcept {
  SoftKeys keys;
  if (sys_.open()) {
    sys_.soft_keys(ctx_, keys);
  } else if (ctx_.screen_ != nullptr) {
    ctx_.screen_->soft_keys(ctx_, keys);
  }
  const char* labels[kFKeyCount];
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    labels[i] = keys.label[i] != nullptr ? keys.label[i] : "";
  }
  const Rect area{0, ui::kScreenH - kKeysH, ui::kScreenW, kKeysH};
  ui::strip(r, area, ui::Edge::Top);  // the key-caps are drawn on chrome
  ui::soft_keys(r, area, labels, kFKeyCount, keys.dim, keys.hot);
}

void Runtime::update_leds() noexcept {
  leds_.clear();
  if (ctx_.screen_ != nullptr && !sys_.open()) {  // (the pads are not the app's while the system screen is up)
    ctx_.screen_->leds(ctx_, leds_);
  }
  if (platform_.leds != nullptr) {
    for (unsigned i = 0; i < kPadCount; ++i) {
      platform_.leds->set(i, leds_.get(i));
    }
  }
}

void Runtime::system_strip(const char* text, int progress, std::uint32_t hold_ms) noexcept {
  sys_.strip(text, progress, hold_ms, platform_.clock.now());
}

std::size_t Runtime::state_line(char* out, std::size_t cap) const noexcept {
  if (out == nullptr || cap == 0u) {
    return 0;
  }
  StateWriter w(out, cap);
  w.str("app", app_.info().name);
  w.str("screen", ctx_.screen_ != nullptr ? ctx_.screen_->title() : nullptr);
  w.flag("playing", engine_.playing());
  w.num("tempo", engine_.tempo());
  w.num("cpu", platform_.cpu_meter ? cpu_percent(engine_.stats()) : 0u);
  w.num("frames", frames_);
  w.flag("sys", sys_.open());
  w.str("section", sys_.section_name());
  w.str("fw", firmware_phase_name(sys_.firmware().phase));
  w.str("card", card_name(ctx_.files_.card()));
  w.str("files", files_phase_name(ctx_.files_.status().phase));
  if (ctx_.audio_in_.available()) {
    w.str("in", source_name(ctx_.audio_in_.source()));
    w.num("gain", ctx_.audio_in_.gain());
    w.num("mon", ctx_.audio_in_.monitor());
    w.flag("gate", !ctx_.audio_in_.settling());
    w.str("radio", radio_state_name(ctx_.radio_.status().state));
    w.num("freq", ctx_.radio_.freq());
  }
  std::size_t n = w.size();
  // The app's pairs go after a space; the room is what the kit's pairs left (the separator, one character and the terminator at least).
  if (w.complete() && n + 3u <= cap) {
    const std::size_t room = cap - n - 1u;  // out[n] is the separator
    std::size_t m = app_.describe_state(ctx_, out + n + 1u, room);
    if (m >= room) {
      m = room - 1u;  // an app that reports more than it was given: keep what fits
    }
    if (m != 0u) {
      out[n] = ' ';
      n += 1u + m;
    }
    out[n] = '\0';
  }
  return n;
}

// Main loop: the app's play() or stop() while the engine follows a master goes to the device, and the messages no route took go to the app.
// While the input is locked (the power-off, an install: the system layer has the unit) the app hears nothing: what the device has is read and
// dropped, so it does not pile up, and App::midi() does not run.
void Runtime::poll_midi() noexcept {
  hal::midi::Device* dev = platform_.midi;
  if (dev == nullptr) {
    return;
  }
  const core::TransportRequest request = engine_.take_transport_request();
  if (request != core::TransportRequest::None) {
    if (engine_.following()) {
      dev->request(request);
    } else if (request == core::TransportRequest::Play) {  // the master was let go before this pass: the request is the engine's own again
      (void)engine_.play();
    } else {
      (void)engine_.stop();
    }
  }
  const bool deaf = locked_ || sys_.locked();
  core::MidiMessage m;
  dsp::UnitId units[Midi::kMaxRoutes];
  for (unsigned n = 0; n < kMidiPerPass && dev->poll(m); ++n) {
    if (!deaf && ctx_.midi_.match(m, units) == 0u) {  // (a routed note went to its unit in the render)
      (void)app_.midi(ctx_, m);
    }
  }
}

// Render context, before the engine: the notes of this block for the routes, at their frames, and the block's transport.
void Runtime::render_midi_in(std::uint32_t frames) noexcept {
  core::MidiMessage in[kMidiInPerBlock];
  core::ClockFrame clock;
  unsigned n = platform_.midi->begin_block(frames, in, kMidiInPerBlock, clock);
  n = n < kMidiInPerBlock ? n : kMidiInPerBlock;
  dsp::UnitId units[Midi::kMaxRoutes];
  for (unsigned i = 0; i < n; ++i) {
    const core::MidiMessage& m = in[i];
    const unsigned routed = ctx_.midi_.match(m, units);
    if (routed == 0u) {
      continue;
    }
    const auto frame = static_cast<std::uint16_t>(m.frame < frames ? m.frame : frames - 1u);
    dsp::Event e;
    if (core::midi_kind(m.status) == core::kMidiControl) {  // all sound off, all notes off
      e = dsp::all_notes_off(frame);
    } else if (core::midi_kind(m.status) == core::kMidiNoteOn && m.data2 != 0u) {
      e = dsp::note_on(m.data1, m.data2, 0u, frame);
    } else {  // a note off, or a note on with velocity 0
      e = dsp::note_off(m.data1, frame);
    }
    for (unsigned r = 0; r < routed; ++r) {
      (void)engine_.inject(units[r], e);
    }
  }
  engine_.follow(clock);
}

void Runtime::render_block(void* user, const hal::audio::BlockIo& io) noexcept {
  auto* self = static_cast<Runtime*>(user);
  constexpr std::uint32_t n = hal::audio::kBlock;
  if (!self->started_ || (io.flags & hal::audio::kFlagSilence) != 0u) {
    for (std::uint32_t i = 0; i < hal::audio::kBlockSamples; ++i) {
      io.out[i] = 0;
    }
    if (!self->started_) {
      return;
    }
  }
  float in_l[n];
  float in_r[n];
  float out_l[n];
  float out_r[n];
  dsp::s16_to_planes(io.in, in_l, in_r, n);
  // Audio in's choice of the line's channels (audio_in.h): one relaxed load a block, and no work at all until an app or the user has selected a line source.
  const std::uint8_t map = self->ctx_.audio_in_.channel_map();
  if (map != 0u) {
    for (std::uint32_t i = 0; i < n; ++i) {
      const float l = in_l[i];
      const float r = in_r[i];
      const float v = map == 1u ? (l + r) * 0.5f : (map == 2u ? l : r);
      in_l[i] = v;
      in_r[i] = v;
    }
  }
  hal::midi::Device* midi = self->platform_.midi;
  if (midi != nullptr) {
    self->render_midi_in(n);
  }
  self->engine_.render(in_l, in_r, out_l, out_r, n);
  if (midi != nullptr) {
    unsigned sent = 0;
    const core::MidiMessage* out = self->engine_.midi_out(sent);
    midi->end_block(self->engine_.block_frame(), out, sent);
  }
  if ((io.flags & hal::audio::kFlagSilence) == 0u) {
    dsp::planes_to_s16(out_l, out_r, io.out, n);
  }
}

}  // namespace o3sdk::app
