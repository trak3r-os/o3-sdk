// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/units/delay/delay.h>
#include <o3sdk/units/synth/synth.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace o3sdk;

namespace {

void check_table(const dsp::UnitInfo& info) {
  INFO(info.name);
  for (unsigned p = 0; p < info.param_count; ++p) {
    INFO(info.params[p].name);
    CHECK(core::param_desc_valid(info.params[p]));
  }
}

float rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double s = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  }
  return static_cast<float>(std::sqrt(s / static_cast<double>(to - from)));
}

}  // namespace

TEST_CASE("units: every parameter table is valid") {
  units::Synth synth;
  static float l[16];
  static float r[16];
  units::Delay delay(l, r, 16);
  check_table(synth.info());
  check_table(delay.info());
  CHECK(synth.info().kind == dsp::UnitKind::Instrument);
  CHECK(delay.info().kind == dsp::UnitKind::Effect);
}

TEST_CASE("synth: a note sounds, every wave stays finite and bounded, and the release ends it") {
  for (int wave = 0; wave < 4; ++wave) {
    INFO("wave " << wave);
    dsp::Engine e;
    units::Synth synth;
    const dsp::UnitId id = e.attach(synth);
    REQUIRE(e.set_source(0, id));
    (void)e.set_param(id, units::Synth::Wave, static_cast<float>(wave));
    (void)e.set_param(id, units::Synth::Release, 20.0f);
    e.start();
    (void)e.note_on(id, 57, 127);
    std::vector<float> out;
    float l[64];
    float r[64];
    for (int b = 0; b < 200; ++b) {
      if (b == 100) {
        (void)e.note_off(id, 57);
      }
      e.render(nullptr, nullptr, l, r, 64);
      out.insert(out.end(), l, l + 64);
    }
    for (const float s : out) {
      REQUIRE(std::isfinite(s));
      REQUIRE(std::fabs(s) < 1.0f);
    }
    CHECK(rms(out, 2000, 6000) > 0.02f);
    CHECK(synth.active_voices() == 0u);  // 20 ms of release, 100 blocks later
    CHECK(rms(out, out.size() - 1000, out.size()) < 1e-4f);
  }
}

TEST_CASE("synth: more notes than voices steal the oldest") {
  dsp::Engine e;
  units::Synth synth;
  const dsp::UnitId id = e.attach(synth);
  REQUIRE(e.set_source(0, id));
  e.start();
  for (std::uint8_t n = 40; n < 40 + 12; ++n) {
    (void)e.note_on(id, n, 100);
  }
  float l[64];
  float r[64];
  e.render(nullptr, nullptr, l, r, 64);
  CHECK(synth.active_voices() == units::Synth::kVoices);
}

TEST_CASE("delay: an impulse comes back after the note value at the tempo, quieter each time") {
  constexpr std::uint32_t kCap = 44'100;
  static float dl[kCap];
  static float dr[kCap];
  dsp::Engine e;
  units::Delay delay(dl, dr, kCap);
  const dsp::UnitId id = e.attach(delay);
  (void)e.set_param(id, units::Delay::Time, 1.0f);  // 1/8
  (void)e.set_param(id, units::Delay::Mode, 0.0f);  // mono
  (void)e.set_param(id, units::Delay::Tone, 12000.0f);
  e.start();
  // Drive the unit directly, as the engine would for a send: one impulse, then silence.
  dsp::Transport t;
  t.tempo_mbpm = 120'000;
  t.frames = 64;
  std::vector<float> out;
  float in_l[64] = {};
  float in_r[64] = {};
  float ol[64];
  float orr[64];
  for (int b = 0; b < 400; ++b) {
    in_l[0] = b == 0 ? 1.0f : 0.0f;
    in_r[0] = in_l[0];
    dsp::ProcessArgs a{nullptr, 0, in_l, in_r, ol, orr, 64, &t};
    delay.process(a);
    out.insert(out.end(), ol, ol + 64);
  }
  const auto eighth = static_cast<std::size_t>(2.0f * dsp::samples_per_sixteenth(120'000) + 0.5f);  // 11025
  std::size_t first = 0;
  float first_peak = 0.0f;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (std::fabs(out[i]) > first_peak && i < eighth + 100) {
      first_peak = std::fabs(out[i]);
      first = i;
    }
  }
  CHECK(first >= eighth - 2);
  CHECK(first <= eighth + 2);
  float second_peak = 0.0f;
  for (std::size_t i = 2 * eighth - 100; i < 2 * eighth + 100 && i < out.size(); ++i) {
    second_peak = std::fabs(out[i]) > second_peak ? std::fabs(out[i]) : second_peak;
  }
  CHECK(second_peak > 0.1f);
  CHECK(second_peak < first_peak);
}
