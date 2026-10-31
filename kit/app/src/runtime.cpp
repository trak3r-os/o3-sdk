// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/runtime.h>

#include <o3sdk/app/brightness.h>
#include <o3sdk/app/firmware_flow.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/components.h>

#include "sys_text.h"

#include <cstdlib>

// An app that hands the runtime a table the display cannot take (App::assets(): outside its region, misaligned, overlapping) has a programming error:
// it aborts in a debug build and in every host build, and costs nothing in the MK66's release build (NDEBUG on an ARM target), where the table is refused.
#if !defined(NDEBUG) || !defined(__arm__)
#define O3SDK_APP_CHECK(cond) \
  do {                       \
    if (!(cond)) {           \
      std::abort();          \
    }                        \
  } while (false)
#else
#define O3SDK_APP_CHECK(cond) \
  do {                       \
  } while (false)
#endif

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
    : app_(app), engine_(engine), platform_(platform), sys_(platform.firmware, platform.build, platform.midi != nullptr, platform.capture != nullptr, platform.usb_audio != nullptr,
           platform.storage != nullptr && platform.writer != nullptr && platform.files_work != nullptr, platform.backlight != nullptr, platform.backlight != nullptr && platform.leds != nullptr, platform.crash) {
  ctx_.engine_ = &engine_;
  ctx_.app_ = &app_;
  ctx_.cpu_meter_ = platform_.cpu_meter;
  ctx_.midi_.bind(platform_.midi, &engine_);
  ctx_.audio_in_.bind(platform_.capture, &engine_);
  ctx_.audio_in_.bind_usb(platform_.usb_audio);
  ctx_.usb_audio_.bind(platform_.usb_audio, &ctx_.files_);
  ctx_.radio_.bind(platform_.capture);
  ctx_.recorder_.bind(platform_.recorder);
  ctx_.files_.bind(platform_.storage, platform_.writer, platform_.files_work, platform_.app_id != nullptr ? platform_.app_id : o3sdk_app_id(), platform_.card_claim);
}

// What the kit keeps for the app (settings.h) as it is now, and putting loaded ones in place. Built every pass for the files service (it compares it with what it last saved): 368 bytes of stores
// and one config_get() for each value item of the app's pages (at most 32; HELLO has 3), about 0.5 us on the unit by estimate (not measured: it runs once a pass of the main loop, not in the render).
// An app's config_get() can change without telling the kit, so there is no dirty flag to keep it from being asked.
KitValues Runtime::kit_values() const noexcept {
  KitValues v;
  v.theme = ctx_.theme_;
  v.midi = ctx_.midi_.config();
  v.in_source = static_cast<std::uint8_t>(ctx_.audio_in_.source());
  v.in_gain = static_cast<std::uint8_t>(ctx_.audio_in_.gain());
  v.in_monitor = static_cast<std::uint8_t>(ctx_.audio_in_.monitor());
  v.radio_region = static_cast<std::uint8_t>(ctx_.radio_.region());
  v.radio_freq = static_cast<std::uint16_t>(ctx_.radio_.freq());
  ctx_.usb_audio_.fill(v);
  v.brightness = ctx_.brightness_;
  v.pad_lights = ctx_.pad_lights_;
  ctx_.midi_.fill(v);
  // The values of the app's pages, clamped into their descriptors: what config_get() gives now.
  const ConfigView& cv = ctx_.config_;
  for (unsigned p = 0; p < cv.pages; ++p) {
    for (unsigned i = 0; i < cv.page[p].count; ++i) {
      const ConfigItem* it = ((cv.page[p].mask >> i) & 1u) != 0u ? &cv.page[p].items[i] : nullptr;
      if (it != nullptr && it->value != nullptr && v.app_count < kAppValueSlots) {
        v.app_key[v.app_count] = it->key;
        v.app_value[v.app_count] = core::param_clamp(*it->value, app_.config_get(ctx_, it->key));
        ++v.app_count;
      }
    }
  }
  return v;
}

// The loaded settings over the app's: the display, the learn bindings, and the values of the app's pages (set only where they differ from what the app has, so a card that holds what setup()
// set costs the app nothing; a value of an item the app no longer has is dropped, one outside its descriptor is clamped).
void Runtime::apply_kit_app(const KitValues& v) noexcept {
  ctx_.brightness_ = v.brightness > 2u ? 2u : v.brightness;
  ctx_.pad_lights_ = v.pad_lights > 2u ? 2u : v.pad_lights;
  ctx_.midi_.restore(v);
  for (unsigned i = 0; i < v.app_count && i < kAppValueSlots; ++i) {
    const ConfigItem* it = ctx_.config_.find_value(v.app_key[i]);
    if (it == nullptr) {
      continue;
    }
    const float value = core::param_clamp(*it->value, v.app_value[i]);
    if (app_.config_get(ctx_, it->key) != value) {
      app_.config_set(ctx_, it->key, value);
    }
  }
  apply_display();
}

// The backlight is told only what it was not: a setting that is high (the default) is the hardware's own and costs nothing.
void Runtime::apply_display() noexcept {
  if (platform_.backlight != nullptr && ctx_.brightness_ != backlight_applied_) {
    backlight_applied_ = ctx_.brightness_;
    platform_.backlight(backlight_duty(ctx_.brightness_));
  }
}

// The loaded audio choice over the app's: only what differs from what is in place is applied, so that a card that holds the defaults (an app that never used audio in) costs the codec nothing
// and the gate nothing. The radio's region and station go first: selecting the radio powers the chip up on them.
void Runtime::apply_kit_audio(const KitValues& v) noexcept {
  ctx_.usb_audio_.restore(v);  // (the port starts with it at the boot, the level and the slots go once the driver runs)
  const KitValues now = kit_values();
  if (v.radio_region != now.radio_region || v.radio_freq != now.radio_freq) {
    ctx_.radio_.restore(static_cast<hal::capture::Region>(v.radio_region), v.radio_freq);
  }
  if (v.in_source != now.in_source || v.in_gain != now.in_gain || v.in_monitor != now.in_monitor) {
    ctx_.audio_in_.restore(static_cast<AudioIn::Source>(v.in_source), v.in_gain, v.in_monitor);
  }
}

Files::SettingsLoad Runtime::load_settings() noexcept {
  if (!started_) {
    return Files::SettingsLoad::NotRead;  // the app's units and pages are not there until start(): the learn bindings and the page values could not be checked against them
  }
  KitValues v = kit_values();  // (the app's defaults: App::setup has run)
  const Files::SettingsLoad result = ctx_.files_.load_settings(v);
  if (result == Files::SettingsLoad::Loaded || result == Files::SettingsLoad::FromBackup) {
    ctx_.theme_ = v.theme;
    ctx_.midi_.set_config(v.midi);
    apply_kit_audio(v);
    apply_kit_app(v);
    ctx_.redraw_ = true;
  }
  return result;
}

bool Runtime::start() noexcept {
  ctx_.now_ = platform_.clock.now();
  ctx_.ms_ = platform_.clock.uptime_ms();
  const bool assets = ui::upload_assets(platform_.display) == hal::UploadResult::Ok;
  if (assets) {
    // The app's own bitmaps (app_assets.h), only on top of the kit's. Never part of the result: a table that did not land is the app's to read.
    const AppAsset* rows = nullptr;
    const std::size_t count = app_.assets(rows);
    const AppAssetsResult up = upload_app_assets(platform_.display, rows, count);
    O3SDK_APP_CHECK(up != AppAssetsResult::Refused);
    ctx_.app_assets_ok_ = up == AppAssetsResult::Ok;
  }
  ctx_.midi_.push_config();  // (the device starts from the kit's defaults; App::setup may change them)
  app_.setup(ctx_);
  ctx_.config_.build(app_);  // (the app's Config pages: constexpr tables, read once)
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
  help_.close();  // (what it says is about the screen that is leaving)
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->leave(ctx_);
  }
  ctx_.screen_ = next;
  ctx_.screen_->enter(ctx_);
  ctx_.redraw_ = true;
}

// One input: the system layer's or the app's. The Config button is the runtime's and never reaches the app (press or release): it opens the system screen, or HELP with Shift held, and closes
// whichever is open. While the system screen or HELP is open (or the input is locked) a press or a turn is theirs, and a press they took is not released to the app either; the release of something
// the app saw pressed does reach it, so a pad held when the screen opened does not leave a note on, nor the Shift that opened HELP stay held.
void Runtime::route(const Input& in) noexcept {
  if (in.is(Button::Config)) {
    if (in.type == Input::Type::Press && !locked_) {
      if (help_.open()) {
        help_.close();
        ctx_.redraw();
      } else if (sys_.open()) {
        sys_.close(ctx_);  // (not past INSTALL)
      } else if (!(in.shift && open_help())) {
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
  } else if (locked_ || sys_.open() || help_.open()) {
    if (in.type == Input::Type::Press) {
      *taken |= bit;
    }
    if (!locked_) {
      if (help_.open()) {
        help_.input(in);
        ctx_.redraw();
      } else {
        sys_.input(ctx_, in);
      }
    }
    return;
  }
  if (!app_.input(ctx_, in) && ctx_.screen_ != nullptr) {
    ctx_.screen_->input(ctx_, in);
  }
}

// Shift + Config: asks the screen what its controls do (Screen::help) and opens HELP over it. Shift is let go of in the panel's state for the two calls (the screen's help and its soft keys are those
// of the screen without Shift: the caps show its plain labels, the Shift row comes from Help::fkey_shift) and held again after. false: there is no screen yet.
bool Runtime::open_help() noexcept {
  Screen* const screen = ctx_.screen_;
  if (screen == nullptr) {
    return false;
  }
  const Panel held = ctx_.panel_;
  ctx_.panel_.apply(core::InputEvent{ctx_.now_, core::InputKind::Button, static_cast<std::uint8_t>(Button::Shift), 0});
  SoftKeys keys;
  screen->soft_keys(ctx_, keys);
  help_.show(*screen, ctx_, keys);
  ctx_.panel_ = held;
  ctx_.redraw();
  return true;
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
  ctx_.usb_audio_.poll(ctx_.ms_);
  {
    const hal::usb_audio::DiskRefusal refused = ctx_.usb_audio_.take_share_refusal();  // a share that waited for the settings and was then refused
    if (refused != hal::usb_audio::DiskRefusal::None) {
      system_strip(hal::usb_audio::disk_refusal_text(refused), -1, SystemConfig::kNoticeMs);
    }
  }
  if (platform_.recorder != nullptr) {  // the Input tap hears the line's channel map as the engine does (hal/record.h)
    platform_.recorder->set_line_map(static_cast<hal::record::LineMap>(ctx_.audio_in_.channel_map()));
  }
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
    apply_kit_app(loaded);
    ctx_.redraw_ = true;
  }
  Files::Strip line;
  if (!locked_ && ctx_.files_.strip(line)) {
    system_strip(line.text, line.progress, line.result ? SystemConfig::kNoticeMs : SystemConfig::kStripMs);
  }

  apply_display();
  const std::uint32_t since = core::tick_elapsed(ctx_.now_, next_frame_ - kFrameUs);
  if (!held_ && (core::tick_reached(ctx_.now_, next_frame_) || (ctx_.redraw_ && since >= kMinFrameUs))) {
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
  ctx_.cpu_percent_ = platform_.cpu_meter ? cpu_percent(engine_.stats()) : 0u;
  // The app's screen decides the theme and the chrome while the system Config screen is closed; Config always has the context's theme and the full chrome.
  const Screen* const screen = sys_.open() ? nullptr : ctx_.screen_;
  const ui::Theme* const screen_theme = screen != nullptr ? screen->theme(ctx_) : nullptr;
  const bool helping = help_.open();  // (HELP always has the full chrome, in the screen's theme)
  const bool bare = !helping && screen != nullptr && screen->chrome(ctx_) == Chrome::None;
  eve::DisplayListBuilder builder(platform_.display, list_.words, eve::kMaxDisplayListWords);
  ui::Renderer r(builder, screen_theme != nullptr ? *screen_theme : ui::theme(ctx_.theme_));
  busy_ = !r.begin(Color::Pane);
  if (busy_) {
    ++frames_busy_;
    return false;
  }
  ctx_.redraw_ = false;
  if (!bare) {
    draw_header(r);
  }
  if (sys_.open()) {
    sys_.draw(ctx_, r, kContentArea);
  } else if (helping) {
    help_.draw(r, kContentArea);
  } else if (ctx_.screen_ != nullptr) {
    ctx_.screen_->draw(ctx_, r, bare ? kScreenArea : kContentArea);
  }
  if (bare) {  // the strip goes last, over whatever the screen drew at the bottom
    sys_.draw_strip(r, Rect{0, ui::kScreenH - kStripH, ui::kScreenW, kStripH});
  } else {
    sys_.draw_strip(r, Rect{0, ui::kScreenH - kKeysH - kStripH, ui::kScreenW, kStripH});
    draw_keys(r);
  }
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
  core::FixedString<48> help_title;
  const char* title = sys_.open() ? "CONFIG" : (ctx_.screen_ != nullptr ? ctx_.screen_->title() : nullptr);
  if (help_.open() && !sys_.open()) {
    help_title.assign("HELP  ");
    help_title.append(title != nullptr ? title : "");
    title = help_title.c_str();
  }
  if (title != nullptr) {
    r.text(8 + r.text_width(name, Face::Data) + 14, label_y, title, Color::ChromeDim, Face::Label);
  }

  const dsp::EngineStats s = engine_.stats();
  const HeaderStyle style = app_.header();
  if (style.tempo) {
    char tempo[12];
    format_tempo(engine_.tempo(), tempo);
    const int tempo_x = 500;
    r.text(tempo_x, data_y, tempo, Color::ChromeText, Face::Data);
    r.text(tempo_x + r.text_width(tempo, Face::Data) + 4, label_y, "BPM", Color::ChromeDim, Face::Label);
  }
  if (style.transport) {
    (void)ui::transport_badge(r, 610, 5, kHeaderH - 10, s.playing);
  }
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
  } else if (help_.open()) {
    help_.soft_keys(keys);
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
  if (ctx_.screen_ != nullptr && !sys_.open() && !help_.open()) {  // (the pads are not the app's while the system screen or HELP is up)
    ctx_.screen_->leds(ctx_, leds_);
  }
  if (platform_.leds != nullptr) {
    const unsigned lights = platform_.backlight != nullptr ? ctx_.pad_lights_ : 2u;  // (the PAD LIGHTS row is there on the same condition: a platform with a backlight and pad LEDs)
    for (unsigned i = 0; i < kPadCount; ++i) {
      platform_.leds->set(i, lights >= 2u ? leds_.get(i) : scale_pad_level(leds_.get(i), lights));
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
  w.num("setup_errors", engine_.setup_errors());
  if (ctx_.audio_in_.available()) {
    w.str("in", source_name(ctx_.audio_in_.source()));
    w.num("gain", ctx_.audio_in_.gain());
    w.num("mon", ctx_.audio_in_.monitor());
    w.flag("gate", !ctx_.audio_in_.settling());
    w.str("radio", radio_state_name(ctx_.radio_.status().state));
    w.num("freq", ctx_.radio_.freq());
  }
  if (ctx_.usb_audio_.available()) {
    const hal::usb_audio::DiskStatus disk = ctx_.usb_audio_.disk();
    w.str("usb", ctx_.usb_audio_.audio() ? hal::usb_audio::profile_name(ctx_.usb_audio_.profile()) : "STD");
    w.flag("dup", ctx_.usb_audio_.duplex());
    w.num("ret", ctx_.usb_audio_.return_level());
    w.flag("strm", ctx_.usb_audio_.streaming());
    w.str("disk", disk.phase == hal::usb_audio::DiskPhase::Out ? "out" : (disk.phase == hal::usb_audio::DiskPhase::Working ? "working" : "idle"));
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
      const Midi::Learned took = ctx_.midi_.take_cc(m);  // a controller that learn is waiting for, or one that is bound, is the kit's
      if (took == Midi::Learned::No) {
        (void)app_.midi(ctx_, m);
      } else if (took != Midi::Learned::Moved) {
        learn_notice(took);
      }
    }
  }
}

// The strip's word for the end of a learn: which controller was bound, or that there is no room.
void Runtime::learn_notice(Midi::Learned took) noexcept {
  core::FixedString<40> text;
  if (took == Midi::Learned::Bound) {
    text.assign("LEARNED CC ");
    append_uint(text, ctx_.midi_.learned_cc());
    text.append(" CHANNEL ");
    append_uint(text, ctx_.midi_.learned_channel());
  } else {
    text.assign("LEARN: NO ROOM FOR ANOTHER BINDING");
  }
  system_strip(text.c_str(), -1, SystemConfig::kNoticeMs);
  ctx_.redraw_ = true;
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
