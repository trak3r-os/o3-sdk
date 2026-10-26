// SPDX-License-Identifier: GPL-3.0-or-later
// A unit is tested against the engine, as o3-sdk's tests/units_test.cpp does for its units: attach it, route it to a slot, start, send
// notes and parameters the way a screen does, render blocks and look at the samples.
#include <doctest/doctest.h>

#include <@APP_ID@/units/voice/voice.h>

#include <o3sdk/dsp/engine.h>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace o3sdk;

namespace {

float rms(const std::vector<float>& v, std::size_t from, std::size_t to) {
  double s = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
  }
  return static_cast<float>(std::sqrt(s / static_cast<double>(to - from)));
}

// `blocks` blocks of 64 frames from slot 0 (the left channel); `at_block` runs once before each block (a note, a parameter).
template <typename Before>
std::vector<float> render(dsp::Engine& e, int blocks, Before&& at_block) {
  std::vector<float> out;
  float l[64];
  float r[64];
  for (int b = 0; b < blocks; ++b) {
    at_block(b);
    e.render(nullptr, nullptr, l, r, 64);
    out.insert(out.end(), l, l + 64);
  }
  return out;
}

}  // namespace

TEST_CASE("voice: the parameter table is valid") {
  @APP_ID@::Voice voice;
  const dsp::UnitInfo& info = voice.info();
  CHECK(info.kind == dsp::UnitKind::Instrument);
  REQUIRE(info.param_count == @APP_ID@::Voice::kParamCount);
  for (unsigned p = 0; p < info.param_count; ++p) {
    INFO(info.params[p].name);
    CHECK(core::param_desc_valid(info.params[p]));
  }
}

TEST_CASE("voice: a note sounds on every wave, stays finite and bounded, and the key up ends it") {
  for (int wave = 0; wave < 3; ++wave) {
    INFO("wave " << wave);
    dsp::Engine e;
    @APP_ID@::Voice voice;
    const dsp::UnitId id = e.attach(voice);
    REQUIRE(e.set_source(0, id));
    (void)e.set_param(id, @APP_ID@::Voice::Wave, static_cast<float>(wave));
    (void)e.set_param(id, @APP_ID@::Voice::Decay, 4000.0f);  // long: the note is still sounding when the key goes up
    e.start();
    const std::vector<float> out = render(e, 200, [&](int b) {
      if (b == 0) {
        (void)e.note_on(id, 57, 127);
      }
      if (b == 100) {
        (void)e.note_off(id, 57);
      }
    });
    for (const float s : out) {
      REQUIRE(std::isfinite(s));
      REQUIRE(std::fabs(s) < 1.0f);
    }
    CHECK(rms(out, 2000, 6000) > 0.02f);                       // sounding
    CHECK_FALSE(voice.sounding());                              // 100 blocks after the key up: the release is over
    CHECK(rms(out, out.size() - 1000, out.size()) < 1e-4f);    // and silent
  }
}

TEST_CASE("voice: the decay ends a held note by itself") {
  dsp::Engine e;
  @APP_ID@::Voice voice;
  const dsp::UnitId id = e.attach(voice);
  REQUIRE(e.set_source(0, id));
  (void)e.set_param(id, @APP_ID@::Voice::Decay, 100.0f);
  e.start();
  const std::vector<float> out = render(e, 300, [&](int b) {
    if (b == 0) {
      (void)e.note_on(id, 60, 100);  // and no note off
    }
  });
  CHECK(rms(out, 500, 1500) > 0.02f);
  CHECK(rms(out, out.size() - 1000, out.size()) < 1e-3f);
  CHECK_FALSE(voice.sounding());
}

namespace {

// A held sine at A2 for 400 blocks with the level set to `level` db before the start and, when `change_to` is not NaN, moved to it at
// block 200 (the way the encoder does it: a Param event at frame 0 of a block).
std::vector<float> held_note(float level_db, float change_to) {
  dsp::Engine e;
  @APP_ID@::Voice voice;
  const dsp::UnitId id = e.attach(voice);
  REQUIRE(e.set_source(0, id));
  (void)e.set_param(id, @APP_ID@::Voice::Wave, 0.0f);
  (void)e.set_param(id, @APP_ID@::Voice::Decay, 4000.0f);
  (void)e.set_param(id, @APP_ID@::Voice::Level, level_db);
  e.start();
  return render(e, 400, [&](int b) {
    if (b == 0) {
      (void)e.note_on(id, 45, 127);
    }
    if (b == 200 && !std::isnan(change_to)) {
      (void)e.set_param(id, @APP_ID@::Voice::Level, change_to);
    }
  });
}

}  // namespace

TEST_CASE("voice: the level parameter scales the output by its dB") {
  const float nan = std::nanf("");
  const std::vector<float> loud = held_note(-12.0f, nan);
  const std::vector<float> quiet = held_note(-24.0f, nan);
  const float a = rms(loud, 64 * 100, 64 * 190);
  const float b = rms(quiet, 64 * 100, 64 * 190);
  REQUIRE(a > 0.01f);
  CHECK(b / a == doctest::Approx(0.2512f).epsilon(0.01));  // -12 dB in amplitude
}

TEST_CASE("voice: a level change in the middle of a note ramps instead of clicking") {
  const float nan = std::nanf("");
  const std::vector<float> steady = held_note(-12.0f, nan);
  const std::vector<float> moved = held_note(-12.0f, -24.0f);
  // What the change added to the signal, sample by sample: a jump of the gain would show as a step in it, a ramp (5 ms) as a slope.
  float worst = 0.0f;
  for (std::size_t i = 64 * 200 - 1; i < 64 * 204; ++i) {  // from the sample before the event: a jump is between it and the next
    const float d0 = moved[i] - steady[i];
    const float d1 = moved[i + 1] - steady[i + 1];
    worst = std::fmax(worst, std::fabs(d1 - d0));
  }
  CHECK(worst < 0.005f);  // the 5 ms ramp gives about 0.001; an instant 12 dB step (a ramp of one sample) gives 0.03
  CHECK(rms(moved, 64 * 300, 64 * 390) < rms(steady, 64 * 300, 64 * 390));  // and the change did arrive
}
