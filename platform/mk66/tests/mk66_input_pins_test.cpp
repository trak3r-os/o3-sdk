// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007d hardware facts: no pin of the input drivers is claimed by anything else on the board (the quiet pins, the display, the SDRAM,
// the audio), the one pair of pins shared on purpose is shared with the codec, and every register constant the hardware half writes
// (the I2C instances, the FTM2 quadrature decoder, the port control bits) is the value the fact tables give: the Teensy core's
// header and NXP's device header. A wrong bit here
// is a peripheral that silently never comes up. Runs on the host.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/i2c_config.h>
#include <o3sdk/platform/mk66/input/pins.h>
#include <o3sdk/platform/mk66/input/scan_config.h>
#include <o3sdk/platform/mk66/input/soc_facts.h>
#include <o3sdk/platform/mk66/sdramc_config.h>

#include <cstddef>
#include <cstdint>

namespace mk66 = o3sdk::platform::mk66;
namespace pins = o3sdk::platform::mk66::input::pins;
namespace soc = o3sdk::platform::mk66::input::soc;
namespace cfg = o3sdk::platform::mk66::i2c_cfg;

namespace {

struct Fact {
  const char* name;
  std::uint32_t ours;
  std::uint32_t expected;
  const char* cited;
};

bool same(const mk66::Pin& a, const mk66::Pin& b) { return a.port == b.port && a.bit == b.bit; }

}  // namespace

TEST_CASE("input pins: the claimed pins are all different") {
  for (unsigned i = 0; i < pins::kClaimedCount; ++i) {
    for (unsigned j = i + 1; j < pins::kClaimedCount; ++j) {
      CHECK_MESSAGE(!same(pins::kClaimed[i], pins::kClaimed[j]), "pins " << i << " and " << j);
    }
  }
}

TEST_CASE("input pins: nothing else on the board uses them, the control bus is the codec's by design") {
  const mk66::Pin elsewhere[] = {
      mk66::board::kStatusLed,     mk66::board::kAudioMute,    mk66::board::kSdramPower,    mk66::board::kFt812ChipSelect,
      mk66::board::kFt812PowerDown, mk66::board::kFt812Sck,    mk66::board::kFt812Mosi,     mk66::board::kFt812Miso,
      mk66::board::kInputSourceSelect, mk66::board::kRadioReset, mk66::board::kMidiTx,      mk66::board::kBootloaderRequest,
      mk66::board::kI2sMclk,       mk66::board::kI2sRxd,       mk66::board::kI2sTxd,        mk66::board::kI2sLrclk,
      mk66::board::kI2sBclk,
  };
  for (unsigned i = 0; i < pins::kClaimedCount; ++i) {
    const mk66::Pin mine = pins::kClaimed[i];
    for (const mk66::Pin& other : elsewhere) {
      CHECK_MESSAGE(!same(mine, other), "claimed pin " << i << " is also a board pin");
    }
    for (const mk66::Pin& sdram : mk66::sdram::kPins) {
      CHECK_MESSAGE(!same(mine, sdram), "claimed pin " << i << " is an SDRAM pin");
    }
  }
  // The two control-bus pins ARE the codec's: one bus, two clients (i2c_bus.h).
  CHECK(same(pins::kControlBusScl, mk66::board::kCodecScl));
  CHECK(same(pins::kControlBusSda, mk66::board::kCodecSda));
  // The grid bus is not.
  CHECK_FALSE(same(pins::kPadBusScl, mk66::board::kCodecScl));
  CHECK_FALSE(same(pins::kPadBusSda, mk66::board::kCodecSda));
}

TEST_CASE("input pins: the pin assignments") {
  // The encoder phases are PORTA_PCR10 and PORTA_PCR11.
  CHECK((pins::kEncoderA.port == mk66::Port::A && pins::kEncoderA.bit == 10));
  CHECK((pins::kEncoderB.port == mk66::Port::A && pins::kEncoderB.bit == 11));
  // via the Teensy core pin map (pin 38 = PORTC_PCR11) and (pin 14 = PORTD_PCR1)
  CHECK((pins::kPadsInt.port == mk66::Port::C && pins::kPadsInt.bit == 11));
  CHECK((pins::kButtonsInt.port == mk66::Port::D && pins::kButtonsInt.bit == 1));
  // the Teensy core pin map: pins 3 and 4 are PORTA_PCR12 and PORTA_PCR13; pins 47 and 48 are PORTD_PCR8 and PORTD_PCR9
  CHECK((pins::kPadBusScl.port == mk66::Port::A && pins::kPadBusScl.bit == 12));
  CHECK((pins::kPadBusSda.port == mk66::Port::A && pins::kPadBusSda.bit == 13));
  CHECK((pins::kControlBusScl.port == mk66::Port::D && pins::kControlBusScl.bit == 8));
  CHECK((pins::kControlBusSda.port == mk66::Port::D && pins::kControlBusSda.bit == 9));
  CHECK(pins::kEncoderMux == 6);
  CHECK(pins::kKeyScannerAddress == 0x34);
  CHECK(pins::kLedDriverAddress == 0x74);
  CHECK(pins::kPadBus == cfg::Instance::I2c2);
  CHECK(pins::kControlBus == cfg::Instance::I2c0);
}

TEST_CASE("input soc: the I2C instances, FTM2 and the port bits are the values the fact tables give") {
  const Fact facts[] = {
      {"I2C0 base", static_cast<std::uint32_t>(cfg::facts(cfg::Instance::I2c0).base), 0x40066000u, "kinetis.h KINETIS_I2C0"},
      {"I2C2 base", static_cast<std::uint32_t>(cfg::facts(cfg::Instance::I2c2).base), 0x400E6000u, "kinetis.h KINETIS_I2C2"},
      {"I2C0 clock gate register", static_cast<std::uint32_t>(cfg::facts(cfg::Instance::I2c0).scgc_register), 0x40048034u, "SIM_SCGC4; display/dev_regs.h; kinetis.h"},
      {"I2C2 clock gate register", static_cast<std::uint32_t>(cfg::facts(cfg::Instance::I2c2).scgc_register), 0x40048028u, "kinetis.h SIM_SCGC1"},
      {"I2C0 clock gate bit", cfg::facts(cfg::Instance::I2c0).scgc_bit, 0x00000040u, "kinetis.h SIM_SCGC4_I2C0"},
      {"I2C2 clock gate bit", cfg::facts(cfg::Instance::I2c2).scgc_bit, 0x00000040u, "kinetis.h SIM_SCGC1_I2C2"},
      {"I2C0 pin mux", cfg::facts(cfg::Instance::I2c0).mux, 2u, "Teensy 3.6 pin map: pins 47 and 48 at mux 2"},
      {"I2C2 pin mux", cfg::facts(cfg::Instance::I2c2).mux, 5u, "Teensy 3.6 pin map: pins 3 and 4 at mux 5"},
      {"I2C divider (240: 375 kHz)", cfg::kFrequencyDivider, 0x1Fu, "kinetis.h I2C_F_DIV240 = 0x1F"},
      {"SIM_SCGC3", soc::kScgc3Register, 0x40048030u, "kinetis.h; display/dev_regs.h sim_scgc3"},
      {"SIM_SCGC3 FTM2", soc::kScgc3Ftm2, 0x01000000u, "kinetis.h SIM_SCGC3_FTM2"},
      {"FTM2 base", static_cast<std::uint32_t>(soc::kFtm2Base), 0x400B8000u, "RM FTM2 base; kinetis.h"},
      {"FTM CNT", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmCnt), 0x400B8004u, "kinetis.h FTM2_CNT"},
      {"FTM MOD", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmMod), 0x400B8008u, "kinetis.h FTM2_MOD"},
      {"FTM C0SC", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmC0Sc), 0x400B800Cu, "kinetis.h FTM2_C0SC"},
      {"FTM C1SC", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmC1Sc), 0x400B8014u, "kinetis.h FTM2_C1SC"},
      {"FTM CNTIN", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmCntin), 0x400B804Cu, "kinetis.h FTM2_CNTIN"},
      {"FTM MODE", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmMode), 0x400B8054u, "kinetis.h FTM2_MODE"},
      {"FTM COMBINE", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmCombine), 0x400B8064u, "kinetis.h FTM2_COMBINE"},
      {"FTM FILTER", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmFilter), 0x400B8078u, "kinetis.h FTM2_FILTER"},
      {"FTM QDCTRL", static_cast<std::uint32_t>(soc::kFtm2Base + soc::kFtmQdctrl), 0x400B8080u, "kinetis.h FTM2_QDCTRL"},
      {"MODE: WPDIS", soc::kModeWpdis, 0x04u, "kinetis.h FTM_MODE_WPDIS"},
      {"MODE: FTMEN", soc::kModeFtmen, 0x01u, "kinetis.h FTM_MODE_FTMEN"},
      {"FILTER", soc::kFilterValue, 0x4444u, "a filter of 4 on all four channel inputs (soc_facts.h)"},
      {"QDCTRL", soc::kQdctrlValue, 0xC1u, "0b11000001 0xC1; kinetis.h PHAFLTREN 0x80, PHBFLTREN 0x40, QUADEN 0x01"},
      {"PCR MUX shift", soc::kPcrMuxShift, 8u, "kinetis.h PORT_PCR_MUX(n) << 8"},
      {"PCR PFE", soc::kPcrPfe, 0x10u, "kinetis.h PORT_PCR_PFE"},
      {"PCR PE", soc::kPcrPe, 0x02u, "kinetis.h PORT_PCR_PE"},
      {"PCR PS", soc::kPcrPs, 0x01u, "kinetis.h PORT_PCR_PS"},
      {"encoder PCR", soc::kPcrEncoder, 0x612u, "PORTA_PCR10 = PORTA_PCR11 = 0x00000612 (ALT6, filter, pull DOWN)"},
      {"INT line PCR (GPIO, pull-up)", soc::kPcrInterruptInput, 0x103u, "kinetis.h: MUX 1, PE, PS (a pull-up input)"},
  };
  for (const Fact& f : facts) {
    CHECK_MESSAGE(f.ours == f.expected, f.name << ": " << f.cited);
  }
}

TEST_CASE("input soc: the encoder pull is DOWN, the INT lines pull UP, and no pin config selects an output") {
  CHECK((soc::kPcrEncoder & soc::kPcrPs) == 0u);  // PS = 0 with PE = 1: pull-down
  CHECK((soc::kPcrEncoder & soc::kPcrPe) != 0u);
  CHECK((soc::kPcrInterruptInput & soc::kPcrPs) != 0u);
  CHECK((soc::kPcrInterruptInput & soc::kPcrPe) != 0u);
  // The mux of an INT line is GPIO (1); a pin is an output only by its direction register, which the hardware half never sets for these.
  CHECK(((soc::kPcrInterruptInput >> soc::kPcrMuxShift) & 7u) == 1u);
  CHECK(((soc::kPcrEncoder >> soc::kPcrMuxShift) & 7u) == pins::kEncoderMux);
}

TEST_CASE("input i2c: the chips on each bus, with an address unique per bus") {
  CHECK(cfg::clients_valid());
  unsigned on_control = 0;
  unsigned on_grid = 0;
  for (const cfg::Client& c : cfg::kClients) {
    (c.bus == cfg::Instance::I2c0 ? on_control : on_grid) += 1u;
  }
  CHECK(on_control == 2u);  // the codec and the control-button scanner share I2C0
  CHECK(on_grid == 2u);     // the pad scanner and the pad LEDs on I2C2
  CHECK(cfg::kClients[0].address7 == mk66::board::kCodecI2cAddress);
  CHECK(cfg::kClients[1].address7 == pins::kKeyScannerAddress);
  CHECK(cfg::kClients[2].address7 == pins::kKeyScannerAddress);  // the same address, the other bus
  CHECK(cfg::kClients[3].address7 == pins::kLedDriverAddress);
}

TEST_CASE("input i2c: one divider for both buses, within every chip's limit, and the clients' wait is the tighter") {
  CHECK(cfg::kSclHz == 375000u);
  CHECK(cfg::kSclHz <= 400000u);  // TCA8418, IS31FL3731 and SGTL5000 all take 400 kHz at most
  // A byte at 375 kHz is 24 us; the clients wait 300 us for one, 12 bytes' time; the codec keeps its 5 ms.
  CHECK(cfg::kClientByteTimeoutUs == 300u);
  CHECK(cfg::kCodecByteTimeoutUs == 5000u);
  // The work of one scan pass fits in a millisecond: INT_STAT, a clear, the counter and kMaxEventsPerDrain events at about 115 us a
  // register, for two chips (see scan_config.h).
  const unsigned transfers_per_chip = 3u + o3sdk::platform::mk66::input::scan::kMaxEventsPerDrain;
  CHECK(transfers_per_chip * 115u * 2u < 2000u);
}
