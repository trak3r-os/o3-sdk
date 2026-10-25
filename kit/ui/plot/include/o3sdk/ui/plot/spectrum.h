// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The spectrum analyser (ADR 0010 amendment 2): a windowed real FFT of 256 or 512 points (fft.h), folded into up to
// kMaxBars logarithmically spaced bars in dB, smoothed (a fast attack, a slow release) with a held peak per bar, drawn as
// bars with a cap or as a translucent fill under a line. It is fed one block of recent frames at a time from the main loop
// (the audio -> main ring, ADR 0010: the plot never touches the audio buffers) at the 20 Hz tier; the analysis is about a
// tenth of a millisecond, bars and caps cost 2 display-list words each.
//
//   bar b covers [f_lo * r^(b/B), f_lo * r^((b+1)/B)), r = f_hi / f_lo; it shows the loudest FFT bin it covers, or, when it
//   is narrower than a bin (the lowest ones), the power interpolated between the two bins around its centre, so the low end
//   does not turn into steps. Levels are dB relative to a full-scale sine (0 dB): the Hann window's coherent gain is divided
//   out, so a sine at amplitude 1.0 reads 0 dB on the bin it falls in. The lane of the plot is [floor_db, ceil_db].
//
// The object is 12.1 KB (the window, the FFT's tables and work, and two arrays, all sized for 512 points, and the bars); no heap.
// Main loop only.

#include <o3sdk/ui/plot/fft.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::ui::plot {

class Spectrum {
 public:
  static constexpr unsigned kMaxBars = 64;

  // The width of an FFT bin, Hz: below it the transform has nothing to say (bin 0 is DC and bin 1 is the first frequency), so
  // a bar cannot start lower: Config::f_lo is at least this and init() refuses less.
  static constexpr float bin_hz(unsigned fft_size, float sample_rate) noexcept {
    return sample_rate / static_cast<float>(fft_size);
  }

  struct Config {
    unsigned fft_size = 512;  // 256 or 512 (any power of two RealFft takes)
    float sample_rate = 44100.0f;
    unsigned bars = 48;       // 1 .. kMaxBars
    float f_lo = 44100.0f / 512.0f;  // one bin of the defaults (86.13 Hz); with another size or rate set it to bin_hz() or more
    float f_hi = 16000.0f;
    float floor_db = -72.0f;
    float ceil_db = 0.0f;
  };

  // Generates the window and the bar-to-bin map. false (and unusable) for an invalid configuration: a size the FFT does not
  // take, no bars, f_lo below one bin (bin_hz()), f_lo >= f_hi or f_hi above Nyquist, ceil_db <= floor_db.
  bool init(const Config& config) noexcept;
  bool ready() const noexcept { return ready_; }
  const Config& config() const noexcept { return cfg_; }
  unsigned bars() const noexcept { return cfg_.bars; }

  // Analyses `fft_size` frames: `frames` is mono, or interleaved stereo (channels 2: the two channels are averaged).
  // Updates every bar's level, smoothing and peak hold; call it once per update tick (20 Hz).
  void analyze(const std::int16_t* frames, unsigned channels) noexcept;
  // The levels without smoothing, as the last analyze() measured them (for tests and for a caller that wants its own).
  float raw_db(unsigned bar) const noexcept { return (bar < cfg_.bars && bar < kMaxBars) ? raw_[bar] : cfg_.floor_db; }
  float level_db(unsigned bar) const noexcept { return (bar < cfg_.bars && bar < kMaxBars) ? level_[bar] : cfg_.floor_db; }
  float peak_db(unsigned bar) const noexcept { return (bar < cfg_.bars && bar < kMaxBars) ? peak_[bar] : cfg_.floor_db; }
  // The centre frequency of a bar, Hz.
  float bar_center_hz(unsigned bar) const noexcept;
  // Forgets the levels and peaks.
  void reset() noexcept;

  // The smoothing and the hold, per analyze() call.
  static constexpr float kAttack = 0.6f;    // share of the way to a louder level that one call covers
  static constexpr float kRelease = 0.18f;  // ... to a quieter one
  static constexpr unsigned kHoldCalls = 20;    // a peak is held this many calls (1 s at 20 Hz) ...
  static constexpr float kPeakFallDb = 1.0f;    // ... and then falls this many dB a call

 private:
  Config cfg_{};
  bool ready_ = false;
  RealFft fft_;
  float window_[RealFft::kMaxN] = {};
  float norm_ = 0.0f;                      // (2 / sum of the window / 32768)^2: power of a full-scale sine -> 1.0
  float buf_[RealFft::kMaxN] = {};
  float out_[RealFft::kMaxN] = {};
  std::uint16_t bin_lo_[kMaxBars] = {};    // first FFT bin of a bar ...
  std::uint16_t bin_hi_[kMaxBars] = {};    // ... one past the last (bin_hi - bin_lo == 1: a narrow bar, interpolated)
  float frac_[kMaxBars] = {};              // narrow bars: where between bin_lo and bin_lo + 1 the centre lies
  float raw_[kMaxBars] = {};
  float level_[kMaxBars] = {};
  float peak_[kMaxBars] = {};
  std::uint8_t hold_[kMaxBars] = {};
};

enum class SpectrumShape : std::uint8_t { Bars, Fill };

struct SpectrumStyle {
  Color bar = Color::Instr;
  Color cap = Color::Text;
  SpectrumShape shape = SpectrumShape::Bars;
  bool caps = true;
  std::uint8_t fill_alpha = 0x50;
};

// Draws the bars in `area` (bars side by side with a one-pixel gap, from the bottom up; or, as a Fill, a translucent area
// under the line through the bar tops), with the held peaks as two-pixel caps. A bar at the floor draws nothing.
void draw_spectrum(Renderer& r, const Rect& area, const Spectrum& spectrum, const SpectrumStyle& style) noexcept;

// Words draw_spectrum adds at most, hard (any style; the renderer's origin moved and a primitive open): a Bars plot is a colour,
// the BEGIN, 2 words a bar, a colour and 2 words a cap; a Fill is a raw section of 4 words with its scissor (2), two strips (a
// COLOR_A, a colour, BEGIN, a vertex a bar and END each, the line's strip with a LINE_WIDTH too) and then the caps as a Bars plot's
// (a colour, BEGIN, 2 a bar): 4 * bars + 17.
constexpr unsigned spectrum_max_words(unsigned bars) noexcept { return 4u * bars + 17u; }

}  // namespace o3sdk::ui::plot
