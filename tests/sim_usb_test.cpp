// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB audio on the hello app, end to end in the simulator (docs/M2C-PLAN.md section 7, slice U): the unit's own USB audio service runs over a fake port and a fake computer, so what is proved
// here is the unit's own code and arithmetic. The scenario apps/hello/scenarios/usb.txt runs as it is and what it leaves is read back: the capture stream of the boot profile P8 carries the
// main output's own samples on channels 7 and 8 and silence on 1..6; P10 (chosen in the Config with F1 twice) carries HELLO's instrument as T1 and the master on 9 and 10; the return the
// computer plays is in the main output at RETURN LEVEL and in no stem; as the audio in's source the same return is what a take of the input records; a card given to the computer refuses a
// save until the eject. A platform with the device that no stream is open on changes nothing the main output plays and does no work in the render (counted). The costs on the unit and the real
// port's timing are the unit's.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/platform/host/fake_usb_audio.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cmath>
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
using app::Button;

namespace {

using Pcm = std::vector<std::int16_t>;

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint32_t le16(const std::vector<std::uint8_t>& b, std::size_t at) { return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8); }

// A 16-bit PCM WAV with a 44-byte header, any number of channels: de-interleaved.
struct Wav {
  bool ok = false;
  std::uint32_t channels = 0;
  std::uint32_t rate = 0;
  std::vector<Pcm> ch;
};
Wav parse_wav(const std::vector<std::uint8_t>& b) {
  Wav w;
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0 || std::memcmp(b.data() + 12, "fmt ", 4) != 0 || le16(b, 20) != 1u ||
      le16(b, 34) != 16u || std::memcmp(b.data() + 36, "data", 4) != 0) {
    return w;
  }
  w.channels = le16(b, 22);
  w.rate = le32(b, 24);
  const std::uint32_t bytes = le32(b, 40);
  if (w.channels == 0u || 44u + bytes > b.size()) {
    return w;
  }
  const std::size_t frames = bytes / (2u * w.channels);
  w.ch.assign(w.channels, Pcm(frames));
  for (std::size_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < w.channels; ++c) {
      w.ch[c][f] = static_cast<std::int16_t>(le16(b, 44 + 2 * (f * w.channels + c)));
    }
  }
  w.ok = true;
  return w;
}

bool all_zero(const Pcm& p) {
  for (const std::int16_t s : p) {
    if (s != 0) {
      return false;
    }
  }
  return true;
}
std::int32_t peak(const Pcm& p) {
  std::int32_t m = 0;
  for (const std::int16_t s : p) {
    m = std::max<std::int32_t>(m, std::abs(static_cast<std::int32_t>(s)));
  }
  return m;
}

// The main output's left channel from the harness's recording (interleaved), and where in it a captured channel starts: the first frame of the capture with a sample that is not zero is a
// frame of the master, found by its value over a few frames (the instrument's samples do not repeat). npos: not found.
Pcm left_of(const std::vector<std::int16_t>& lr) {
  Pcm l(lr.size() / 2u);
  for (std::size_t i = 0; i < l.size(); ++i) {
    l[i] = lr[2u * i];
  }
  return l;
}
std::size_t find_start(const Pcm& main_l, const Pcm& captured_l, std::size_t& first_nonzero) {
  first_nonzero = 0;
  while (first_nonzero + 16u < captured_l.size() && captured_l[first_nonzero] == 0) {
    ++first_nonzero;
  }
  for (std::size_t off = 0; off + 16u < main_l.size(); ++off) {
    bool same = true;
    for (std::size_t k = 0; k < 16u && same; ++k) {
      same = main_l[off + k] == captured_l[first_nonzero + k];
    }
    if (same) {
      return off;
    }
  }
  return std::string::npos;
}

}  // namespace

#ifdef O3SDK_HELLO_SCENARIOS
TEST_CASE("sim usb: scenarios/usb.txt runs as it is - P8's master on 7 and 8 is the main output to the sample, P10 has T1 and the master on 9 and 10, the return is mixed and in no stem, as the source it is the take") {
  host::ScratchDir card_dir("sim-usb-card");
  host::ScratchDir out_dir("sim-usb-out");
  REQUIRE(card_dir.ok());
  host::DirCard card(card_dir.path());
  host::FakeCapture capture;
  host::FakeUsbAudio usb;
  sim::Harness h(o3sdk_app(), nullptr, nullptr, &card, nullptr, &capture, &usb);
  REQUIRE(h.start());
  h.record(true);
  const std::filesystem::path file = std::filesystem::path(O3SDK_HELLO_SCENARIOS) / "usb.txt";
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  REQUIRE_FALSE(text.str().empty());
  const sim::ScenarioResult r = sim::run_scenario(h, text.str(), out_dir.path());
  CHECK_MESSAGE(r.ok, file.string() << ":" << r.line << ": " << r.error);
  CHECK(card.violations() == 0u);
  const Pcm main_l = left_of(h.audio());
  const std::filesystem::path out = out_dir.path();

  // 1. The boot profile.
  const Wav p8 = parse_wav(read_file(out / "01_p8.wav"));
  REQUIRE(p8.ok);
  CHECK(p8.channels == 8u);
  CHECK(p8.rate == 44100u);
  for (unsigned c = 0; c < 6u; ++c) {
    CHECK(all_zero(p8.ch[c]));
  }
  std::size_t first = 0;
  const std::size_t at = find_start(main_l, p8.ch[6], first);
  REQUIRE(at != std::string::npos);
  CHECK(p8.ch[6].size() - first > 20000u);
  bool exact = true;
  for (std::size_t i = first; i < p8.ch[6].size(); ++i) {
    exact = exact && p8.ch[6][i] == main_l[at - first + i];
  }
  CHECK(exact);  // channel 7 is what the main output played, sample for sample
  CHECK(peak(p8.ch[7]) > 1000);

  // 2. P10.
  const Wav p10 = parse_wav(read_file(out / "02_p10.wav"));
  REQUIRE(p10.ok);
  CHECK(p10.channels == 10u);
  CHECK(peak(p10.ch[0]) > 1000);  // T1: HELLO's instrument
  for (unsigned c = 1; c < 8u; ++c) {
    CHECK(all_zero(p10.ch[c]));
  }
  CHECK(peak(p10.ch[8]) > 1000);
  CHECK(peak(p10.ch[9]) > 1000);

  // 3. The return: after the computer's 1 kHz tone (at -12 dBFS, 8231 peak) has primed, the main output is the stems' master plus the tone at RETURN LEVEL 100 per cent, and the stems are
  // the master alone: the difference over the last part of the capture is the tone.
  const Wav ret = parse_wav(read_file(out / "03_return.wav"));
  REQUIRE(ret.ok);
  CHECK(ret.channels == 10u);
  std::size_t first_ret = 0;
  const std::size_t at_ret = find_start(main_l, ret.ch[8], first_ret);
  REQUIRE(at_ret != std::string::npos);
  const std::size_t n = ret.ch[8].size();
  double sum = 0.0;
  double before = 0.0;
  std::size_t counted = 0;
  const std::size_t tail = 4410;  // the last 100 ms
  for (std::size_t i = n - tail; i < n; ++i, ++counted) {
    const double d = static_cast<double>(main_l[at_ret - first_ret + i]) - static_cast<double>(ret.ch[8][i]);
    sum += d * d;
  }
  for (std::size_t i = first_ret + 128; i < first_ret + 128 + 2000u && i < n; ++i) {  // (before the tone: the capture of the P10 stream ran 600 ms ahead of it)
    const double d = static_cast<double>(main_l[at_ret - first_ret + i]) - static_cast<double>(ret.ch[8][i]);
    before += d * d;
  }
  const double rms = std::sqrt(sum / static_cast<double>(counted));
  CHECK(rms > 5000.0);  // 8231 / sqrt(2) = 5820
  CHECK(rms < 6500.0);
  CHECK(before == 0.0);

  // 4. The take of the input, with INPUT = USB and RETURN LEVEL 0: the computer's 440 Hz.
  const Wav take = parse_wav(read_file(std::filesystem::path(card_dir.path()) / "Apps" / "hello" / "take_001.wav"));
  REQUIRE(take.ok);
  CHECK(take.rate == 44100u);
  REQUIRE(take.ch.size() >= 1u);
  const Pcm& t = take.ch[0];
  CHECK(t.size() == 44100u);
  unsigned crossings = 0;
  for (std::size_t i = 1; i < t.size(); ++i) {
    if (t[i - 1] < 0 && t[i] >= 0) {
      ++crossings;
    }
  }
  CHECK(crossings >= 436u);  // one a period: 440 in a second (a few less where the take began before the tone)
  CHECK(crossings <= 441u);
  CHECK(peak(t) > 7500);
  CHECK(peak(t) < 9000);

  // 5. The card as a disk (the scenario's expects hold the rest): the saved riff is on the card after the eject.
  CHECK(std::filesystem::exists(std::filesystem::path(card_dir.path()) / "Apps" / "hello" / "riff.o3a"));
}

TEST_CASE("sim usb: a USB device nobody listens to changes not a sample of the main output and costs the render nothing") {
  const auto run = [](host::FakeUsbAudio* usb) {
    sim::Harness h(o3sdk_app(), nullptr, nullptr, nullptr, nullptr, nullptr, usb);
    REQUIRE(h.start());
    h.record(true);
    h.press(Button::Play);
    h.run_ms(5);
    h.release(Button::Play);
    h.run_ms(1500);
    return std::vector<std::int16_t>(h.audio());
  };
  const std::vector<std::int16_t> without = run(nullptr);
  host::FakeUsbAudio usb;
  const std::vector<std::int16_t> with = run(&usb);
  REQUIRE(without.size() > 100000u);
  CHECK(with == without);
  CHECK(peak(left_of(without)) > 1000);  // (it played)
  CHECK(usb.service().active_blocks() == 0u);
  CHECK(usb.service().pulled_blocks() == 0u);
  CHECK(usb.service().tap().published() == 0u);
  CHECK(usb.captured().empty());
}

TEST_CASE("sim usb: with the stream open the main output is still not a sample different, the engine stages the master pair only, and the work is counted") {
  const auto run = [](host::FakeUsbAudio* usb, std::uint32_t* stem_frames) {
    sim::Harness h(o3sdk_app(), nullptr, nullptr, nullptr, nullptr, nullptr, usb);
    REQUIRE(h.start());
    h.record(true);
    if (usb != nullptr) {
      usb->listen(true);
    }
    h.press(Button::Play);
    h.run_ms(5);
    h.release(Button::Play);
    h.run_ms(1500);
    if (stem_frames != nullptr) {
      *stem_frames = h.engine().feature_work().stem_frames;
    }
    return std::vector<std::int16_t>(h.audio());
  };
  const std::vector<std::int16_t> without = run(nullptr, nullptr);
  host::FakeUsbAudio usb;
  std::uint32_t staged = 0;
  const std::vector<std::int16_t> with = run(&usb, &staged);
  CHECK(with == without);
  const std::uint32_t blocks = static_cast<std::uint32_t>(with.size() / 2u / 64u);
  CHECK(staged > (blocks - 4u) * 2u * 64u);  // two sources a block (master left and right), from the first block after the stream opened
  CHECK(staged <= blocks * 2u * 64u);
  CHECK(usb.captured().size() / 8u > 60000u);
}
#endif
