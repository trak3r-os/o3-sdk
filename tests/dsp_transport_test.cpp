// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/dsp/transport.h>

#include <cstdint>
#include <vector>

using namespace o3sdk;

namespace {

// Steps of a grid seen by a run of `blocks` blocks of `frames`, as absolute sample positions.
std::vector<std::uint64_t> walk(std::uint32_t mbpm, std::uint32_t period, std::uint32_t offset, std::uint32_t swing, unsigned blocks,
                                std::uint32_t frames, std::vector<std::uint64_t>* indices = nullptr) {
  dsp::Transport t;
  t.playing = true;
  t.tempo_mbpm = mbpm;
  t.inc = dsp::ticks_per_sample_q32(mbpm);
  t.frames = frames;
  std::vector<std::uint64_t> at;
  for (unsigned b = 0; b < blocks; ++b) {
    dsp::for_each_step(t, period, offset, swing, [&](std::uint32_t frame, std::uint64_t k) {
      at.push_back(static_cast<std::uint64_t>(b) * frames + frame);
      if (indices != nullptr) {
        indices->push_back(k);
      }
    });
    t.pos += t.inc * frames;
  }
  return at;
}

}  // namespace

TEST_CASE("transport: a minute at 120 BPM is 120 quarters to the tick") {
  const std::uint64_t inc = dsp::ticks_per_sample_q32(120'000);
  const auto rate = static_cast<std::uint64_t>(core::kRefRateHz + 0.5f);
  const std::uint64_t pos = inc * rate * 60u;
  const std::uint64_t ticks = pos >> 32;
  CHECK(ticks >= 120u * dsp::kPpqn - 1u);
  CHECK(ticks <= 120u * dsp::kPpqn);
}

TEST_CASE("transport: tempo is clamped to 20..300 BPM") {
  CHECK(dsp::ticks_per_sample_q32(1'000) == dsp::ticks_per_sample_q32(dsp::kMinTempoMilliBpm));
  CHECK(dsp::ticks_per_sample_q32(900'000) == dsp::ticks_per_sample_q32(dsp::kMaxTempoMilliBpm));
  CHECK(dsp::ticks_per_sample_q32(240'000) > dsp::ticks_per_sample_q32(120'000));
}

TEST_CASE("transport: frame_at finds a tick inside the block, and nothing outside it") {
  dsp::Transport t;
  t.playing = true;
  t.inc = dsp::ticks_per_sample_q32(120'000);
  t.frames = 64;
  t.pos = 0;
  CHECK(dsp::frame_at(t, 0) == 0);
  const std::uint64_t last = (t.end() - 1u) >> 32;
  CHECK(dsp::frame_at(t, last) >= 0);
  CHECK(dsp::frame_at(t, last + 1u) == -1);
  t.playing = false;
  CHECK(dsp::frame_at(t, 0) == -1);
}

TEST_CASE("transport: sixteenths at 120 BPM land every 5512.5 samples, each exactly once, across block edges") {
  std::vector<std::uint64_t> k;
  const auto at = walk(120'000, dsp::kTicksPerSixteenth, 0, 0, 2000, 64, &k);
  REQUIRE(at.size() >= 23u);
  for (std::size_t i = 0; i < at.size(); ++i) {
    CHECK(k[i] == i);  // in order, none skipped, none twice
    const double expected = static_cast<double>(i) * 5512.5;
    CHECK(static_cast<double>(at[i]) >= expected - 1.0);
    CHECK(static_cast<double>(at[i]) <= expected + 1.0);
  }
}

TEST_CASE("transport: the grid does not depend on the block size") {
  const auto a = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 4000, 64);
  const auto b = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 8000, 32);
  const auto c = walk(133'000, dsp::kTicksPerSixteenth, 0, 0, 256000, 1);
  REQUIRE(a.size() >= 30u);
  for (std::size_t i = 0; i < 30; ++i) {
    CHECK(a[i] == b[i]);
    CHECK(a[i] == c[i]);
  }
}

TEST_CASE("transport: swing delays the odd steps only, by its ticks; an offset moves every step") {
  const auto straight = walk(120'000, dsp::kTicksPerSixteenth, 0, 0, 1000, 64);
  const auto swung = walk(120'000, dsp::kTicksPerSixteenth, 0, 80, 1000, 64);
  const auto late = walk(120'000, dsp::kTicksPerSixteenth, 120, 0, 1000, 64);
  REQUIRE(straight.size() >= 8u);
  REQUIRE(swung.size() >= 8u);
  REQUIRE(late.size() >= 8u);
  const double per_tick = 5512.5 / 240.0;
  for (std::size_t i = 0; i < 8; ++i) {
    const double shift = (i % 2u == 1u) ? 80.0 * per_tick : 0.0;
    CHECK(static_cast<double>(swung[i]) == doctest::Approx(static_cast<double>(straight[i]) + shift).epsilon(0.0005));
    CHECK(static_cast<double>(late[i]) == doctest::Approx(static_cast<double>(straight[i]) + 120.0 * per_tick).epsilon(0.0005));
  }
}

TEST_CASE("transport: samples_per_sixteenth matches the grid") {
  CHECK(dsp::samples_per_sixteenth(120'000) == doctest::Approx(5512.5f));
  CHECK(dsp::samples_per_sixteenth(60'000) == doctest::Approx(11025.0f));
}
