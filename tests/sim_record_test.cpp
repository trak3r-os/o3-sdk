// SPDX-License-Identifier: GPL-3.0-or-later
//
// Recording on the hello app, end to end in the simulator (docs/M2C-PLAN.md section 6, slice RC): the unit's record service (platform/mk66/audio) runs over the fake codec, so what is
// proved here is the unit's own code and arithmetic. The scenario apps/hello/scenarios/record.txt runs as it is and the WAVs it leaves on the card read back as what was heard; a take on
// a step of the riff's transport starts at the sample of that sixteenth (the codec's input is a ramp whose value is the sample's own index, so one sample off is a failure); a level
// take keeps its pre-roll; a full buffer is counted; a save puts the take on the card as a WAV that reads back equal; an idle recorder does no work, and a recorder that records does not
// change a sample of what the main output plays. The codec's real lag and the cost of a take on the unit are the unit's.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/audio_in.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/recorder.h>
#include <o3sdk/dsp/transport.h>
#include <o3sdk/hal/record.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/platform/mk66/audio/record_service.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace o3sdk;
namespace host = o3sdk::platform::host;
namespace hr = o3sdk::hal::record;

namespace {

using Pcm = std::vector<std::int16_t>;

constexpr std::uint32_t kLoop = 30000;
// A ramp the codec "hears": L = R = (n mod 30000) + 1, where n counts the samples since the harness began (the wav is played in a loop from its first sample when it is given). Never
// zero, so a take that missed its start shows.
Pcm ramp_wav() {
  Pcm lr(2u * kLoop);
  for (std::uint32_t i = 0; i < kLoop; ++i) {
    lr[2u * i] = lr[2u * i + 1u] = static_cast<std::int16_t>(i + 1u);
  }
  return lr;
}
std::int16_t ramp_at(std::uint64_t n) { return static_cast<std::int16_t>(n % kLoop + 1u); }

// Silence for `kSilent` samples, then a rising signal of at least 2,000: a level of 1,000 is crossed at exactly sample kSilent of the loop.
constexpr std::uint32_t kLevelLoop = 40000;
constexpr std::uint32_t kSilent = 15000;
Pcm level_wav() {
  Pcm lr(2u * kLevelLoop);
  for (std::uint32_t i = kSilent; i < kLevelLoop; ++i) {
    lr[2u * i] = lr[2u * i + 1u] = static_cast<std::int16_t>(2000u + (i - kSilent));
  }
  return lr;
}

struct Rig {
  host::ScratchDir card_dir;
  host::DirCard card;
  host::FakeCapture capture;
  sim::Harness h;
  explicit Rig(const char* tag) : card_dir(tag), card(card_dir.path()), h(o3sdk_app(), nullptr, nullptr, &card, nullptr, &capture) {}
  app::Recorder& rec() { return h.runtime().context().recorder(); }
  void tap(app::Button b) {
    h.press(b);
    h.run_ms(1);
    h.release(b);
    h.run_ms(1);
  }
  bool wait_state(hr::State s, std::uint32_t max_ms) {
    for (std::uint32_t i = 0; i < max_ms && rec().state() != s; ++i) {
      h.run_ms(1);
    }
    return rec().state() == s;
  }
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint32_t le16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8); }

// A 16-bit PCM WAV of the kit's (44-byte header, `fmt ` then `data`): its channels, rate and samples.
struct Wav {
  bool ok = false;
  std::uint32_t channels = 0;
  std::uint32_t rate = 0;
  Pcm pcm;
};
Wav parse_wav(const std::vector<std::uint8_t>& b) {
  Wav w;
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0 || std::memcmp(b.data() + 12, "fmt ", 4) != 0 || le16(b, 20) != 1u || le16(b, 34) != 16u ||
      std::memcmp(b.data() + 36, "data", 4) != 0) {
    return w;
  }
  w.channels = le16(b, 22);
  w.rate = le32(b, 24);
  const std::uint32_t bytes = le32(b, 40);
  if (44u + bytes > b.size() || le32(b, 4) != 36u + bytes) {
    return w;
  }
  w.pcm.resize(bytes / 2u);
  std::memcpy(w.pcm.data(), b.data() + 44, bytes);
  w.ok = true;
  return w;
}

hr::Take take_of(Pcm& buf, std::uint32_t frames, hr::Channels ch, hr::Align align) {
  hr::Take t;
  t.buf = buf.data();
  t.capacity_frames = frames;
  t.channels = ch;
  t.align = align;
  return t;
}

}  // namespace

#ifdef O3SDK_HELLO_SCENARIOS
TEST_CASE("sim record: scenarios/record.txt runs as it is, and the WAVs it leaves are takes of 44.1 kHz") {
  host::ScratchDir card_dir("sim-record-card");
  host::ScratchDir out_dir("sim-record-out");
  REQUIRE(card_dir.ok());
  host::DirCard card(card_dir.path());
  host::FakeCapture capture;
  sim::Harness h(o3sdk_app(), nullptr, nullptr, &card, nullptr, &capture);
  REQUIRE(h.start());
  const std::filesystem::path file = std::filesystem::path(O3SDK_HELLO_SCENARIOS) / "record.txt";
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  REQUIRE_FALSE(text.str().empty());
  const sim::ScenarioResult r = sim::run_scenario(h, text.str(), out_dir.path());
  CHECK_MESSAGE(r.ok, file.string() << ":" << r.line << ": " << r.error);
  CHECK(card.violations() == 0u);
  const std::filesystem::path apps = std::filesystem::path(card_dir.path()) / "Apps" / "hello";
  const std::uint32_t frames[4] = {44100, 44100, 44100, 0};  // (the fourth was stopped by the key: its length is the time it ran)
  for (unsigned n = 1; n <= 4; ++n) {
    const Wav w = parse_wav(read_file(apps / ("take_00" + std::to_string(n) + ".wav")));
    REQUIRE_MESSAGE(w.ok, "take_00" << n << ".wav");
    CHECK(w.channels == 1u);
    CHECK(w.rate == 44100u);
    if (frames[n - 1] != 0u) {
      CHECK(w.pcm.size() == frames[n - 1]);
    } else {
      CHECK(w.pcm.size() > 5000u);
      CHECK(w.pcm.size() < 44100u);
    }
  }
  // The take of the input, 440 Hz at -12 dBFS: a sine as loud as the codec had it, from its first frame.
  const Wav tone = parse_wav(read_file(apps / "take_001.wav"));
  std::int32_t peak = 0;
  for (const std::int16_t s : tone.pcm) {
    peak = std::max<std::int32_t>(peak, std::abs(static_cast<std::int32_t>(s)));
  }
  CHECK(peak > 8000);
  CHECK(peak < 8300);
  // The level take: the 50 ms heard before the tone are silence (the input was off), the tone then starts it.
  const Wav lvl = parse_wav(read_file(apps / "take_003.wav"));
  std::size_t q = 0;
  while (q < lvl.pcm.size() && lvl.pcm[q] == 0) {
    ++q;
  }
  CHECK(q >= 2205u);
  CHECK(q <= 2207u);
  CHECK(h.runtime().frame_failures() == 0u);
}
#endif

TEST_CASE("sim record: a take on a step starts at the sample of the sixteenth, from the very first one to the 50th") {
  Rig rig("sim-record-step");
  rig.capture.input_wav(ramp_wav());
  REQUIRE(rig.h.start());
  rig.h.run_ms(20);
  // Play: find the block the transport started in (the engine's block_frame is the last block's).
  std::uint64_t base = 0;
  bool found = false;
  rig.h.press(app::Button::Play);
  rig.h.run_ms(1);
  rig.h.release(app::Button::Play);
  for (unsigned i = 0; i < 20 && !found; ++i) {
    const std::uint64_t before = rig.h.samples_rendered();
    rig.h.run_ms(1);
    if (rig.h.samples_rendered() != before && rig.h.engine().block_frame().started) {
      base = rig.h.samples_rendered() - hal::audio::kBlock;
      found = true;
    }
  }
  REQUIRE(found);
  const std::uint64_t inc = dsp::ticks_per_sample_q32(rig.h.engine().tempo());
  const auto step_sample = [&](std::uint64_t k) { return base + (((k * dsp::kTicksPerSixteenth) << 32) + inc - 1u) / inc; };
  Pcm buf(64);
  unsigned inside = 0;  // steps that begin inside a block (not on its first frame)
  for (unsigned round = 0; round < 50; ++round) {
    rig.h.run_ms(round % 7u + 1u);
    REQUIRE(rig.rec().arm(take_of(buf, 16, hr::Channels::Left, hr::Align::Step)));
    const std::uint64_t armed_at = rig.h.samples_rendered();  // the take can start on a step at or after this sample
    std::uint64_t k = 0;
    while (step_sample(k) < armed_at) {
      ++k;
    }
    REQUIRE(rig.wait_state(hr::State::Done, 400));
    REQUIRE(rig.rec().frames() == 16u);
    for (std::uint32_t i = 0; i < 16; ++i) {
      REQUIRE(buf[i] == ramp_at(step_sample(k) + i));  // the take's first frame IS the step's sample, and the rest follow it
    }
    inside += step_sample(k) % hal::audio::kBlock != 0u ? 1u : 0u;
    rig.rec().release();
  }
  CHECK(inside > 40u);  // (120 BPM is 5,512.5 samples a sixteenth: nearly every step is inside a block, half of them at a half sample)
}

TEST_CASE("sim record: a stop on a step makes the take exactly the frames between two steps") {
  Rig rig("sim-record-stop");
  rig.capture.input_wav(ramp_wav());
  REQUIRE(rig.h.start());
  rig.h.run_ms(20);
  rig.h.press(app::Button::Play);
  rig.h.run_ms(1);
  rig.h.release(app::Button::Play);
  rig.h.run_ms(40);
  Pcm buf(100000);
  REQUIRE(rig.rec().arm(take_of(buf, 100000, hr::Channels::Mix, hr::Align::Bar)));
  REQUIRE(rig.wait_state(hr::State::Recording, 4000));
  rig.rec().stop_at(hr::Align::Bar);  // the next bar: 16 sixteenths of 5,512.5 samples
  REQUIRE(rig.wait_state(hr::State::Done, 6000));
  CHECK(rig.rec().frames() == 88200u);
  CHECK(rig.rec().overrun() == 0u);
}

TEST_CASE("sim record: a level take keeps 50 ms before the signal in order") {
  Rig rig("sim-record-level");
  rig.capture.input_wav(level_wav());
  REQUIRE(rig.h.start());
  rig.h.run_ms(5);
  Pcm buf(2u * 30000u, 0x5A5A);
  hr::Take t = take_of(buf, 20000, hr::Channels::Mix, hr::Align::Level);
  t.threshold = 1000;
  t.preroll_frames = 2205;
  REQUIRE(rig.rec().arm(t));
  CHECK(rig.rec().state() == hr::State::Listening);
  rig.h.run_ms(100);  // 4,410 samples of silence: listening, nothing is a take yet
  CHECK(rig.rec().state() == hr::State::Listening);
  CHECK(rig.rec().frames() == 0u);
  REQUIRE(rig.wait_state(hr::State::Done, 3000));
  const app::Recorder::Clip c = rig.rec().clip();
  REQUIRE(c.frames > 2205u);
  CHECK(c.channels == 1u);
  // The take is silence for the pre-roll, then the signal from its first sample, rising by one a sample: nothing out of order.
  std::uint32_t q = 0;
  while (q < c.frames && c.pcm[q] == 0) {
    ++q;
  }
  CHECK(q >= 2205u);
  CHECK(q <= 2206u);
  for (std::uint32_t i = q; i < c.frames; ++i) {
    REQUIRE(c.pcm[i] == static_cast<std::int16_t>(2000u + (i - q)));
  }
  // Nothing was written past the capacity.
  for (std::size_t i = 20000; i < buf.size(); ++i) {
    REQUIRE(buf[i] == 0x5A5A);
  }
}

TEST_CASE("sim record: a full buffer is counted and the take holds all that fit; the save is a WAV that reads back equal") {
  Rig rig("sim-record-save");
  rig.capture.input_wav(ramp_wav());
  REQUIRE(rig.h.start());
  rig.h.run_ms(10);
  constexpr std::uint32_t kCap = 1000;  // 15 blocks and 40 frames: the 24 frames of the 16th block that do not fit are the overrun
  Pcm buf(kCap + 64u, 0x5A5A);
  REQUIRE(rig.rec().arm(take_of(buf, kCap, hr::Channels::Mix, hr::Align::Free)));
  const std::uint64_t armed_at = rig.h.samples_rendered();
  REQUIRE(rig.wait_state(hr::State::Done, 200));
  CHECK(rig.rec().frames() == kCap);
  CHECK(rig.rec().overrun() == 24u);
  // The take is the input from a block boundary on, unbroken.
  std::uint64_t first = armed_at;
  while (ramp_at(first) != buf[0]) {
    ++first;
    REQUIRE(first < armed_at + 4u * hal::audio::kBlock);
  }
  CHECK(first % hal::audio::kBlock == 0u);
  for (std::uint32_t i = 0; i < kCap; ++i) {
    REQUIRE(buf[i] == ramp_at(first + i));
  }
  for (std::size_t i = kCap; i < buf.size(); ++i) {
    REQUIRE(buf[i] == 0x5A5A);
  }
  // Saved: Apps/hello/take_001.wav, mono, 44,100 Hz, the same samples. A second save is the next number.
  app::Files& files = rig.h.runtime().context().files();
  REQUIRE(rig.rec().save(files, "", "take"));
  for (unsigned i = 0; i < 2000 && files.busy(); ++i) {
    rig.h.run_ms(1);
  }
  REQUIRE(files.status().phase == app::Files::Status::Done);
  const std::filesystem::path apps = std::filesystem::path(rig.card_dir.path()) / "Apps" / "hello";
  const Wav w = parse_wav(read_file(apps / "take_001.wav"));
  REQUIRE(w.ok);
  CHECK(w.channels == 1u);
  CHECK(w.rate == 44100u);
  REQUIRE(w.pcm.size() == kCap);
  CHECK(std::memcmp(w.pcm.data(), buf.data(), kCap * sizeof(std::int16_t)) == 0);
  REQUIRE(rig.rec().save(files, "", "take"));
  for (unsigned i = 0; i < 2000 && files.busy(); ++i) {
    rig.h.run_ms(1);
  }
  CHECK(std::filesystem::is_regular_file(apps / "take_002.wav"));
  rig.rec().release();
  CHECK(rig.rec().state() == hr::State::Idle);
  CHECK_FALSE(rig.rec().save(files, "", "take"));  // no take
  CHECK(rig.card.violations() == 0u);
}

TEST_CASE("sim record: a stereo take saves as a two-channel WAV, and a take that is not Done does not save") {
  Rig rig("sim-record-stereo");
  rig.capture.input_wav(ramp_wav());
  REQUIRE(rig.h.start());
  rig.h.run_ms(10);
  Pcm buf(2u * 640u);
  app::Files& files = rig.h.runtime().context().files();
  REQUIRE(rig.rec().arm(take_of(buf, 640, hr::Channels::Stereo, hr::Align::Free)));
  CHECK_FALSE(rig.rec().save(files, "", "st"));  // Recording or Waiting is no take yet
  REQUIRE(rig.wait_state(hr::State::Done, 100));
  REQUIRE(rig.rec().save(files, "", "st"));
  for (unsigned i = 0; i < 2000 && files.busy(); ++i) {
    rig.h.run_ms(1);
  }
  const std::filesystem::path apps = std::filesystem::path(rig.card_dir.path()) / "Apps" / "hello";
  const Wav w = parse_wav(read_file(apps / "st_001.wav"));
  REQUIRE(w.ok);
  CHECK(w.channels == 2u);
  REQUIRE(w.pcm.size() == 1280u);
  CHECK(std::memcmp(w.pcm.data(), buf.data(), buf.size() * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sim record: an idle recorder does no work, and recording changes nothing of what the main output plays") {
  Pcm idle_audio;
  {
    Rig rig("sim-record-golden-a");
    rig.capture.input_wav(ramp_wav());
    REQUIRE(rig.h.start());
    rig.h.record(true);
    rig.h.run_ms(5);
    rig.tap(app::Button::Play);
    rig.h.run_ms(600);
    CHECK(rig.h.recorder().active_blocks() == 0u);  // every block of the run found the recorder Idle
    idle_audio = rig.h.audio();
  }
  REQUIRE(idle_audio.size() > 2u * 20000u);
  bool any = false;
  for (const std::int16_t s : idle_audio) {
    any = any || s != 0;
  }
  CHECK(any);
  Rig rig("sim-record-golden-b");
  rig.capture.input_wav(ramp_wav());
  REQUIRE(rig.h.start());
  rig.h.record(true);
  rig.h.run_ms(5);
  static Pcm buf(2u * 441000u);
  REQUIRE(rig.rec().arm(take_of(buf, 441000, hr::Channels::Stereo, hr::Align::Free)));  // the take runs all through the run (and the master is not what it records)
  rig.tap(app::Button::Play);
  rig.h.run_ms(600);
  CHECK(rig.rec().state() == hr::State::Recording);
  CHECK(rig.h.recorder().active_blocks() > 300u);
  REQUIRE(rig.h.audio().size() == idle_audio.size());
  CHECK(std::memcmp(rig.h.audio().data(), idle_audio.data(), idle_audio.size() * sizeof(std::int16_t)) == 0);
}

TEST_CASE("sim record: an Input take hears the line's channel map the runtime applies, and the runtime passes it to the recorder") {
  Rig rig("sim-record-map");
  Pcm lr = ramp_wav();
  for (std::uint32_t i = 0; i < kLoop; ++i) {
    lr[2u * i + 1u] = static_cast<std::int16_t>(i + 1u + 1000u);  // the right channel is the left one plus 1,000
  }
  rig.capture.input_wav(lr);
  REQUIRE(rig.h.start());
  rig.h.run_ms(10);
  REQUIRE(rig.h.runtime().context().audio_in().select(hal::capture::Source::LineLeft, 0));
  rig.h.run_ms(300);  // the gate opens again after the switch settles; the map reached the recorder on the first pass
  Pcm buf(256);
  // A take of the RIGHT channel is the left one: the engine's input is the left channel in both planes.
  REQUIRE(rig.rec().arm(take_of(buf, 128, hr::Channels::Right, hr::Align::Free)));
  const std::uint64_t armed_at = rig.h.samples_rendered();
  REQUIRE(rig.wait_state(hr::State::Done, 50));
  std::uint64_t first = armed_at;
  while (ramp_at(first) != buf[0]) {
    ++first;
    REQUIRE(first < armed_at + 4u * hal::audio::kBlock);
  }
  for (std::uint32_t i = 0; i < 128; ++i) {
    REQUIRE(buf[i] == ramp_at(first + i));
  }
  rig.rec().release();
  // Back to the line's two channels as they are (LineMix is their mean; here: the mean of l and l + 1,000).
  REQUIRE(rig.h.runtime().context().audio_in().select(hal::capture::Source::LineMix, 0));
  rig.h.run_ms(300);
  REQUIRE(rig.rec().arm(take_of(buf, 128, hr::Channels::Left, hr::Align::Free)));
  const std::uint64_t armed2 = rig.h.samples_rendered();
  REQUIRE(rig.wait_state(hr::State::Done, 50));
  std::uint64_t second = armed2 - 0u;
  while (static_cast<std::int16_t>(ramp_at(second) + 500) != buf[0] && second < armed2 + 4u * hal::audio::kBlock) {
    ++second;
  }
  CHECK(second < armed2 + 4u * hal::audio::kBlock);
  for (std::uint32_t i = 0; i < 128; ++i) {
    CHECK(buf[i] == static_cast<std::int16_t>(ramp_at(second + i) + 500));
  }
}
