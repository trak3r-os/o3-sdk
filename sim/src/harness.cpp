// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/sim/harness.h>

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/host/png.h>
#include <o3sdk/ui/boot_screen.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace o3sdk::sim {

namespace {

// The drawing of the pads under the screen.
constexpr int kGap = 16;
constexpr int kPadW = 56;
constexpr int kPadH = 40;
constexpr int kPadGap = 9;
constexpr int kPadsW = static_cast<int>(app::kPadCols) * kPadW + static_cast<int>(app::kPadCols - 1u) * kPadGap;
constexpr int kPadsH = static_cast<int>(app::kPadRows) * kPadH + static_cast<int>(app::kPadRows - 1u) * kPadGap;
constexpr int kMarginX = (EveRaster::kWidth - kPadsW) / 2;
constexpr int kImageH = EveRaster::kHeight + kGap + kPadsH + kMarginX;

struct Rgb {
  int r, g, b;
};
constexpr Rgb kBackground{28, 28, 32};
constexpr Rgb kPadOff{58, 58, 64};
constexpr Rgb kPadLit{255, 238, 205};  // the LEDs are monochrome: a warm white at full level
constexpr Rgb kHeld{90, 170, 255};

void fill(std::vector<std::uint8_t>& px, int w, int x0, int y0, int x1, int y1, Rgb c) {
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      std::uint8_t* p = &px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u];
      p[0] = static_cast<std::uint8_t>(c.r);
      p[1] = static_cast<std::uint8_t>(c.g);
      p[2] = static_cast<std::uint8_t>(c.b);
      p[3] = 255;
    }
  }
}

Rgb mix(Rgb a, Rgb b, int level) {
  return Rgb{a.r + (b.r - a.r) * level / 255, a.g + (b.g - a.g) * level / 255, a.b + (b.b - a.b) * level / 255};
}

void put_u32(std::FILE* f, std::uint32_t v) {
  const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
                              static_cast<unsigned char>(v >> 24)};
  std::fwrite(b, 1, 4, f);
}

void put_u16(std::FILE* f, std::uint16_t v) {
  const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
  std::fwrite(b, 1, 2, f);
}

}  // namespace

Harness::Harness(app::App& app, hal::firmware::FirmwareSink* firmware, platform::host::FakeMidi* midi)
    : app_(app),
      midi_(midi),
      raster_(std::make_unique<EveRaster>()),
      display_(std::make_unique<RasterDisplay>(*raster_)),
      engine_(std::make_unique<dsp::Engine>()) {
  platform_ = std::make_unique<app::Platform>(app::Platform{*display_, input_, &leds_, clock_, false, firmware, "sim", midi});
  runtime_ = std::make_unique<app::Runtime>(app_, *engine_, *platform_);
  clock_.set(1000);  // not zero: a tick of 0 is a legal time, but starting elsewhere catches code that treats it as "never"
}

Harness::~Harness() = default;

bool Harness::start() {
  const bool ok = runtime_->start();
  runtime_->poll();
  const std::string refused = setup_report();
  if (!refused.empty()) {
    std::fputs(refused.c_str(), stderr);
  }
  return ok && refused.empty();
}

std::string Harness::setup_report() const {
  std::string text;
  const dsp::Engine& e = *engine_;
  for (unsigned i = 0; i < e.setup_errors(); ++i) {
    const dsp::SetupError* err = e.setup_error(i);
    if (err == nullptr) {
      break;  // the rest are counted, not kept
    }
    char args[64] = "";
    const unsigned unit = err->unit;
    switch (err->call) {
      case dsp::SetupCall::Attach:
      case dsp::SetupCall::AddHook:
        break;
      case dsp::SetupCall::SetSource:
        std::snprintf(args, sizeof args, "slot %u, unit %u", unsigned{err->slot}, unit);
        break;
      case dsp::SetupCall::SetInsert:
        std::snprintf(args, sizeof args, "slot %u, insert %u, unit %u", unsigned{err->slot}, unsigned{err->index}, unit);
        break;
      case dsp::SetupCall::SetSendEffect:
        std::snprintf(args, sizeof args, "bus %u, unit %u", unsigned{err->slot}, unit);
        break;
      case dsp::SetupCall::SetMasterInsert:
        std::snprintf(args, sizeof args, "insert %u, unit %u", unsigned{err->index}, unit);
        break;
    }
    char line[192];
    std::snprintf(line, sizeof line, "setup refused: %s(%s): %s%s\n", dsp::Engine::setup_call_name(err->call), args,
                  dsp::Engine::setup_reason_name(err->reason),
                  unit == dsp::kNoUnit && err->reason == dsp::SetupReason::UnknownUnit ? " (255 is kNoUnit: an attach was refused before)" : "");
    text += line;
  }
  if (e.setup_errors() > dsp::kMaxSetupErrors) {
    text += "setup refused: " + std::to_string(e.setup_errors() - dsp::kMaxSetupErrors) + " more\n";
  }
  return text;
}

void Harness::push(core::InputKind kind, std::uint8_t id, std::int16_t value) {
  input_.push(core::InputEvent{clock_.now(), kind, id, value});
}

void Harness::press(app::Button b) { push(core::InputKind::Button, static_cast<std::uint8_t>(b), 1); }
void Harness::release(app::Button b) { push(core::InputKind::Button, static_cast<std::uint8_t>(b), 0); }
void Harness::pad_down(unsigned pad) { push(core::InputKind::Pad, static_cast<std::uint8_t>(pad), 127); }
void Harness::pad_up(unsigned pad) { push(core::InputKind::Pad, static_cast<std::uint8_t>(pad), 0); }
void Harness::turn(int detents) { push(core::InputKind::Encoder, app::kEncoder, static_cast<std::int16_t>(detents)); }

// Samples due by now at the engine's rate: uptime x rate, from the one rate constant (exact integers).
std::uint64_t Harness::due_samples() const {
  const std::uint64_t us = clock_.uptime_us();
  return us * core::kCpuHz / 1000000u * core::kRefCyclesPerSample.den / core::kRefCyclesPerSample.num;
}

void Harness::render_due() {
  const std::uint64_t due = due_samples();
  const hal::audio::BlockCallback cb = runtime_->block_callback();
  std::int16_t in[hal::audio::kBlockSamples] = {};
  std::int16_t out[hal::audio::kBlockSamples];
  hal::audio::BlockReport report{};
  while (samples_ + hal::audio::kBlock <= due) {
    const hal::audio::BlockIo io{in, out, 0u, &report};
    cb.fn(cb.user, io);
    samples_ += hal::audio::kBlock;
    if (recording_) {
      audio_.insert(audio_.end(), out, out + hal::audio::kBlockSamples);
    }
  }
}

void Harness::run_ms(std::uint32_t ms) {
  for (std::uint32_t i = 0; i < ms; ++i) {
    clock_.advance(core::kTicksPerMs);
    render_due();
    runtime_->poll();
  }
}

std::vector<std::uint8_t> Harness::panel_image(int& width, int& height) const {
  width = EveRaster::kWidth;
  height = kImageH;
  std::vector<std::uint8_t> px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
  fill(px, width, 0, 0, width, height, kBackground);
  const std::uint8_t* screen = raster_->rgba();
  for (std::size_t i = 0; i < EveRaster::kFramebufferBytes; ++i) {
    px[i] = screen[i];
  }
  const app::Panel& panel = runtime_->context().panel();
  for (unsigned pad = 0; pad < app::kPadCount; ++pad) {
    const int x = kMarginX + static_cast<int>(app::pad_col(pad)) * (kPadW + kPadGap);
    const int y = EveRaster::kHeight + kGap + static_cast<int>(app::pad_row(pad)) * (kPadH + kPadGap);
    if (panel.pad_held(pad)) {
      fill(px, width, x - 3, y - 3, x + kPadW + 3, y + kPadH + 3, kHeld);
    }
    fill(px, width, x, y, x + kPadW, y + kPadH, mix(kPadOff, kPadLit, leds_.level(pad)));
  }
  return px;
}

bool Harness::show_boot_screen() { return ui::show_boot_screen(*display_, app_.info().name, "o3sdk simulator"); }

bool Harness::snapshot(const std::string& path) const {
  int w = 0;
  int h = 0;
  const std::vector<std::uint8_t> px = panel_image(w, h);
  return platform::host::write_png_rgba(path.c_str(), px.data(), w, h);
}

bool Harness::write_wav(const std::string& path) const {
  const auto rate = static_cast<std::uint32_t>(core::kRefRateHz + 0.5f);
  return write_wav_s16(path, audio_, rate);
}

namespace {
// A check takes this many passes of the main loop (a millisecond each in the simulator): about what 2 KiB a pass makes of a 600 KB image.
constexpr unsigned kCheckSlices = 300;
}  // namespace

bool load_firmware_dir(platform::host::FakeFirmwareSink& fake, const std::string& dir, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    error = "not a directory: " + dir;
    return false;
  }
  std::vector<std::pair<std::string, std::uint32_t>> files;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".ptf") {
      std::uint64_t size = e.file_size(ec);
      // A fixture is a few bytes: when the whole file is one decimal number, that is the size the list shows (a .ptf is hundreds of KB).
      std::ifstream in(e.path());
      std::string text;
      std::getline(in, text);
      while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
      }
      if (!text.empty() && text.size() <= 9 && text.find_first_not_of("0123456789") == std::string::npos) {
        size = std::stoul(text);
      }
      files.emplace_back(e.path().filename().string(), static_cast<std::uint32_t>(size));
    }
  }
  std::sort(files.begin(), files.end());
  fake.clear_files();
  for (const auto& f : files) {
    fake.add(f.first, f.second);
    platform::host::FakeFirmwareSink::Script s;
    s.slices = kCheckSlices;
    if (f.first.find("bad") != std::string::npos) {
      s.verdict = hal::firmware::CheckStatus::State::Failed;
      s.offset = 1024;
    }
    fake.script(f.first, s);
  }
  return true;
}

bool write_wav_s16(const std::string& path, const std::vector<std::int16_t>& lr, std::uint32_t rate) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return false;
  }
  const auto data = static_cast<std::uint32_t>(lr.size() * sizeof(std::int16_t));
  std::fwrite("RIFF", 1, 4, f);
  put_u32(f, 36u + data);
  std::fwrite("WAVEfmt ", 1, 8, f);
  put_u32(f, 16u);
  put_u16(f, 1u);  // PCM
  put_u16(f, 2u);  // stereo
  put_u32(f, rate);
  put_u32(f, rate * 4u);
  put_u16(f, 4u);
  put_u16(f, 16u);
  std::fwrite("data", 1, 4, f);
  put_u32(f, data);
  for (const std::int16_t s : lr) {
    put_u16(f, static_cast<std::uint16_t>(s));
  }
  return std::fclose(f) == 0;
}

}  // namespace o3sdk::sim
