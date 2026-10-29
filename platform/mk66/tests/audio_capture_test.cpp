// SPDX-License-Identifier: GPL-3.0-or-later
//
// The codec side of the audio in (docs/M2C-PLAN.md section 5, slice AI; audio.cpp's audio_input_apply and capture.cpp's select_source): a source switch, a gain change and the unmute, mute and stop
// plans against a model of the SGTL5000's registers, with the one rule audio.cpp adds to them: CHIP_ANA_CTRL's SELECT_ADC (bit 2) is the last source switch's, whatever plan writes the register.
// What the sources write and how the percentages map is sgtl5000.h's (mk66_sgtl5000_test.cpp); what the hardware does with them is the unit's (`radio` and the loopback check). The model of
// Mk66Audio below is the rule of audio.cpp (apply_input, plan_write) written as the test sees it: a change to one is a change to the other.
#include <doctest/doctest.h>

#include <o3sdk/hal/capture.h>
#include <o3sdk/platform/mk66/sgtl5000.h>

#include <cstdint>
#include <initializer_list>

namespace sg = o3sdk::platform::mk66::sgtl5000;
namespace cap = o3sdk::hal::capture;

namespace {

struct Registers {
  std::uint16_t r[0x40] = {};
  unsigned writes = 0;
  bool refuse = false;  // the codec stops acknowledging
};

// audio.cpp's rule: the unmute, mute and stop plans write CHIP_ANA_CTRL with the select bit of the last input plan; an input plan's own write sets it.
class Model {
 public:
  explicit Model(Registers& regs) : regs_(regs) {}
  bool write(std::uint16_t reg, std::uint16_t value) {
    if (regs_.refuse) {
      return false;
    }
    regs_.r[reg & 0x3F] = value;
    ++regs_.writes;
    return true;
  }
  bool plan_write(const sg::Step& step) {
    return write(step.reg, step.reg == sg::kAnaCtrl ? sg::ana_ctrl_with_adc(step.value, adc_line_) : step.value);
  }
  bool apply_input(const sg::InputPlan& plan) {
    for (unsigned i = 0; i < plan.count; ++i) {
      if (!write(plan.step[i].reg, plan.step[i].value)) {
        return false;
      }
      if (plan.step[i].reg == sg::kAnaCtrl) {
        adc_line_ = plan.line;
      }
    }
    return true;
  }
  void unmute() {
    sg::Step step{};
    for (unsigned i = 0; sg::unmute_step(i, step); ++i) {
      plan_write(step);
    }
    enabled_ = true;
  }
  void mute() {
    enabled_ = false;
    for (const sg::Step& step : sg::kMute) {
      plan_write(step);
    }
  }
  void stop() {
    enabled_ = false;
    (void)sg::run_stop([this](const sg::Step& s) { return plan_write(s); }, [] {}, [](std::uint32_t) {});
  }
  // capture.cpp's choice of plan for a source (line family, microphone, radio through the line input, none for Usb), composed with the outputs' state.
  bool select(cap::Source source, unsigned gain) {
    switch (source) {
      case cap::Source::LineMix:
      case cap::Source::LineLeft:
      case cap::Source::LineRight:
      case cap::Source::Radio: return apply_input(sg::line_input_plan(gain, !enabled_));
      case cap::Source::MicLow:
      case cap::Source::MicHigh: return apply_input(sg::mic_input_plan(source == cap::Source::MicHigh, gain, !enabled_));
      case cap::Source::Usb:
      case cap::Source::Count: break;
    }
    return true;
  }
  bool enabled() const { return enabled_; }

 private:
  Registers& regs_;
  bool adc_line_ = true;
  bool enabled_ = false;
};

bool select_is_line(const Registers& r) { return (r.r[sg::kAnaCtrl] & sg::kAnaCtrlSelectLine) != 0u; }
bool outputs_muted(const Registers& r) {
  return (r.r[sg::kAnaCtrl] & (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone)) == (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone);
}
bool outputs_open(const Registers& r) { return (r.r[sg::kAnaCtrl] & (sg::kAnaCtrlMuteLineOut | sg::kAnaCtrlMuteHeadphone)) == 0u; }

}  // namespace

TEST_CASE("audio input: the unmute, mute and stop plans keep the select of the last source switch, for every source") {
  for (unsigned s = 0; s < cap::kSourceCount; ++s) {
    const auto source = static_cast<cap::Source>(s);
    Registers regs;
    Model audio(regs);
    audio.mute();  // the power-up state: the select is the line's
    CHECK(select_is_line(regs));
    CHECK(audio.select(source, 60));
    const bool line = source != cap::Source::MicLow && source != cap::Source::MicHigh;  // Usb leaves the codec as it was: the line
    CHECK(select_is_line(regs) == line);
    audio.unmute();
    CHECK(select_is_line(regs) == line);
    CHECK(outputs_open(regs));
    audio.mute();
    CHECK(select_is_line(regs) == line);
    CHECK(outputs_muted(regs));
    audio.unmute();
    audio.stop();
    CHECK(select_is_line(regs) == line);
    CHECK(outputs_muted(regs));
  }
}

TEST_CASE("audio input: a switch composed for the state of the outputs never opens muted ones nor mutes open ones") {
  for (bool open : {false, true}) {
    for (unsigned a = 0; a < cap::kSourceCount; ++a) {
      for (unsigned b = 0; b < cap::kSourceCount; ++b) {
        Registers regs;
        Model audio(regs);
        audio.mute();
        if (open) {
          audio.unmute();
        }
        CHECK(audio.select(static_cast<cap::Source>(a), 30));
        CHECK(audio.select(static_cast<cap::Source>(b), 90));
        CHECK(open ? outputs_open(regs) : outputs_muted(regs));
        CHECK((regs.r[sg::kAnaCtrl] & sg::kAnaCtrlMuteAdc) == 0u);  // the ADC listens
      }
    }
  }
}

TEST_CASE("audio input: the gain is written before the select, and a gain change touches only the gain") {
  Registers regs;
  Model audio(regs);
  audio.mute();
  const unsigned before = regs.writes;
  CHECK(audio.select(cap::Source::MicHigh, 100));
  CHECK(regs.writes - before == 3u);  // the preamp, the ADC steps, the select
  CHECK(regs.r[sg::kMicCtrl] == (sg::kMicCtrlBase | 3u));
  CHECK(regs.r[sg::kAnaAdcCtrl] == 0x0022);
  const std::uint16_t ana = regs.r[sg::kAnaCtrl];
  const unsigned gain_before = regs.writes;
  CHECK(audio.apply_input(sg::mic_gain_plan(true, 50)));  // 22 dB: +20 preamp, 2 dB = 1 step
  CHECK(regs.writes - gain_before == 2u);
  CHECK(regs.r[sg::kAnaCtrl] == ana);  // the select and the mutes untouched
  CHECK(regs.r[sg::kMicCtrl] == (sg::kMicCtrlBase | 1u));
  CHECK(regs.r[sg::kAnaAdcCtrl] == 0x0011);
  CHECK(audio.apply_input(sg::line_gain_plan(100)));
  CHECK(regs.r[sg::kAnaAdcCtrl] == 0x00FF);  // 15 steps in both channels
  CHECK(regs.r[sg::kAnaCtrl] == ana);
}

TEST_CASE("audio input: a codec that does not acknowledge fails the switch and does not move the select the plans keep") {
  Registers regs;
  Model audio(regs);
  audio.mute();
  CHECK(audio.select(cap::Source::MicLow, 10));
  CHECK_FALSE(select_is_line(regs));
  regs.refuse = true;
  CHECK_FALSE(audio.select(cap::Source::LineMix, 10));  // the first write fails: the select was not reached
  regs.refuse = false;
  audio.unmute();
  CHECK_FALSE(select_is_line(regs));  // still the microphone's
  CHECK(audio.select(cap::Source::LineMix, 10));  // the retry goes through
  audio.mute();
  CHECK(select_is_line(regs));
}
