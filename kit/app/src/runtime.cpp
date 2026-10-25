// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/runtime.h>

#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/components.h>

namespace o3sdk::app {

namespace {

using ui::Color;
using ui::Face;
using ui::Rect;

// At most one frame this often when a screen asks for redraws (encoder turns): the display's swap paces it anyway.
constexpr core::Tick kMinFrameUs = 16'667;

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

// The render's load of the last block in percent (the header's number).
std::uint32_t cpu_percent(const dsp::EngineStats& s) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(s.cycles) * 100u / hal::audio::kCyclesPerBlock);
}

}  // namespace

Runtime::Runtime(App& app, dsp::Engine& engine, Platform& platform) noexcept : app_(app), engine_(engine), platform_(platform) {
  ctx_.engine_ = &engine_;
  ctx_.app_ = &app_;
}

bool Runtime::start() noexcept {
  ctx_.now_ = platform_.clock.now();
  ctx_.ms_ = platform_.clock.uptime_ms();
  const bool assets = ui::upload_assets(platform_.display) == hal::UploadResult::Ok;
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

void Runtime::poll() noexcept {
  ctx_.now_ = platform_.clock.now();
  ctx_.ms_ = platform_.clock.uptime_ms();

  core::InputEvent e;
  for (unsigned n = 0; n < 32u && platform_.input.poll(e); ++n) {
    ctx_.panel_.apply(e);
    const Input in = make_input(e, ctx_.panel_.shift());
    if (!app_.input(ctx_, in) && ctx_.screen_ != nullptr) {
      ctx_.screen_->input(ctx_, in);
    }
    switch_screen();
  }

  app_.update(ctx_);
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->update(ctx_);
  }
  switch_screen();

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
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->draw(ctx_, r, kContentArea);
  }
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
  if (ctx_.screen_ != nullptr) {
    r.text(8 + r.text_width(name, Face::Data) + 14, label_y, ctx_.screen_->title(), Color::ChromeDim, Face::Label);
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
  if (ctx_.screen_ != nullptr) {
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
  if (ctx_.screen_ != nullptr) {
    ctx_.screen_->leds(ctx_, leds_);
  }
  if (platform_.leds != nullptr) {
    for (unsigned i = 0; i < kPadCount; ++i) {
      platform_.leds->set(i, leds_.get(i));
    }
  }
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
  self->engine_.render(in_l, in_r, out_l, out_r, n);
  if ((io.flags & hal::audio::kFlagSilence) == 0u) {
    dsp::planes_to_s16(out_l, out_r, io.out, n);
  }
}

}  // namespace o3sdk::app
