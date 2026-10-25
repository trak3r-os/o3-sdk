// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The FT812's SPI bus on the MK66: which DSPI, which pins, which clock, as values the compiler and the host tests check.
// Chip facts: K66 Sub-Family Reference Manual, "DSPI": SCK = (f_bus / PBR) x ((1 + DBR) / BR), CTAR field positions,
// PUSHR layout (NXP's MK66F18 device header was used as a fact table to cross-check the positions, nothing copied).
//
// Board facts:
//   * The FT812 is on SPI2: MOSI = PTD13 (Teensy pin 52), MISO = PTD14 (51), SCK = PTD12 (53), all pin-mux ALT2
//     (the Teensy 3.6 SPI2 pin map puts pins 52/51/53 at mux 2).
//   * Chip select is a plain GPIO, PTD15 (pin 54), held low by software for a whole transfer; PD# is a GPIO,
//     PTD11 (pin 55). Both pins
//     are already driven high by boot::early_pins() (board.h). PTD10 (a reset line of the display header) is not used;
//     this image leaves it alone.
//   * SPI mode 0, MSB first, 8-bit frames.
//   * The init runs in the 1 MHz class and the data at the largest rate not above 20 MHz that the 90 MHz bus's divider table
//     gives: 18 MHz (the Teensy SPI2.h divider table; the radar card `eve-driver-libraries` worked it through). Not measured on a unit (hw checklist DSP-7).
//
// Limits (FT81X Programmers Guide p.11-12): at most 11 MHz until the FT812's clock is up, at most 30 MHz after; the project
// spec asks for 20 MHz at most after. Both are static_asserts below, against the real bus clock.

#include <o3sdk/platform/mk66/clock_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::display::spi {

inline constexpr std::uint32_t kMaxInitHz = 11'000'000u;
inline constexpr std::uint32_t kMaxDataHz = 20'000'000u;

// CTAR fields (RM "DSPI Clock and Transfer Attributes Register"): DBR [31], FMSZ [30:27] (frame size - 1), CPOL [26],
// CPHA [25], LSBFE [24], PBR [17:16], BR [3:0]. Delays (CSSCK, ASC, DT) are left 0: chip select is a GPIO.
struct Rate {
  std::uint8_t pbr;  // prescaler code 0..3 -> divide by 2, 3, 5, 7
  std::uint8_t br;   // baud-rate scaler code 0..15 -> divide by 2, 4, 6, 8, 16, 32, ... 32768
  bool dbr;          // double baud rate: SCK = twice the rate the other two fields give
};

constexpr std::uint32_t pbr_divider(std::uint8_t code) noexcept {
  return code == 0 ? 2u : code == 1 ? 3u : code == 2 ? 5u : 7u;
}
constexpr std::uint32_t br_divider(std::uint8_t code) noexcept {
  // 2, 4, 6, 8, then powers of two from 16 (code 4) to 32768 (code 15)
  return code < 4 ? 2u * (code + 1u) : 16u << (code - 4u);
}
constexpr std::uint32_t sck_hz(std::uint32_t bus_hz, const Rate& r) noexcept {
  return bus_hz / pbr_divider(r.pbr) * (r.dbr ? 2u : 1u) / br_divider(r.br);
}

inline constexpr std::uint32_t kFrameBits8 = 7u;  // FMSZ for 8-bit frames
constexpr std::uint32_t ctar(const Rate& r) noexcept {
  return (r.dbr ? (1u << 31) : 0u) | (kFrameBits8 << 27) | (static_cast<std::uint32_t>(r.pbr & 3u) << 16) | (r.br & 15u);
  // CPOL = 0, CPHA = 0 (mode 0), LSBFE = 0 (MSB first)
}

// Init: 90 MHz / 5 / 16 = 1.125 MHz, the 1 MHz class (a wide margin under 11 MHz for the warm-start-sensitive part).
inline constexpr Rate kInitRate = {2, 4, false};
// Data: 90 MHz / 5 x 2 / 2 = 18 MHz, the largest rate not above 20 MHz. DBR with the /5 prescaler gives a 40/60 duty
// cycle (RM, DBR description), about 22 ns at its narrowest: inside the FT812's SCK high/low minimum at 30 MHz, to be
// confirmed on a scope (DSP-7).
inline constexpr Rate kDataRate = {2, 0, true};

inline constexpr std::uint32_t kBusHz = clock::kBusHz;
static_assert(sck_hz(kBusHz, kInitRate) == 1'125'000u, "init clock 1.125 MHz at the 90 MHz bus");
static_assert(sck_hz(kBusHz, kDataRate) == 18'000'000u, "data clock 18 MHz at the 90 MHz bus");
static_assert(sck_hz(kBusHz, kInitRate) <= kMaxInitHz, "init clock at most 11 MHz until the FT812 clock is up (Guide p.11)");
static_assert(sck_hz(kBusHz, kDataRate) <= kMaxDataHz, "data clock at most 20 MHz (spec; the chip allows 30 MHz)");
// If the bus clock were the 60 MHz an earlier architecture note claimed, both would still be inside the limits.
static_assert(sck_hz(60'000'000u, kInitRate) <= kMaxInitHz && sck_hz(60'000'000u, kDataRate) <= kMaxDataHz,
              "the rates stay legal at a 60 MHz bus too");
static_assert(ctar(kInitRate) == 0x3802'0004u, "CTAR0 for the init rate: FMSZ 7, PBR /5, BR /16, no DBR");
static_assert(ctar(kDataRate) == 0xB802'0000u, "CTAR0 for the data rate: DBR, FMSZ 7, PBR /5, BR /2");

}  // namespace o3sdk::platform::mk66::display::spi
