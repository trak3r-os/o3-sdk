// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board's capture control (OS-011c, capture.h): the input source switch and the FM radio. Ported from track3r-os platform/mk66/src/capture.cpp (M2c slice AI) with the names of this tree
// and the diag text's header of platform/mk66/audio; nothing else is changed.
//
// THE SWITCH. The codec's ADC has two inputs (CHIP_ANA_CTRL SELECT_ADC): the microphone and the line input. The line input is fed by an analogue switch from the line jack or
// from the Si4703's output, PTE26 (board.h kInputSourceSelect). The recorder drives it HIGH for the line (and for the microphone, where it does not matter and the radio
// is kept off the input) and LOW for the radio (the polarity assumed; platform/mk66/README.md and board.h said the opposite until OS-011c). The
// boot level, LOW, is the radio's position; the radio is in reset then, so nothing is heard. Which position the line jack's signal needs at boot is a hardware check.)
//
// THE RADIO. si4703.h: a state machine over I2C0; this file gives it the board's reset line (PTE27) and the SDA hold the chip's 2-wire select needs.
#include "internal.h"
#include "regs.h"

#include <o3sdk/hal/capture.h>
#include <o3sdk/platform/mk66/audio.h>
#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/capture.h>
#include <o3sdk/platform/mk66/sgtl5000.h>
#include <o3sdk/platform/mk66/si4703.h>

#include <new>

namespace o3sdk::platform::mk66 {
namespace {

namespace cap = o3sdk::hal::capture;

void mux_line() noexcept { regs::gpio_psor(board::kInputSourceSelect) = regs::mask_of(board::kInputSourceSelect); }
void mux_radio() noexcept { regs::gpio_pcor(board::kInputSourceSelect) = regs::mask_of(board::kInputSourceSelect); }

// The reset line (PTE27, an output driven low at boot by boot::early_pins) and the bus's SDA.
class BoardPins final : public si4703::Pins {
 public:
  bool reset() noexcept override {
    if (i2c::sda_hold(true) != i2c::Result::Ok) {
      return false;
    }
    regs::gpio_pcor(board::kRadioReset) = regs::mask_of(board::kRadioReset);  // already low at the first power-up; low again for a later one
    boot::delay_ms(si4703::kResetLowMs);
    regs::gpio_psor(board::kRadioReset) = regs::mask_of(board::kRadioReset);  // the chip samples SDIO (low) at this edge: 2-wire mode
    boot::delay_ms(si4703::kResetLowMs);
    return i2c::sda_hold(false) == i2c::Result::Ok;  // SDA released, the bus restored
  }
  void hold_reset() noexcept override { regs::gpio_pcor(board::kRadioReset) = regs::mask_of(board::kRadioReset); }
};

class Mk66Capture final : public cap::CaptureHw {
 public:
  Mk66Capture() noexcept : radio_(i2c::bus(i2c_cfg::Instance::I2c0), pins_) {}

  bool select_source(cap::Source source, std::uint8_t gain_percent) noexcept override {
    const bool muted = !audio_device().outputs_enabled();
    bool ok = true;
    switch (source) {
      case cap::Source::LineMix:
      case cap::Source::LineLeft:
      case cap::Source::LineRight:
        ok = audio_input_apply(sgtl5000::line_input_plan(gain_percent, muted));
        mux_line();
        break;
      case cap::Source::MicLow:
      case cap::Source::MicHigh:
        ok = audio_input_apply(sgtl5000::mic_input_plan(source == cap::Source::MicHigh, gain_percent, muted));
        mux_line();  // the line position keeps the receiver off the input
        break;
      case cap::Source::Radio:
        ok = audio_input_apply(sgtl5000::line_input_plan(gain_percent, muted));
        mux_radio();
        radio_.power(true);  // the receiver is powered when it is first wanted
        break;
      case cap::Source::Usb:
      case cap::Source::Count:
        break;  // no analogue part: the codec stays as it is
    }
    return ok;
  }

  bool set_gain(cap::Source source, std::uint8_t gain_percent) noexcept override {
    switch (source) {
      case cap::Source::LineMix:
      case cap::Source::LineLeft:
      case cap::Source::LineRight:
      case cap::Source::Radio:
        return audio_input_apply(sgtl5000::line_gain_plan(gain_percent));
      case cap::Source::MicLow:
      case cap::Source::MicHigh:
        return audio_input_apply(sgtl5000::mic_gain_plan(source == cap::Source::MicHigh, gain_percent));
      case cap::Source::Usb:
      case cap::Source::Count:
        break;
    }
    return true;
  }

  void radio_power(bool on) noexcept override { radio_.power(on); }
  void radio_region(cap::Region region) noexcept override { radio_.set_region(region); }
  void radio_tune(std::uint16_t freq) noexcept override { radio_.tune(freq); }
  void radio_seek(bool up) noexcept override { radio_.seek(up); }
  void service(std::uint64_t now_ms) noexcept override { radio_.service(now_ms); }
  cap::RadioStatus radio_status() const noexcept override { return radio_.status(); }

  // `diag radio` (OS-RAD1): the driver's text, then the levels of the lines it depends on (read, nothing is driven). SEN is not an MCU pin: the board's pull-up holds it high.
  std::size_t report(char* out, std::size_t cap) const noexcept {
    std::size_t n = radio_.describe(out, cap);
    audio::diag::Text t(out + n, cap - n);
    t.str("pins: RST# (PTE27) ").dec(level(board::kRadioReset)).str(", SDIO/SDA (PTD9) ").dec(level(board::kCodecSda)).str(", SCL (PTD8) ").dec(level(board::kCodecScl))
        .str(", PTE26 ").dec(level(board::kInputSourceSelect)).str(" (0 radio, 1 line), SEN pulled up on the board").nl();
    return n + t.size();
  }
  void restart() noexcept { radio_.restart(); }

 private:
  static unsigned level(Pin pin) noexcept { return (regs::gpio_pdir(pin) & regs::mask_of(pin)) != 0u ? 1u : 0u; }

  BoardPins pins_;  // before radio_: the radio holds a reference
  si4703::Radio radio_;
};

static_assert(sizeof(Mk66Capture) <= kCaptureStorageBytes && alignof(Mk66Capture) <= kCaptureStorageAlign, "the capture device outgrew its SDRAM storage (capture.h)");

}  // namespace

hal::capture::CaptureHw& capture_create(void* storage) noexcept { return *new (storage) Mk66Capture(); }

// The one CaptureHw of this image is a Mk66Capture (capture_create is the only way to get one).
std::size_t capture_radio_report(hal::capture::CaptureHw& hw, char* out, std::size_t cap) noexcept { return static_cast<Mk66Capture&>(hw).report(out, cap); }
void capture_radio_restart(hal::capture::CaptureHw& hw) noexcept { static_cast<Mk66Capture&>(hw).restart(); }

}  // namespace o3sdk::platform::mk66
