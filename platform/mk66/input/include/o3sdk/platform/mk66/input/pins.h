// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The board's input pins (OS-007d). Board facts (each cited),
// none of them yet seen on a unit of this tree (README.md, checklist INP). Kept apart from board.h so the audio slice (OS-005a)
// and this one do not edit the same lines; a host test (mk66_input_pins_test.cpp) proves no pin is claimed twice across the
// quiet pins, the display, the SDRAM and these.

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/i2c_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::input::pins {

// ---- Rotary encoder: the FTM2 quadrature decoder -------------------------------------------------------------------------
// Phase A = PTA10, phase B = PTA11 (PORTA_PCR10 = PORTA_PCR11 = 0x00000612): pin mux ALT6 = FTM2_QD_PHA /
// FTM2_QD_PHB, passive filter on, pull enabled and selected DOWN (PS = 0). The pull-down is the board's: the contacts are taken to the
// supply, and a pull-up here would hold both lines high and kill the dial. (Teensy pins 10 / 11 are
// PTC4 / PTC6: NOT these pins, and never configured as GPIO.) There is no encoder push: the encoder
// switch is unused (docs/design/product-ux.md section 3, maintainer), so no pin of it is configured.
inline constexpr Pin kEncoderA = {Port::A, 10};
inline constexpr Pin kEncoderB = {Port::A, 11};
inline constexpr std::uint8_t kEncoderMux = 6;

// ---- Key scanners' interrupt lines ----------------------------------------------------------------------------------------
// The pad scanner's INT is Teensy pin 38 = PTC11, the button scanner's pin 14 = PTD1. The TCA8418's INT is open drain, active low, so both
// pins are pull-up inputs and a key event is a falling edge. This tree reads the LEVEL in
// the main loop instead (no pin interrupt: README, "Scanning") and polls the chip anyway every few milliseconds, so a line that is not
// wired as assumed costs latency, not keys. Both are inputs for ever: nothing here drives them.
inline constexpr Pin kPadsInt = {Port::C, 11};
inline constexpr Pin kButtonsInt = {Port::D, 1};

// ---- The two I2C buses ----------------------------------------------------------------------------------------------------
// Both are instances of the platform's one I2C master (i2c_config.h, src/i2c.cpp), whose facts and citations are there:
//   pad bus ("grid", Teensy `Wire2`), I2C2, PTA12 / PTA13, ALT5: the pad key scanner (TCA8418) and the pad LED driver (IS31FL3731);
//   control bus (Teensy `Wire`), I2C0, PTD8 / PTD9, ALT2: the control-button key scanner (TCA8418 at 0x34 again: two chips with one
//   address, one on each bus) AND the SGTL5000 codec (0x0A, OS-005a) and the Si4703 radio (0x10, not driven here).
// (the first README of this tree called pins 47/48 "PTA12/PTA13": a copied comment; those are the pad bus.)
inline constexpr Pin kPadBusScl = i2c_cfg::kGridScl;
inline constexpr Pin kPadBusSda = i2c_cfg::kGridSda;
inline constexpr Pin kControlBusScl = mk66::board::kCodecScl;
inline constexpr Pin kControlBusSda = mk66::board::kCodecSda;
inline constexpr i2c_cfg::Instance kPadBus = i2c_cfg::Instance::I2c2;
inline constexpr i2c_cfg::Instance kControlBus = i2c_cfg::Instance::I2c0;

inline constexpr std::uint8_t kKeyScannerAddress = 0x34;  // TCA8418, both chips
inline constexpr std::uint8_t kLedDriverAddress = 0x74;   // IS31FL3731, AD pin to ground

// Every pin this slice configures, for the ownership test.
inline constexpr Pin kClaimed[] = {kEncoderA,  kEncoderB,      kPadsInt,       kButtonsInt,
                                   kPadBusScl, kPadBusSda,     kControlBusScl, kControlBusSda};
inline constexpr unsigned kClaimedCount = sizeof(kClaimed) / sizeof(kClaimed[0]);

}  // namespace o3sdk::platform::mk66::input::pins
