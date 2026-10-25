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
  static Scanner s(i2c::bus(pins::kPadBus), i2c::bus(pins::kControlBus), hardware(), system_clock());
  return s;
}

PadLeds& pad_leds() noexcept {
  static PadLeds l(i2c::bus(pins::kPadBus), system_clock());
  return l;
}

hal::Input& input() noexcept { return scanner(); }
hal::Leds& leds() noexcept { return pad_leds(); }

void service() noexcept { scanner().service(); }

bool init() noexcept {
  configure_pins();
  start_encoder();
  (void)i2c::init(pins::kPadBus);      // a bus a slave holds is released here; a chip that still does not answer is the scan's to retry
  (void)i2c::init(pins::kControlBus);  // also the codec's bus: re-initialising it is harmless (i2c.cpp)
  Scanner& s = scanner();
  s.begin();
  const hal::InputStats stats = s.stats();
  g_last_init.pads_online = (stats.online & hal::InputStats::kPadsOnline) != 0u;
  g_last_init.buttons_online = (stats.online & hal::InputStats::kButtonsOnline) != 0u;
  return g_last_init.pads_online && g_last_init.buttons_online;
}

InitResult last_init() noexcept { return g_last_init; }

}  // namespace o3sdk::platform::mk66::input::device
