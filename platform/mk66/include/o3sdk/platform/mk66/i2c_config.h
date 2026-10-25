// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The I2C peripherals of the MK66 the board uses and how the one master (src/i2c.cpp) drives them: facts as `constexpr` values in a
// header the host tests can read (mk66_input_pins_test.cpp pins them against the two fact tables this tree reads: the Teensy core's
// header and NXP's MK66F18 header). OS-005a's codec bus (I2C0) and OS-007d's grid bus (I2C2) are two instances of the same master.
// The I2C0 facts (base, divider) live here, in the base library, so that the input and audio slices both read them from one place.

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::i2c_cfg {

enum class Instance : std::uint8_t { I2c0 = 0, I2c2 = 1 };
inline constexpr unsigned kInstanceCount = 2;

// Where an instance is: its register block, its clock gate (a register of the SIM and a bit of it), its two pins (open drain,
// external pull-ups) and the pin mux that routes the peripheral to them.
struct InstanceFacts {
  std::uintptr_t base;
  std::uintptr_t scgc_register;
  std::uint32_t scgc_bit;
  Pin scl;
  Pin sda;
  std::uint8_t mux;
};

// I2C0's register block (K66 RM "I2C", base 0x4006_6000; bus clock 90 MHz).
inline constexpr std::uintptr_t kI2c0Base = 0x40066000u;

// The control bus, I2C0: PTD8 / PTD9, ALT2 (board.h kCodecScl, kCodecSda: the SGTL5000, the control-button scanner, the radio). Clock
// gate SIM_SCGC4[I2C0] (0x40048034 bit 6).
// The grid bus, I2C2: Teensy pins 3 / 4 = PTA12 (SCL) / PTA13 (SDA), ALT5, external pull-ups, 400 kHz (the pad scanner and the pad LED driver).
// Base 0x400E6000, clock gate SIM_SCGC1[I2C2] (SIM_SCGC1 = 0x40048028, bit 6, mask 0x40).
inline constexpr Pin kGridScl = {Port::A, 12};
inline constexpr Pin kGridSda = {Port::A, 13};
inline constexpr std::uint8_t kGridMux = 5;
inline constexpr InstanceFacts kInstances[kInstanceCount] = {
    {kI2c0Base, 0x40048034u, 1u << 6, board::kCodecScl, board::kCodecSda, board::kCodecI2cMux},
    {0x400E6000u, 0x40048028u, 1u << 6, kGridScl, kGridSda, kGridMux},
};

constexpr const InstanceFacts& facts(Instance which) noexcept { return kInstances[static_cast<unsigned>(which)]; }

// Who talks on which bus: the chips, their 7-bit addresses and the slice that drives them. The Si4703 radio shares the control bus and
// is held in reset (board.h kRadioReset): nothing drives it. Two chips share the address 0x34, one on each bus: an address is unique
// per bus, never per board.
struct Client {
  Instance bus;
  std::uint8_t address7;
  const char* name;
};
inline constexpr Client kClients[] = {
    {Instance::I2c0, board::kCodecI2cAddress, "SGTL5000 codec (OS-005a)"},
    {Instance::I2c0, 0x34, "TCA8418 control-button scanner (OS-007d)"},
    {Instance::I2c2, 0x34, "TCA8418 pad scanner (OS-007d)"},
    {Instance::I2c2, 0x74, "IS31FL3731 pad LED driver (OS-007d)"},
};
inline constexpr unsigned kClientCount = sizeof(kClients) / sizeof(kClients[0]);
constexpr bool clients_valid() noexcept {
  for (unsigned i = 0; i < kClientCount; ++i) {
    for (unsigned j = i + 1; j < kClientCount; ++j) {
      if (kClients[i].bus == kClients[j].bus && kClients[i].address7 == kClients[j].address7) {
        return false;  // two chips at one address on one bus
      }
    }
  }
  return true;
}
static_assert(clients_valid(), "no two chips share an address on one bus");

// One divider for every instance, the same on both buses so that a client of one cannot find the other slower or faster. F = 0x1F:
// MULT 0, ICR 0x1F, an SCL divider of 240 (the divider table of the Teensy Wire library): 90 MHz / 240 = 375 kHz, below the 400 kHz of
// the SGTL5000 (datasheet "I2C Serial Clock Frequency" max 400 kHz), the TCA8418 and the IS31FL3731; a request for 400 kHz is
// quantised to the next divider, which is this one.
inline constexpr std::uint8_t kFrequencyDivider = 0x1F;
inline constexpr std::uint32_t kSclDivider = 240;
inline constexpr std::uint32_t kSclHz = clock::kBusHz / kSclDivider;

// How long a master waits for one byte (acknowledge or data) before it gives up. A byte at 375 kHz is 24 us.
//   kCodecByteTimeoutUs   OS-005a's wait for the SGTL5000 (register reads and writes at init and in the unmute ramp): 5 ms, as it was.
//   kClientByteTimeoutUs  `I2cBus::transfer`, used by the input drivers once a millisecond in the main loop: 300 us (12 bytes' time),
//                         so a dead bus costs a transfer at most the first byte's wait, not a stretch of the frame.
inline constexpr std::uint32_t kCodecByteTimeoutUs = 5000;
inline constexpr std::uint32_t kClientByteTimeoutUs = 300;

static_assert(kClientByteTimeoutUs < kCodecByteTimeoutUs, "the clients' wait is the tighter one");
static_assert(kSclHz <= 400000u, "no chip on these buses accepts more than 400 kHz");
static_assert(clock::kBusHz % kSclDivider == 0u, "an exact SCL frequency");

}  // namespace o3sdk::platform::mk66::i2c_cfg
