// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 half of the input drivers (OS-007d): see device.h. Pins, the FTM2 quadrature decoder and the two bus masters; everything
// above it is the pure code of the scan and the LED driver.
//
// Pin modes (hardware safety): the two INT lines and the encoder pins are INPUTS and stay inputs (nothing here sets a GPIO direction
// bit to output); the I2C pins are open drain, owned by the I2C master (i2c.cpp), which drives a line low only and releases it. The
// encoder's pull-down and the INT lines' pull-ups are the board's (see pins.h). The SD detect, the power switch and the encoder push are
// not touched: they have no role in this slice.
#include <o3sdk/platform/mk66/input/device.h>

#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3sdk/platform/mk66/input/pins.h>
#include <o3sdk/platform/mk66/input/soc_facts.h>
#include <o3sdk/platform/mk66/platform.h>

#include <initializer_list>

namespace o3sdk::platform::mk66::input::device {
namespace {

using regs::at;

volatile std::uint32_t& ftm2(std::uintptr_t offset) noexcept { return at<std::uint32_t>(soc::kFtm2Base + offset); }
volatile std::uint32_t& sim_scgc3() noexcept { return at<std::uint32_t>(soc::kScgc3Register); }

void configure_pins() noexcept {
  for (const Pin pin : {pins::kPadsInt, pins::kButtonsInt}) {
    regs::gpio_pddr(pin) = regs::gpio_pddr(pin) & ~regs::mask_of(pin);  // an input, explicitly (it is one after reset)
    regs::port_pcr(pin) = soc::kPcrInterruptInput;
  }
  regs::port_pcr(pins::kEncoderA) = soc::kPcrEncoder;
  regs::port_pcr(pins::kEncoderB) = soc::kPcrEncoder;
}

// The decoder counts, this tree reads the counter (no compare interrupt).
void start_encoder() noexcept {
  sim_scgc3() = sim_scgc3() | soc::kScgc3Ftm2;
  ftm2(soc::kFtmMode) = soc::kModeWpdis;
  ftm2(soc::kFtmMode) = soc::kModeWpdis | soc::kModeFtmen;
  ftm2(soc::kFtmCnt) = 0;
  ftm2(soc::kFtmMod) = 0;
  ftm2(soc::kFtmC0Sc) = 0;
  ftm2(soc::kFtmC1Sc) = 0;
  ftm2(soc::kFtmSc) = 0;
  ftm2(soc::kFtmFilter) = soc::kFilterValue;
  ftm2(soc::kFtmCntin) = 0;
  ftm2(soc::kFtmMod) = soc::kModulo;
  ftm2(soc::kFtmCnt) = 0;
  ftm2(soc::kFtmCombine) = 0;
  ftm2(soc::kFtmQdctrl) = soc::kQdctrlValue;
}

bool line_low(Pin pin) noexcept { return (regs::gpio_pdir(pin) & regs::mask_of(pin)) == 0u; }

// OS-BUG-PADS: the level of a pin, read only, no register written. GPIO_PDIR shows the pin whenever PORTx_PCRn[MUX] is a digital function (K66 Sub-Family Reference Manual, GPIOx_PDIR: "0 pin logic
// level is logic 0, or is not configured for use by digital function"), the I2C master's ALT5 among them: `diag radio` reads the control bus's PTD8 / PTD9 (ALT2) the same way (capture.cpp).
bool pin_high(Pin pin) noexcept { return (regs::gpio_pdir(pin) & regs::mask_of(pin)) != 0u; }

// The level of a pin whose mux may still be 0 (disabled: PTA12 / PTA13 before the master takes them), which reads 0 whatever the line does. Only the read right before the master's first
// initialisation uses it (LineReader::read_before_init): the mux is set to GPIO for the read (an input: the direction bit is not touched, no pull is added) and put back, and the master's
// initialisation, which follows at once, rewrites both PCRs. A pin on a digital function is read as it is, untouched.
bool pin_high_unclaimed(Pin pin) noexcept {
  volatile std::uint32_t& pcr = regs::port_pcr(pin);
  const std::uint32_t saved = pcr;
  if (((saved >> regs::kPcrMuxShift) & 7u) != 0u) {
    return pin_high(pin);
  }
  pcr = saved | regs::kPcrMuxGpio;
  const bool high = pin_high(pin);
  pcr = saved;
  return high;
}

class BoardLines final : public LineReader {
 public:
  std::uint8_t read() noexcept override {
    return static_cast<std::uint8_t>((pin_high(pins::kPadBusScl) ? hal::PadBusStats::kLineScl : 0u) |
                                     (pin_high(pins::kPadBusSda) ? hal::PadBusStats::kLineSda : 0u) |
                                     (pin_high(pins::kPadsInt) ? hal::PadBusStats::kLineInt : 0u));
  }
  std::uint8_t read_before_init() noexcept override {
    return static_cast<std::uint8_t>((pin_high_unclaimed(pins::kPadBusScl) ? hal::PadBusStats::kLineScl : 0u) |
                                     (pin_high_unclaimed(pins::kPadBusSda) ? hal::PadBusStats::kLineSda : 0u) |
                                     (pin_high(pins::kPadsInt) ? hal::PadBusStats::kLineInt : 0u));  // (INT is a GPIO input since configure_pins)
  }
};

BoardLines& lines() noexcept {
  static BoardLines l;
  return l;
}

// The pad bus: the I2C2 master behind its gate, its retry policy and the record of what the two chips on it answered.
PadBus& pad_bus() noexcept {
  static PadBus b(i2c::bus(pins::kPadBus), lines(), system_clock());
  return b;
}

class BoardHardware final : public ScanHardware {
 public:
  bool pads_int_asserted() noexcept override { return line_low(pins::kPadsInt); }
  bool buttons_int_asserted() noexcept override { return line_low(pins::kButtonsInt); }
  std::uint16_t encoder_count() noexcept override { return static_cast<std::uint16_t>(ftm2(soc::kFtmCnt) & soc::kCountMask); }
};

BoardHardware& hardware() noexcept {
  static BoardHardware h;
  return h;
}

InitResult g_last_init;

}  // namespace

Scanner& scanner() noexcept {
  static Scanner s(pad_bus(), i2c::bus(pins::kControlBus), hardware(), system_clock());
  return s;
}

PadLeds& pad_leds() noexcept {
  static PadLeds l(pad_bus(), system_clock());
  return l;
}

hal::Input& input() noexcept { return scanner(); }
hal::Leds& leds() noexcept { return pad_leds(); }

void service() noexcept { scanner().service(); }

bool init() noexcept {
  configure_pins();
  start_encoder();
  pad_bus().arm(boot::rail_up_ms());   // the pad bus (I2C2) is not touched here: the gate opens a second after the rail and after the audio start, and the scan brings the chips up then (pad_bus.h)
  (void)i2c::init(pins::kControlBus);  // also the codec's bus: re-initialising it is harmless (i2c.cpp)
  Scanner& s = scanner();
  s.attach_pad_bus(pad_bus());
  s.begin();                           // the control buttons now (three attempts); the pad scanner is deferred until the gate
  g_last_init.buttons_online = (s.stats().online & hal::InputStats::kButtonsOnline) != 0u;
  return g_last_init.buttons_online;
}

InitResult last_init() noexcept { return g_last_init; }

void audio_started() noexcept { pad_bus().audio_started(); }

}  // namespace o3sdk::platform::mk66::input::device
