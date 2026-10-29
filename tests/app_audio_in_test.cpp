// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio in and the FM radio for an app (docs/M2C-PLAN.md section 5, slice AI): Context::audio_in() and radio() over the fake capture device in the simulator's harness. The source switch
// settles silent, the line's channels, the gain, the monitor, the peaks that exist only while asked for, the retry of a codec that does not acknowledge, the radio's requests and its Absent,
// the kit's settings, the Config's AUDIO section, and the proof that an app that never calls any of it renders what it did before. The fake's own rules are host_fake_capture_test.cpp.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/audio_in.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/radio.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/app/settings.h>
#include <o3sdk/core/audio_clock.h>
#include <o3sdk/formats/chunk_file.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/platform/host/fake_capture.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace o3sdk;
namespace host = o3sdk::platform::host;
using app::Button;
using hal::capture::Source;
using RState = hal::capture::RadioStatus::State;

namespace {

// An instrument: a sine of its own (`own`, 0 for none) plus its input passed through (in_l to out_l, in_r to out_r) when `listen`, so that the codec's input is audible at the master through a
// unit, as it is for any instrument or effect that uses `in_l` and `in_r`.
class Voice final : public dsp::Unit {
 public:
  Voice(float own, bool listen) : info_{"VOICE", dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0}, own_(own), listen_(listen) {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override { phase_ = 0.0f; }
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i) {
      const float s = own_ * std::sin(phase_);
      phase_ += 0.05f;
      if (phase_ > 6.2831853f) {
        phase_ -= 6.2831853f;
      }
      a.out_l[i] = s + (listen_ ? a.in_l[i] : 0.0f);
      a.out_r[i] = s + (listen_ ? a.in_r[i] : 0.0f);
    }
  }

 private:
  dsp::UnitInfo info_;
  float own_;
  bool listen_;
  float phase_ = 0.0f;
};

class BlankScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "PROBE"; }
  void draw(app::Context&, ui::Renderer&, const ui::Rect&) noexcept override {}
};

class ProbeApp final : public app::App {
 public:
  explicit ProbeApp(float own, bool listen) : voice_(own, listen) {}
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"AIPROBE", "1.0.0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override {
    const dsp::UnitId id = ctx.engine().attach(voice_);
    (void)ctx.engine().set_source(0, id);
    ctx.show(screen_);
  }

 private:
  Voice voice_;
  BlankScreen screen_;
};

// An app with its own Voice for each test (the harness drives the app it is given).
struct Rig {
  explicit Rig(float own, bool listen, bool with_capture = true, host::HostCard* card = nullptr)
      : app(own, listen), h(app, nullptr, nullptr, card, "aiprobe", with_capture ? &cap : nullptr) {
    started = h.start();
  }
  host::FakeCapture cap;
  ProbeApp app;
  sim::Harness h;
  bool started = false;
  app::Context& ctx() { return h.runtime().context(); }
  app::AudioIn& in() { return ctx().audio_in(); }
  app::Radio& radio() { return ctx().radio(); }
  std::string state() {
    char line[512];
    (void)h.runtime().state_line(line, sizeof line);
    return line;
  }
  void tap(Button b) {
    h.press(b);
    h.run_ms(1);
    h.release(b);
    h.run_ms(1);
  }
};

// RMS of the left channel of the recorded master between two times (ms).
float rms(const std::vector<std::int16_t>& a, unsigned from_ms, unsigned to_ms) {
  const auto at = [](unsigned ms) { return static_cast<std::size_t>(static_cast<float>(ms) * core::kRefRateHz / 1000.0f); };
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = at(from_ms); i < at(to_ms) && 2u * i < a.size(); ++i, ++n) {
    const double v = static_cast<double>(a[2u * i]) / 32768.0;
    sum += v * v;
  }
  return n == 0u ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}
float rms_right(const std::vector<std::int16_t>& a, unsigned from_ms, unsigned to_ms) {
  const auto at = [](unsigned ms) { return static_cast<std::size_t>(static_cast<float>(ms) * core::kRefRateHz / 1000.0f); };
  double sum = 0.0;
  std::size_t n = 0;
  for (std::size_t i = at(from_ms); i < at(to_ms) && 2u * i + 1u < a.size(); ++i, ++n) {
    const double v = static_cast<double>(a[2u * i + 1u]) / 32768.0;
    sum += v * v;
  }
  return n == 0u ? 0.0f : static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
}

constexpr float kTone = 0.25118864f / 1.41421356f;  // the RMS of a sine at -12 dBFS

}  // namespace

// ---- Without a capture device ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("audio in: without a capture device every call is refused and the runtime is as it was") {
  Rig rig(0.2f, false, /*with_capture=*/false);
  REQUIRE(rig.started);
  rig.h.run_ms(50);
  CHECK_FALSE(rig.in().available());
  CHECK_FALSE(rig.radio().available());
  CHECK_FALSE(rig.in().select(Source::MicHigh, 50));
  CHECK_FALSE(rig.in().set_gain(10));
  CHECK(rig.in().source() == Source::LineMix);
  CHECK(rig.in().gain() == 0u);
  const dsp::InputPeaks p = rig.in().peaks();
  CHECK(p.l == 0.0f);
  CHECK(p.r == 0.0f);
  CHECK(rig.radio().status().state == RState::Absent);
  CHECK_FALSE(rig.radio().usable());
  rig.radio().tune(9370);  // (nothing to tune: no change, no crash)
  rig.radio().seek(true);
  // The engine's input features were not touched, there is no AUDIO section, and the state line has no audio pairs.
  CHECK(rig.h.engine().input_gate());
  CHECK_FALSE(rig.h.engine().input_peaks_on());
  CHECK(rig.h.engine().input_monitor() == 0.0f);
  CHECK(rig.state().find(" in=") == std::string::npos);
  rig.tap(Button::Config);
  CHECK(rig.state().find("section=audio") == std::string::npos);
  CHECK(rig.state().find("section=display") != std::string::npos);
}

TEST_CASE("audio in: an app that never calls it renders what it did, and the engine's features cost nothing") {
  // The same app (an instrument that ignores its input) on a platform without a capture device and on one with a codec that hears a loud tone: the master is bit-identical.
  std::vector<std::int16_t> bare;
  std::vector<std::int16_t> with;
  {
    Rig rig(0.3f, false, false);
    REQUIRE(rig.started);
    rig.h.record(true);
    rig.h.run_ms(300);
    bare = rig.h.audio();
  }
  {
    Rig rig(0.3f, false, true);
    REQUIRE(rig.started);
    rig.cap.input_tone(440.0f, -3.0f);
    rig.h.record(true);
    rig.h.run_ms(300);
    with = rig.h.audio();
    const dsp::FeatureWork w = rig.h.engine().feature_work();
    CHECK(w.gate_frames == 0u);
    CHECK(w.monitor_frames == 0u);
    CHECK(w.peak_frames == 0u);
    CHECK(rig.h.engine().input_gate());
    CHECK_FALSE(rig.h.engine().input_peaks_on());
    CHECK(rig.cap.selects() == 0u);  // nothing touched the codec
    CHECK(rig.cap.gains() == 0u);
    CHECK(rig.cap.power_calls() == 0u);  // and the receiver is off
    CHECK(rig.radio().status().state == RState::Off);
  }
  REQUIRE(bare.size() == with.size());
  CHECK(bare == with);
  CHECK(rms(bare, 50, 300) > 0.05f);  // (it did play)
}

// ---- Source, gate and channels --------------------------------------------------------------------------------------------------------------------

TEST_CASE("audio in: a source switch settles silent for 200 ms, then the input reaches the units") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.input_tone(440.0f, -12.0f);
  rig.h.record(true);
  rig.h.run_ms(100);
  const unsigned t0 = 100;
  // The tone is at the master from the start (the power-up input; nothing was selected).
  const float base = rms(rig.h.audio(), 40, 100);
  CHECK(base > 0.05f);
  REQUIRE(rig.in().select(Source::MicLow, 0));
  CHECK(rig.cap.selects() == 1u);
  CHECK(rig.cap.source() == Source::MicLow);
  CHECK(rig.in().settling());
  CHECK_FALSE(rig.h.engine().input_gate());
  rig.h.run_ms(190);
  CHECK(rig.in().settling());
  CHECK(rms(rig.h.audio(), t0 + 10, t0 + 190) == 0.0f);  // silent the whole time it settles (past the one block's fade)
  rig.h.run_ms(40);
  CHECK_FALSE(rig.in().settling());
  CHECK(rig.h.engine().input_gate());
  rig.h.run_ms(60);
  CHECK(rms(rig.h.audio(), t0 + 250, t0 + 290) == doctest::Approx(base).epsilon(0.02));
  CHECK(rig.state().find("in=miclow") != std::string::npos);
  CHECK(rig.state().find("gate=1") != std::string::npos);
  // A source that does not exist is refused and changes nothing.
  CHECK_FALSE(rig.in().select(static_cast<Source>(99), 10));
  CHECK(rig.in().source() == Source::MicLow);
}

TEST_CASE("audio in: LineMix is the mean of the line's channels, LineLeft and LineRight one channel in both planes, the rest as they are") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  // Left: a sine at 0.4, right: silence. Interleaved, long enough to loop well.
  std::vector<std::int16_t> lr;
  for (unsigned i = 0; i < 4410; ++i) {
    lr.push_back(static_cast<std::int16_t>(0.4f * 32767.0f * std::sin(0.063f * static_cast<float>(i))));
    lr.push_back(0);
  }
  rig.cap.input_wav(lr);
  rig.h.record(true);
  rig.h.run_ms(20);
  std::size_t mark = rig.h.audio().size();
  const auto window = [&](Source s) {
    REQUIRE(rig.in().select(s, 0));
    rig.h.run_ms(300);
    mark = rig.h.audio().size();
    rig.h.run_ms(100);
    std::vector<std::int16_t> w(rig.h.audio().begin() + static_cast<std::ptrdiff_t>(mark), rig.h.audio().end());
    return std::pair<float, float>{rms(w, 0, 100), rms_right(w, 0, 100)};
  };
  const auto mic = window(Source::MicLow);  // not a line source: the planes as the codec gave them, the reference
  const float full = mic.first;
  CHECK(full > 0.05f);
  CHECK(mic.second == 0.0f);
  const auto left = window(Source::LineLeft);
  CHECK(left.first == doctest::Approx(full).epsilon(0.03));
  CHECK(left.second == doctest::Approx(full).epsilon(0.03));  // the left channel in both planes
  const auto right = window(Source::LineRight);
  CHECK(right.first == 0.0f);
  CHECK(right.second == 0.0f);
  const auto mix = window(Source::LineMix);
  CHECK(mix.first == doctest::Approx(full * 0.5f).epsilon(0.03));
  CHECK(mix.second == doctest::Approx(full * 0.5f).epsilon(0.03));
}

TEST_CASE("audio in: the gain is the platform's, set_gain does not settle, and select keeps what it was given") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.input_tone(1000.0f, -40.0f);
  REQUIRE(rig.in().select(Source::LineMix, 0));
  rig.h.run_ms(300);
  const dsp::InputPeaks base = rig.in().peaks();  // starts the measurement
  CHECK(base.l == 0.0f);
  rig.h.run_ms(50);
  const dsp::InputPeaks quiet = rig.in().peaks();
  CHECK(quiet.l == doctest::Approx(0.01f).epsilon(0.1));
  CHECK(rig.in().gain() == 0u);
  CHECK(rig.in().set_gain(100));
  CHECK(rig.cap.gains() == 1u);
  CHECK(rig.cap.gain() == 100u);
  CHECK_FALSE(rig.in().settling());  // no gate for a gain
  CHECK(rig.h.engine().input_gate());
  rig.h.run_ms(50);
  const dsp::InputPeaks loud = rig.in().peaks();
  CHECK(loud.l / quiet.l == doctest::Approx(std::pow(10.0f, 22.5f / 20.0f)).epsilon(0.05));  // the line's top: 22.5 dB
  CHECK(rig.in().set_gain(250));  // clamped
  CHECK(rig.in().gain() == 100u);
  CHECK(rig.in().select(Source::MicHigh, 50));
  CHECK(rig.in().gain() == 50u);
  CHECK(rig.cap.gain() == 50u);
}

// ---- The monitor and the peaks ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("audio in: the monitor joins the master, in per cent of the input, off by default") {
  Rig rig(0.0f, false);  // an instrument that does not listen: only the monitor can put the input in the master
  REQUIRE(rig.started);
  rig.cap.input_tone(440.0f, -12.0f);
  rig.h.record(true);
  rig.h.run_ms(100);
  CHECK(rms(rig.h.audio(), 40, 100) == 0.0f);
  CHECK(rig.in().monitor() == 0u);
  rig.in().monitor(100);
  rig.h.run_ms(100);
  const float full = rms(rig.h.audio(), 140, 200);
  CHECK(full == doctest::Approx(kTone).epsilon(0.05));
  rig.in().monitor(app::AudioIn::kMonitorOn);
  CHECK(rig.h.engine().input_monitor() == doctest::Approx(0.7f));
  rig.h.run_ms(100);
  CHECK(rms(rig.h.audio(), 240, 300) == doctest::Approx(full * 0.7f).epsilon(0.05));
  rig.in().monitor(400);  // clamped to 100
  CHECK(rig.in().monitor() == 100u);
  rig.in().monitor(0);
  rig.h.run_ms(100);
  CHECK(rms(rig.h.audio(), 340, 400) == 0.0f);
  CHECK(rig.h.engine().feature_work().monitor_frames > 0u);
}

TEST_CASE("audio in: the peaks are measured only while somebody asks, and the Config's AUDIO section asks while it is open") {
  Rig rig(0.0f, false);
  REQUIRE(rig.started);
  rig.cap.input_tone(440.0f, -6.0f);
  rig.h.run_ms(100);
  CHECK_FALSE(rig.h.engine().input_peaks_on());
  CHECK(rig.h.engine().feature_work().peak_frames == 0u);
  const dsp::InputPeaks first = rig.in().peaks();  // the first call: zeros, and the measurement starts
  CHECK(first.l == 0.0f);
  rig.h.run_ms(20);
  CHECK(rig.h.engine().input_peaks_on());
  const dsp::InputPeaks p = rig.in().peaks();
  CHECK(p.l == doctest::Approx(0.5012f).epsilon(0.05));
  CHECK(p.r == doctest::Approx(0.5012f).epsilon(0.05));
  CHECK(rig.in().peaks().l == 0.0f);  // the read cleared it
  // The app stops asking: after a second the measurement stops (and costs nothing).
  rig.h.run_ms(1100);
  CHECK_FALSE(rig.h.engine().input_peaks_on());
  const std::uint32_t frames = rig.h.engine().feature_work().peak_frames;
  rig.h.run_ms(100);
  CHECK(rig.h.engine().feature_work().peak_frames == frames);
  // The AUDIO section: open, the meter is fed; closed, it stops.
  rig.tap(Button::Config);
  REQUIRE(rig.state().find("section=audio") != std::string::npos);
  rig.h.run_ms(50);
  CHECK(rig.h.engine().input_peaks_on());
  CHECK(rig.in().meter().l == doctest::Approx(0.5012f).epsilon(0.05));
  rig.tap(Button::Config);
  rig.h.run_ms(5);
  CHECK_FALSE(rig.h.engine().input_peaks_on());
}

// ---- A codec that does not answer --------------------------------------------------------------------------------------------------------------------

TEST_CASE("audio in: a refused codec write is tried again every 500 ms, the gate stays shut until it went through, and nothing waits") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.input_tone(440.0f, -12.0f);
  rig.h.run_ms(50);
  rig.cap.refuse_writes(2);
  CHECK_FALSE(rig.in().select(Source::MicHigh, 30));
  CHECK(rig.in().failed());
  CHECK(rig.in().source() == Source::MicHigh);  // the choice stands
  CHECK(rig.cap.selects() == 1u);
  rig.h.run_ms(450);
  CHECK(rig.cap.selects() == 1u);  // not before 500 ms
  rig.h.run_ms(100);
  CHECK(rig.cap.selects() == 2u);  // the first retry: refused again
  CHECK(rig.in().failed());
  CHECK_FALSE(rig.h.engine().input_gate());
  rig.h.run_ms(500);
  CHECK(rig.cap.selects() == 3u);  // the second: through
  CHECK_FALSE(rig.in().failed());
  CHECK(rig.cap.source() == Source::MicHigh);
  CHECK(rig.cap.gain() == 30u);
  CHECK(rig.in().settling());
  CHECK_FALSE(rig.h.engine().input_gate());
  rig.h.run_ms(250);
  CHECK(rig.h.engine().input_gate());
  rig.h.run_ms(2000);
  CHECK(rig.cap.selects() == 3u);  // and no more
  // A gain that is refused is retried the same way, and never closes the gate.
  rig.cap.refuse_writes(1);
  CHECK_FALSE(rig.in().set_gain(10));
  CHECK(rig.in().failed());
  CHECK(rig.in().gain() == 10u);
  CHECK(rig.h.engine().input_gate());
  rig.h.run_ms(600);
  CHECK_FALSE(rig.in().failed());
  CHECK(rig.cap.gain() == 10u);
  CHECK(rig.h.engine().input_gate());
}

// ---- The radio ----------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("radio: a source of Radio powers it up, a request made while it starts is held and runs when it is ready") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.radio_stations({8850, 9370, 10110});
  CHECK(rig.radio().available());
  CHECK(rig.radio().status().state == RState::Off);
  CHECK(rig.radio().usable());
  rig.radio().tune(9373);  // snapped to a channel of Europe (100 kHz)
  CHECK(rig.radio().freq() == 9370u);
  CHECK(rig.in().select(Source::Radio, 0));
  CHECK(rig.cap.power_calls() == 1u);
  CHECK(rig.cap.mux_radio());
  rig.h.run_ms(100);
  CHECK(rig.radio().status().state == RState::Starting);
  rig.radio().seek(true);  // asked while it is starting: held
  const auto until = [&](RState s, unsigned max_ms) {
    for (unsigned i = 0; i < max_ms && rig.radio().status().state != s; ++i) {
      rig.h.run_ms(1);
    }
    return rig.radio().status().state == s;
  };
  CHECK(until(RState::Seeking, 1000));
  CHECK(until(RState::Ready, 1000));
  CHECK(rig.radio().freq() == 10110u);  // the next station up from 93.70
}

TEST_CASE("radio: tune, step and seek move the frequency the radio reports and the app reads") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.radio_stations({8850, 9370, 10110});
  REQUIRE(rig.in().select(Source::Radio, 0));
  rig.h.run_ms(900);
  REQUIRE(rig.radio().status().state == RState::Ready);
  CHECK(rig.radio().freq() == 8750u);
  rig.radio().seek(true);
  CHECK(rig.h.runtime().context().radio().status().state != RState::Off);
  rig.h.run_ms(50);
  CHECK(rig.radio().status().state == RState::Seeking);
  rig.h.run_ms(500);
  CHECK(rig.radio().status().state == RState::Ready);
  CHECK(rig.radio().freq() == 8850u);  // followed what the chip reports
  CHECK(rig.radio().status().rssi == 40u);
  rig.radio().seek(true);
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 9370u);
  rig.radio().seek(false);
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 8850u);
  rig.radio().seek(false);  // wraps at the bottom of the band
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 10110u);
  rig.radio().step(-3);
  CHECK(rig.radio().freq() == 10080u);
  rig.radio().step(5000);  // clamped to the top of the band
  CHECK(rig.radio().freq() == 10800u);
  rig.radio().tune(100);  // and the bottom
  CHECK(rig.radio().freq() == 8750u);
  rig.h.run_ms(200);
  CHECK(rig.radio().status().state == RState::Ready);
  CHECK(rig.radio().status().freq == 8750u);
  // The region: its own band and spacing; the frequency made valid for it.
  rig.radio().tune(9380);
  rig.radio().set_region(hal::capture::Region::Usa);  // 200 kHz channels from 87.50: 93.80 is not one, 93.90 is the nearest
  CHECK(rig.radio().region() == hal::capture::Region::Usa);
  CHECK(rig.radio().freq() == 9390u);
  rig.radio().set_region(hal::capture::Region::Japan);  // from 76.00
  rig.radio().tune(7600);
  CHECK(rig.radio().freq() == 7600u);
  rig.radio().set_region(static_cast<hal::capture::Region>(9));  // refused
  CHECK(rig.radio().region() == hal::capture::Region::Japan);
}

TEST_CASE("radio: a receiver that does not answer is Absent, nothing retries by itself, and a person's power(true) tries once more") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.radio_absent();
  REQUIRE(rig.in().select(Source::Radio, 0));
  rig.h.run_ms(100);
  CHECK(rig.radio().status().state == RState::Absent);
  CHECK_FALSE(rig.radio().usable());
  CHECK(rig.state().find("radio=absent") != std::string::npos);
  CHECK(rig.cap.power_calls() == 1u);
  rig.radio().seek(true);  // does nothing to a receiver that is not there
  rig.radio().tune(9370);
  rig.h.run_ms(4000);
  CHECK(rig.cap.power_calls() == 1u);  // not asked again in four seconds
  CHECK(rig.radio().status().state == RState::Absent);
  CHECK(rig.cap.seeks() == 1u);  // (the call reached the device, which ignored it)
  rig.radio().power(true);
  CHECK(rig.cap.power_calls() == 2u);
  rig.h.run_ms(100);
  CHECK(rig.radio().status().state == RState::Absent);
  rig.cap.radio_absent(false);  // a receiver that turns out to be there answers a retry
  rig.radio().power(true);
  rig.h.run_ms(900);
  CHECK(rig.radio().status().state == RState::Ready);
  CHECK(rig.radio().usable());
  // The radio off again: the chip back in reset.
  rig.radio().power(false);
  CHECK(rig.radio().status().state == RState::Off);
}

// ---- The kit's settings -----------------------------------------------------------------------------------------------------------------------------

TEST_CASE("settings: audio in and the radio are keys 10 to 14 of SETS, round trip, and an old file keeps the defaults") {
  using namespace app::settings;
  static_assert(kInputSource == 10 && kInputGain == 11 && kInputMonitor == 12 && kRadioRegion == 13 && kRadioFreq == 14 && kKeyCount == 14, "keys are appended, never reused");
  app::KitValues v;
  v.in_source = static_cast<std::uint8_t>(Source::MicHigh);
  v.in_gain = 37;
  v.in_monitor = 70;
  v.radio_region = static_cast<std::uint8_t>(hal::capture::Region::Japan);
  v.radio_freq = 8210;
  std::uint8_t buf[kMaxBytes];
  const std::size_t n = write(v, buf, sizeof buf);
  REQUIRE(n != 0u);
  app::KitValues back;
  Report rep;
  REQUIRE(read(buf, n, back, rep) == Result::Ok);
  CHECK(back == v);
  CHECK(rep.applied == kKeyCount);
  CHECK(rep.skipped_keys == 0u);
  CHECK(rep.skipped_values == 0u);
  // The defaults are an untouched input.
  const app::KitValues d;
  CHECK(d.in_source == 0u);
  CHECK(d.in_gain == 0u);
  CHECK(d.in_monitor == 0u);
  CHECK(d.radio_region == 0u);
  CHECK(d.radio_freq == 8750u);
  CHECK(d != v);
}

TEST_CASE("settings: a value out of range for an audio key is skipped and counted, the setting keeps its value") {
  using namespace app::settings;
  const auto file = [](std::uint16_t key, float value) {
    std::vector<std::uint8_t> buf(256);
    formats::chunk::Writer w(buf.data(), buf.size(), "O3SK");
    REQUIRE(w.begin("SETS"));
    REQUIRE(w.u16(kSetsVersion));
    REQUIRE(w.u16(1));
    REQUIRE(w.u16(key));
    REQUIRE(w.f32(value));
    REQUIRE(w.end());
    buf.resize(w.finish());
    return buf;
  };
  struct Case {
    std::uint16_t key;
    float value;
    bool ok;
  };
  const Case cases[] = {{kInputSource, 6.0f, true},    {kInputSource, 7.0f, false},     {kInputGain, 100.0f, true},     {kInputGain, 101.0f, false},
                        {kInputMonitor, 100.0f, true}, {kInputMonitor, 200.0f, false},  {kRadioRegion, 3.0f, true},     {kRadioRegion, 4.0f, false},
                        {kRadioFreq, 7600.0f, true},   {kRadioFreq, 10800.0f, true},    {kRadioFreq, 7599.0f, false},   {kRadioFreq, 10801.0f, false},
                        {kRadioFreq, 70000.0f, false}, {kInputGain, 12.5f, false},      {kInputGain, -1.0f, false}};
  for (const Case& c : cases) {
    const std::vector<std::uint8_t> bytes = file(c.key, c.value);
    app::KitValues v;
    Report rep;
    REQUIRE(read(bytes.data(), bytes.size(), v, rep) == Result::Ok);
    CHECK(rep.applied == (c.ok ? 1u : 0u));
    CHECK(rep.skipped_values == (c.ok ? 0u : 1u));
    if (!c.ok) {
      CHECK(v == app::KitValues{});
    }
  }
}

namespace {
std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
}  // namespace

TEST_CASE("audio in: the user's choice is kept on the card, and applied over the app's defaults at the next start") {
  host::ScratchDir card_dir("ai-card");
  REQUIRE(card_dir.ok());
  host::DirCard card(card_dir.path());
  Rig rig(0.0f, true, true, &card);
  REQUIRE(rig.started);
  CHECK(rig.cap.selects() == 0u);  // a card with no settings: the defaults, the codec untouched
  rig.cap.radio_stations({9370});
  REQUIRE(rig.in().select(Source::Radio, 40));
  rig.in().monitor(55);
  rig.h.run_ms(900);
  rig.radio().tune(9370);
  rig.h.run_ms(100);
  rig.h.run_ms(2000);  // the debounce (1.5 s) and the write
  const std::string file = card_dir.path() + "/Apps/aiprobe/system.o3s";
  REQUIRE(std::filesystem::exists(file));
  const unsigned selects_before = rig.cap.selects();
  CHECK(rig.h.reboot());
  CHECK(rig.in().source() == Source::Radio);
  CHECK(rig.in().gain() == 40u);
  CHECK(rig.in().monitor() == 55u);
  CHECK(rig.radio().freq() == 9370u);
  CHECK(rig.cap.selects() == selects_before + 1u);  // applied once, with the gate shut and the receiver powered
  CHECK(rig.cap.source() == Source::Radio);
  CHECK(rig.h.engine().input_monitor() == doctest::Approx(0.55f));
  rig.h.run_ms(1000);
  CHECK(rig.radio().status().state == RState::Ready);
  CHECK(rig.radio().status().freq == 9370u);
  // A card with nothing of it (an app that used none) costs the codec nothing at start.
  {
    host::ScratchDir other("ai-card2");
    REQUIRE(other.ok());
    host::DirCard card2(other.path());
    Rig plain(0.0f, true, true, &card2);
    REQUIRE(plain.started);
    plain.h.run_ms(100);
    CHECK(plain.cap.selects() == 0u);
    CHECK(plain.h.engine().input_gate());
    CHECK(plain.h.engine().input_monitor() == 0.0f);
  }
}

TEST_CASE("audio in: a card written by a unit with a capture device keeps its audio values when read with none") {
  host::ScratchDir card_dir("ai-card3");
  REQUIRE(card_dir.ok());
  const std::string file = card_dir.path() + "/Apps/aiprobe/system.o3s";
  {
    host::DirCard card(card_dir.path());
    Rig rig(0.0f, true, true, &card);
    REQUIRE(rig.started);
    REQUIRE(rig.in().select(Source::MicLow, 20));
    rig.in().monitor(30);
    rig.h.run_ms(2500);
    REQUIRE(std::filesystem::exists(file));
  }
  {
    host::DirCard card(card_dir.path());
    Rig rig(0.0f, false, false, &card);  // no capture device here
    REQUIRE(rig.started);
    rig.h.run_ms(100);
    CHECK(rig.in().source() == Source::MicLow);  // kept, not applied
    CHECK(rig.in().monitor() == 30u);
    CHECK(rig.h.engine().input_monitor() == 0.0f);  // (nothing of it reaches the render)
    CHECK(rig.h.engine().input_gate());
    rig.tap(Button::Config);  // a theme change writes the file again: the audio values must survive it
    rig.tap(Button::Down);
    rig.h.run_ms(2500);
  }
  app::KitValues v;
  app::settings::Report rep;
  const std::string bytes = read_all(file);
  REQUIRE(app::settings::read(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), v, rep) == app::settings::Result::Ok);
  CHECK(v.in_source == static_cast<std::uint8_t>(Source::MicLow));
  CHECK(v.in_gain == 20u);
  CHECK(v.in_monitor == 30u);
}

// ---- The Config's AUDIO section ------------------------------------------------------------------------------------------------------------------------

TEST_CASE("AUDIO section: INPUT, GAIN and MONITOR with the encoder and F1, the radio's rows only for the radio") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.radio_stations({8850, 9370});
  rig.tap(Button::Config);
  CHECK(rig.state().find("sys=1") != std::string::npos);
  CHECK(rig.state().find("section=audio") != std::string::npos);  // first: there is no update path here
  // INPUT: the encoder steps the source (LINE, LINE LEFT, ...), clamped; F1 steps and wraps.
  rig.h.turn(1);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::LineLeft);
  CHECK(rig.cap.source() == Source::LineLeft);
  rig.h.turn(-9);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::LineMix);
  rig.tap(Button::F1);
  CHECK(rig.in().source() == Source::LineLeft);
  rig.h.turn(-1);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::LineMix);
  // GAIN: one per cent a detent, five with Shift.
  rig.tap(Button::Down);
  rig.h.turn(7);
  rig.h.run_ms(1);
  CHECK(rig.in().gain() == 7u);
  rig.h.press(Button::Shift);
  rig.h.run_ms(1);
  rig.h.turn(2);
  rig.h.run_ms(1);
  rig.h.release(Button::Shift);
  rig.h.run_ms(1);
  CHECK(rig.in().gain() == 17u);
  CHECK(rig.cap.gain() == 17u);
  CHECK(rig.cap.gains() == 2u);
  // MONITOR: F1 is off / on at 70, the encoder a per cent a detent.
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.in().monitor() == 70u);
  rig.h.turn(-10);
  rig.h.run_ms(1);
  CHECK(rig.in().monitor() == 60u);
  rig.tap(Button::F1);
  CHECK(rig.in().monitor() == 0u);
  // No radio rows, and no seek keys, while the input is not the radio.
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::F6);
  CHECK(rig.cap.seeks() == 0u);
  // The radio: the rows and F5 / F6 appear. INPUT (up three rows) to RADIO.
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.h.turn(5);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::Radio);
  rig.h.run_ms(900);
  rig.tap(Button::F6);
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 8850u);
  rig.tap(Button::F6);
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 9370u);
  rig.tap(Button::F5);
  rig.h.run_ms(500);
  CHECK(rig.radio().freq() == 8850u);
  // FREQ (the fourth row): a channel a detent, ten with Shift. REGION (fifth): F1 steps and wraps. SEEK (sixth): the encoder seeks in its direction.
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.tap(Button::Down);
  rig.h.turn(2);
  rig.h.run_ms(1);
  CHECK(rig.radio().freq() == 8870u);
  rig.tap(Button::Down);
  rig.tap(Button::F1);
  CHECK(rig.radio().region() == hal::capture::Region::Usa);
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  rig.tap(Button::F1);
  CHECK(rig.radio().region() == hal::capture::Region::Europe);  // wrapped
  rig.h.run_ms(300);
  rig.tap(Button::Down);
  const unsigned seeks = rig.cap.seeks();
  rig.h.turn(-1);
  rig.h.run_ms(1);
  CHECK(rig.cap.seeks() == seeks + 1u);
  // The cursor is kept inside the rows when the input leaves the radio.
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.tap(Button::Up);
  rig.h.turn(-5);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::LineMix);
}

TEST_CASE("AUDIO section: a radio that is not there is greyed, skipped by INPUT, and F5 retries it") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.cap.radio_absent();
  rig.tap(Button::Config);
  rig.h.turn(5);
  rig.h.run_ms(100);
  REQUIRE(rig.in().source() == Source::Radio);
  CHECK(rig.radio().status().state == RState::Absent);
  rig.tap(Button::F6);  // no seek key for a radio that is not there
  CHECK(rig.cap.seeks() == 0u);
  CHECK(rig.cap.power_calls() == 1u);
  rig.tap(Button::F5);
  CHECK(rig.cap.power_calls() == 2u);  // the retry a person asked for
  rig.h.run_ms(100);
  rig.h.turn(-1);
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::MicHigh);
  rig.h.turn(1);  // does not step back onto the radio
  rig.h.run_ms(1);
  CHECK(rig.in().source() == Source::MicHigh);
  rig.h.run_ms(2000);
  CHECK(rig.cap.power_calls() == 2u);
}

TEST_CASE("AUDIO section: the pure steps of the rows") {
  using hal::capture::Source;
  // (system_pages.h is private to kit/app/src: the behaviour is what the section does with the keys, above; here the clamps of the percentage rows through the section's keys)
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  rig.tap(Button::Config);
  rig.tap(Button::Down);
  rig.h.turn(100);  // GAIN clamps at 100
  rig.h.run_ms(1);
  CHECK(rig.in().gain() == 100u);
  rig.h.turn(-100);
  rig.h.run_ms(1);
  CHECK(rig.in().gain() == 0u);
  rig.tap(Button::F1);  // F1 wraps by one: 1
  CHECK(rig.in().gain() == 1u);
  rig.tap(Button::Down);
  rig.h.turn(100);
  rig.h.run_ms(1);
  CHECK(rig.in().monitor() == 100u);
  rig.h.turn(-100);
  rig.h.run_ms(1);
  CHECK(rig.in().monitor() == 0u);
}

TEST_CASE("audio in: the state line says the choice, the gate and the radio, and only with a capture device") {
  Rig rig(0.0f, true);
  REQUIRE(rig.started);
  CHECK(rig.state().find("in=line gain=0 mon=0 gate=1 radio=off freq=8750") != std::string::npos);
  REQUIRE(rig.in().select(Source::MicHigh, 33));
  rig.in().monitor(5);
  CHECK(rig.state().find("in=michigh gain=33 mon=5 gate=0 radio=off freq=8750") != std::string::npos);
  rig.h.run_ms(300);
  CHECK(rig.state().find("gate=1") != std::string::npos);
}

#ifdef O3SDK_HELLO_SCENARIOS
// ---- HELLO ---------------------------------------------------------------------------------------------------------------------------------------------

namespace {
std::string read_scenario(const std::string& name) {
  std::ifstream in(std::string(O3SDK_HELLO_SCENARIOS) + "/" + name);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
}  // namespace

TEST_CASE("sim audio in: scenarios/audio_in.txt runs as it is, and the master holds the tone, the gap, the louder tone and the radio's stations") {
  host::ScratchDir out_dir("ai-out");
  REQUIRE(out_dir.ok());
  host::FakeCapture cap;
  sim::Harness h(o3sdk_app(), nullptr, nullptr, nullptr, nullptr, &cap);
  REQUIRE(h.start());
  const std::string text = read_scenario("audio_in.txt");
  REQUIRE_FALSE(text.empty());
  const sim::ScenarioResult r = sim::run_scenario(h, text, out_dir.path());
  INFO(r.error << " (line " << r.line << ")");
  CHECK(r.ok);
  CHECK(r.snapshots == 10u);
  CHECK(cap.seeks() == 3u);  // F6, F6 and F5
  // The master in 10 ms windows (the riff is not playing: only the monitored input is there).
  const std::vector<std::int16_t>& a = h.audio();
  REQUIRE(a.size() > 2u * 44100u);
  const unsigned windows = static_cast<unsigned>(static_cast<float>(a.size() / 2u) / core::kRefRateHz * 100.0f);
  std::vector<float> level;
  for (unsigned w = 0; w < windows; ++w) {
    level.push_back(rms(a, w * 10u, w * 10u + 10u));
  }
  // Nothing before the monitor is on; then the tone at 70 % of -12 dBFS; then a silent run of at least 150 ms (the INPUT switch settling) with the tone on both sides.
  unsigned first = 0;
  while (first < level.size() && level[first] < 1e-3f) {
    ++first;
  }
  CHECK(first >= 10u);
  REQUIRE(first + 100u < level.size());
  const float tone = level[first + 10u];
  CHECK(tone == doctest::Approx(kTone * 0.7f).epsilon(0.1));
  unsigned gap_from = first + 10u;
  while (gap_from < level.size() && level[gap_from] > 1e-3f) {
    ++gap_from;
  }
  unsigned gap_to = gap_from;
  while (gap_to < level.size() && level[gap_to] < 1e-3f) {
    ++gap_to;
  }
  REQUIRE(gap_to < level.size());
  CHECK(gap_to - gap_from >= 15u);
  CHECK(gap_to - gap_from <= 30u);
  CHECK(level[gap_to + 10u] == doctest::Approx(tone).epsilon(0.05));  // the same tone after it settled
  // GAIN 20 per cent is three steps of 1.5 dB: the tone is 1.68 times as loud somewhere after the gap.
  bool louder = false;
  for (unsigned w = gap_to + 10u; w < level.size(); ++w) {
    louder = louder || (level[w] > tone * 1.6f && level[w] < tone * 1.76f);
  }
  CHECK(louder);
  // The radio: its stations are tones at freq / 10 Hz (88.5 MHz is 885 Hz, 93.7 is 937 Hz); 440 Hz is the line's tone. The frequency of a window by its zero crossings.
  unsigned seen_440 = 0;
  unsigned seen_885 = 0;
  unsigned seen_937 = 0;
  const std::size_t per_window = static_cast<std::size_t>(core::kRefRateHz / 10.0f);  // 100 ms
  for (std::size_t at = 0; at + per_window <= a.size() / 2u; at += per_window) {
    unsigned crossings = 0;
    float peak = 0.0f;
    for (std::size_t i = at + 1; i < at + per_window; ++i) {
      crossings += (a[2u * i] >= 0) != (a[2u * (i - 1u)] >= 0) ? 1u : 0u;
      peak = std::fmax(peak, std::fabs(static_cast<float>(a[2u * i])) / 32768.0f);
    }
    if (peak < 0.05f) {
      continue;
    }
    const float hz = static_cast<float>(crossings) * 0.5f * 10.0f;
    seen_440 += std::fabs(hz - 440.0f) < 15.0f ? 1u : 0u;
    seen_885 += std::fabs(hz - 885.0f) < 15.0f ? 1u : 0u;
    seen_937 += std::fabs(hz - 937.0f) < 15.0f ? 1u : 0u;
  }
  CHECK(seen_440 >= 5u);
  CHECK(seen_885 >= 3u);
  CHECK(seen_937 >= 3u);
}

TEST_CASE("sim audio in: scenarios/audio_in_absent.txt runs as it is") {
  host::ScratchDir out_dir("ai-out2");
  REQUIRE(out_dir.ok());
  host::FakeCapture cap;
  sim::Harness h(o3sdk_app(), nullptr, nullptr, nullptr, nullptr, &cap);
  REQUIRE(h.start());
  const sim::ScenarioResult r = sim::run_scenario(h, read_scenario("audio_in_absent.txt"), out_dir.path());
  INFO(r.error << " (line " << r.line << ")");
  CHECK(r.ok);
  CHECK(r.snapshots == 3u);
  CHECK(cap.power_calls() == 2u);  // the first request and the one retry the scenario made: nothing else asked
}

TEST_CASE("sim audio in: HELLO without a capture device renders what it did (the scenario lines that need one fail with a message)") {
  host::ScratchDir out_dir("ai-out3");
  REQUIRE(out_dir.ok());
  sim::Harness h(o3sdk_app());
  REQUIRE(h.start());
  const sim::ScenarioResult r = sim::run_scenario(h, "input tone 440\n", out_dir.path());
  CHECK_FALSE(r.ok);
  CHECK(r.line == 1);
  CHECK(r.error.find("no capture device") != std::string::npos);
  const sim::ScenarioResult q = sim::run_scenario(h, "radio absent\n", out_dir.path());
  CHECK_FALSE(q.ok);
}
#endif
