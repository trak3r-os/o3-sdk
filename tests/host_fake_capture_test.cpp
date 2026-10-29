// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fake codec input and FM radio (platform/host/fake_capture.h, docs/M2C-PLAN.md section 5): what the "codec hears" through the source and the gain, the receiver's states and times, its
// seek over the listed stations, an absent chip, held requests, and refused writes. The fake is also the reference model of hal/capture.h, so these are the contract the kit's AudioIn and
// Radio are tested against (app_audio_in_test.cpp).
#include <doctest/doctest.h>

#include <o3sdk/platform/host/fake_capture.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace o3sdk;
using host_t = platform::host::FakeCapture;
using hal::capture::Region;
using hal::capture::Source;
using State = hal::capture::RadioStatus::State;

namespace {

constexpr unsigned kFrames = 4410;  // 100 ms

// The peak of the left channel over a block of the fake's input.
float peak_of(host_t& cap, unsigned frames = kFrames) {
  std::vector<std::int16_t> lr(2u * frames);
  cap.read_block(lr.data(), frames);
  float peak = 0.0f;
  for (unsigned i = 0; i < frames; ++i) {
    peak = std::fmax(peak, std::fabs(static_cast<float>(lr[2u * i])) / 32768.0f);
  }
  return peak;
}

// Steps the fake's service once a millisecond.
void run(host_t& cap, std::uint64_t& now, unsigned ms) {
  for (unsigned i = 0; i < ms; ++i) {
    cap.service(++now);
  }
}

}  // namespace

TEST_CASE("fake capture: silent until a world is given, then the tone through the line at the gain's dB") {
  host_t cap;
  CHECK(peak_of(cap) == 0.0f);
  cap.input_tone(1000.0f, -40.0f);
  CHECK(peak_of(cap) == doctest::Approx(0.01f).epsilon(0.02));  // -40 dBFS
  // The line: gain * 15 / 100 steps of 1.5 dB.
  CHECK(cap.select_source(Source::LineMix, 0));
  CHECK(peak_of(cap) == doctest::Approx(0.01f).epsilon(0.02));
  CHECK(cap.set_gain(Source::LineMix, 100));  // 15 steps: 22.5 dB
  CHECK(peak_of(cap) == doctest::Approx(0.01f * std::pow(10.0f, 22.5f / 20.0f)).epsilon(0.02));
  CHECK(cap.set_gain(Source::LineMix, 40));  // 6 steps: 9 dB
  CHECK(peak_of(cap) == doctest::Approx(0.01f * std::pow(10.0f, 9.0f / 20.0f)).epsilon(0.02));
  // The microphone: gain * 25 / 100 dB in the low range, gain * 44 / 100 in the high one.
  CHECK(cap.select_source(Source::MicLow, 100));
  CHECK(peak_of(cap) == doctest::Approx(0.01f * std::pow(10.0f, 25.0f / 20.0f)).epsilon(0.02));
  CHECK(cap.select_source(Source::MicHigh, 50));
  CHECK(peak_of(cap) == doctest::Approx(0.01f * std::pow(10.0f, 22.0f / 20.0f)).epsilon(0.02));
  // Too loud clips at 16 bits, never wraps.
  cap.input_tone(1000.0f, 0.0f);
  CHECK(cap.select_source(Source::MicHigh, 100));
  CHECK(peak_of(cap) <= 1.0f);
  CHECK(peak_of(cap) > 0.99f);
  // The USB return is not the codec's: nothing.
  CHECK(cap.select_source(Source::Usb, 0));
  CHECK(peak_of(cap) == 0.0f);
  cap.input_off();
  CHECK(cap.select_source(Source::LineMix, 0));
  CHECK(peak_of(cap) == 0.0f);
  CHECK(cap.selects() == 6u);
  CHECK(cap.gains() == 2u);
}

TEST_CASE("fake capture: a WAV is looped, stereo kept, and the tone's phase is continuous across blocks") {
  host_t cap;
  std::vector<std::int16_t> lr;
  for (int i = 0; i < 100; ++i) {
    lr.push_back(static_cast<std::int16_t>(i));
    lr.push_back(static_cast<std::int16_t>(-i));
  }
  cap.input_wav(lr);
  std::vector<std::int16_t> got(2u * 250u);
  cap.read_block(got.data(), 250);
  for (unsigned i = 0; i < 250; ++i) {
    CHECK(got[2u * i] == static_cast<std::int16_t>(i % 100u));
    CHECK(got[2u * i + 1u] == static_cast<std::int16_t>(-static_cast<int>(i % 100u)));
  }
  cap.input_tone(500.0f, -6.0f);
  std::vector<std::int16_t> a(2u * 64u);
  std::vector<std::int16_t> b(2u * 64u);
  cap.read_block(a.data(), 64);
  cap.read_block(b.data(), 64);
  // The step between the last of one block and the first of the next is no bigger than inside a block (no restart of the sine).
  int biggest = 0;
  for (unsigned i = 1; i < 64; ++i) {
    biggest = std::max(biggest, std::abs(a[2u * i] - a[2u * (i - 1u)]));
  }
  CHECK(std::abs(b[0] - a[2u * 63u]) <= biggest + 1);
}

TEST_CASE("fake capture: a WAV file is read, 16-bit PCM mono or stereo only") {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "o3sdk_fake_capture_test";
  std::filesystem::create_directories(dir);
  const auto write_wav = [&](const char* name, unsigned channels, unsigned bits, const std::vector<std::int16_t>& pcm) {
    const std::string path = (dir / name).string();
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(pcm.size() * 2u);
    out.write("RIFF", 4);
    u32(36u + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(44100);
    u32(44100u * channels * bits / 8u);
    u16(static_cast<std::uint16_t>(channels * bits / 8u));
    u16(static_cast<std::uint16_t>(bits));
    out.write("data", 4);
    u32(bytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    return path;
  };
  host_t cap;
  std::string why;
  CHECK(cap.input_wav_file(write_wav("mono.wav", 1, 16, {100, 200, 300}), why));
  std::int16_t out[6];
  cap.read_block(out, 3);
  CHECK(out[0] == 100);
  CHECK(out[1] == 100);  // mono: both channels
  CHECK(out[4] == 300);
  CHECK(cap.input_wav_file(write_wav("stereo.wav", 2, 16, {1, -1, 2, -2}), why));
  cap.read_block(out, 2);
  CHECK(out[0] == 1);
  CHECK(out[1] == -1);
  CHECK_FALSE(cap.input_wav_file(write_wav("bits8.wav", 1, 8, {1, 2}), why));
  CHECK_FALSE(why.empty());
  CHECK_FALSE(cap.input_wav_file((dir / "not_there.wav").string(), why));
  std::filesystem::remove_all(dir);
}

TEST_CASE("fake capture: the receiver powers up in 0.7 s, then listens; a power-up from Ready does nothing") {
  host_t cap;
  std::uint64_t now = 0;
  CHECK(cap.radio_status().state == State::Off);
  run(cap, now, 10);
  CHECK(cap.radio_status().state == State::Off);
  cap.radio_power(true);
  CHECK(cap.radio_status().state == State::Starting);
  run(cap, now, 600);
  CHECK(cap.radio_status().state == State::Starting);
  run(cap, now, 150);
  CHECK(cap.radio_status().state == State::Ready);
  CHECK(cap.radio_status().chip_id != 0u);
  const std::uint32_t gen = cap.radio_status().generation;
  cap.radio_power(true);
  CHECK(cap.radio_status().state == State::Ready);
  CHECK(cap.radio_status().generation == gen);
  cap.radio_power(false);
  CHECK(cap.radio_status().state == State::Off);
  CHECK(cap.radio_status().generation != gen);
}

TEST_CASE("fake capture: tune snaps to a channel, the state follows, a station is a tone at freq / 10 Hz while the source is Radio") {
  host_t cap;
  std::uint64_t now = 0;
  cap.radio_stations({9370});
  CHECK(cap.select_source(Source::Radio, 0));  // powers it up
  run(cap, now, 800);
  REQUIRE(cap.radio_status().state == State::Ready);
  CHECK(peak_of(cap) == 0.0f);  // 87.50: nothing there
  CHECK(cap.radio_status().rssi < 10u);
  cap.radio_tune(9373);
  CHECK(cap.radio_status().freq == 9370u);  // the target, shown at once
  run(cap, now, 1);
  CHECK(cap.radio_status().state == State::Tuning);
  CHECK(peak_of(cap) == 0.0f);  // not listening while it tunes
  run(cap, now, 100);
  CHECK(cap.radio_status().state == State::Ready);
  CHECK(cap.radio_status().rssi >= 30u);
  // 937 Hz: count the sign changes of a second.
  std::vector<std::int16_t> lr(2u * 44100u);
  cap.read_block(lr.data(), 44100);
  unsigned crossings = 0;
  for (unsigned i = 1; i < 44100; ++i) {
    crossings += (lr[2u * i] >= 0) != (lr[2u * (i - 1u)] >= 0) ? 1u : 0u;
  }
  CHECK(static_cast<float>(crossings) * 0.5f == doctest::Approx(937.0f).epsilon(0.01));
  CHECK(lr[2u * 100u] == lr[2u * 100u + 1u]);  // both channels
  // Another source: the radio is not heard (and the line's world is silent).
  CHECK(cap.select_source(Source::LineMix, 0));
  CHECK(peak_of(cap) == 0.0f);
}

TEST_CASE("fake capture: a seek finds the next listed station in its direction, wraps, and says when there is none") {
  host_t cap;
  std::uint64_t now = 0;
  cap.radio_stations({10110, 8850, 9370});  // (kept in order)
  cap.radio_power(true);
  run(cap, now, 800);
  REQUIRE(cap.radio_status().state == State::Ready);
  const auto seek = [&](bool up) {
    cap.radio_seek(up);
    run(cap, now, 1);
    CHECK(cap.radio_status().state == State::Seeking);
    run(cap, now, 450);
    CHECK(cap.radio_status().state == State::Ready);
    return cap.radio_status().freq;
  };
  CHECK(seek(true) == 8850u);
  CHECK(seek(true) == 9370u);
  CHECK(seek(true) == 10110u);
  CHECK(seek(true) == 8850u);  // wrapped at the top
  CHECK_FALSE(cap.radio_status().band_limit);
  CHECK(seek(false) == 10110u);  // and at the bottom
  CHECK(seek(false) == 9370u);
  // With nothing on the air: the same frequency and the band limit.
  cap.radio_stations({});
  CHECK(seek(true) == 9370u);
  CHECK(cap.radio_status().band_limit);
  // A seek or a tune asked while it is busy is held: the last one wins.
  cap.radio_stations({10110});
  const auto until = [&](State s, unsigned max_ms) {
    for (unsigned i = 0; i < max_ms && cap.radio_status().state != s; ++i) {
      run(cap, now, 1);
    }
    return cap.radio_status().state == s;
  };
  cap.radio_seek(true);
  run(cap, now, 2);
  cap.radio_tune(9000);
  cap.radio_seek(false);
  CHECK(cap.radio_status().state == State::Seeking);
  CHECK(until(State::Ready, 600));
  CHECK(cap.radio_status().freq == 10110u);  // the first seek ran its course
  CHECK(until(State::Seeking, 20));          // then the last request: seek down
  CHECK(until(State::Ready, 600));
}

TEST_CASE("fake capture: the regions change the channel grid, and the frequency is made valid for the new one") {
  host_t cap;
  std::uint64_t now = 0;
  cap.radio_power(true);
  run(cap, now, 800);
  cap.radio_tune(9380);
  run(cap, now, 100);
  cap.radio_region(Region::Usa);
  CHECK(cap.radio_status().region == Region::Usa);
  CHECK(cap.radio_status().freq == 9390u);  // 200 kHz channels from 87.50
  run(cap, now, 100);
  CHECK(cap.radio_status().state == State::Ready);
  cap.radio_region(Region::Japan);
  cap.radio_tune(7600);
  CHECK(cap.radio_status().freq == 7600u);
  cap.radio_region(static_cast<Region>(7));  // refused
  CHECK(cap.radio_status().region == Region::Japan);
}

TEST_CASE("fake capture: a board with no receiver ends in Absent after a few passes, ignores seeks and tunes, and starts again only when asked") {
  host_t cap;
  std::uint64_t now = 0;
  cap.radio_absent();
  cap.radio_power(true);
  CHECK(cap.radio_status().state == State::Starting);
  run(cap, now, 30);
  CHECK(cap.radio_status().state == State::Absent);
  CHECK(std::string(cap.radio_status().fail) == "PB/N");
  const std::uint32_t gen = cap.radio_status().generation;
  cap.radio_seek(true);
  run(cap, now, 5000);
  CHECK(cap.radio_status().state == State::Absent);
  CHECK(cap.radio_status().generation == gen);  // nothing moved by itself
  cap.radio_power(true);  // the retry
  CHECK(cap.radio_status().state == State::Starting);
  run(cap, now, 30);
  CHECK(cap.radio_status().state == State::Absent);
  cap.radio_absent(false);
  cap.radio_power(true);
  run(cap, now, 800);
  CHECK(cap.radio_status().state == State::Ready);
  CHECK(std::string(cap.radio_status().fail).empty());
}

TEST_CASE("fake capture: refuse_writes makes the next calls answer false and leaves the codec where it was") {
  host_t cap;
  REQUIRE(cap.select_source(Source::LineMix, 10));
  cap.refuse_writes(2);
  CHECK_FALSE(cap.select_source(Source::MicHigh, 90));
  CHECK_FALSE(cap.set_gain(Source::LineMix, 50));
  CHECK(cap.source() == Source::LineMix);
  CHECK(cap.gain() == 10u);
  CHECK(cap.refused() == 2u);
  CHECK(cap.select_source(Source::MicHigh, 90));
  CHECK(cap.source() == Source::MicHigh);
  CHECK(cap.gain() == 90u);
  // A gain for a source that is not the one in use does not move the codec.
  CHECK(cap.set_gain(Source::MicLow, 5));
  CHECK(cap.gain() == 90u);
}
