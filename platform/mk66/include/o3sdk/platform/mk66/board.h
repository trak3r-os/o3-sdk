// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Board facts of the Tracker that the platform layer needs before anything else: which pin does what, its level and its pull,
// on the Teensy 3.6 pin map. None has been verified on this tree's own hardware sessions yet (see README.md, the first-boot checklist).

#include <cstdint>

namespace o3sdk::platform::mk66 {

enum class Port : std::uint8_t { A = 0, B = 1, C = 2, D = 3, E = 4 };

struct Pin {
  Port port;
  std::uint8_t bit;  // 0..31
};

constexpr bool operator==(const Pin& a, const Pin& b) noexcept { return a.port == b.port && a.bit == b.bit; }

namespace board {

// Status LED: Teensy pin 78 = PTC19 (PORTC bit 19). An output, active low: it idles HIGH
// and every blink pulls the pin LOW.
// Whether it is a visible LED, a test pad or both on the maintainer's unit is an open question (README.md).
inline constexpr Pin kStatusLed = {Port::C, 19};
inline constexpr bool kStatusLedActiveLow = true;

// Audio output mute key: Teensy pin 21 = PTD6 (PORTD bit 6). LOW mutes the output, HIGH unmutes it. This image drives it LOW
// as the second pin action of the reset sequence, right after the power key below and before the rail's settle wait (OS-BUG-PADS
// step 3; before that change it was the first). A LOW level cannot back-power anything, so it does not wait for the rail.
// This image makes no sound and never unmutes.
inline constexpr Pin kAudioMute = {Port::D, 6};

// SDRAM power key: Teensy pin 80 = PTA24 (RAM control, pin 75 of the chip). HIGH
// powers the SDRAM, LOW removes its power: SDRAM contents do not survive a
// power cycle, ADR 0013 D7.
// OS-BUG-PADS step 3: it is the FIRST pin the reset sequence drives (boot::early_pins), HIGH, with the PCR below; the mute key goes
// LOW at once, then the sequence waits kRailSettleUs, and only then drives the other quiet pins, so that the HIGH ones (FT812 chip
// select and PD#, the status LED, MIDI TX) are not driven into a rail that is still ramping. Whether the key also feeds more than the
// SDRAM (the pad chips of the later board revision, Control 4.0 / Grid 2.0) is an open research question, and so is how long its
// rail takes to come up: the wait is a bounded compromise, not a measured ramp, and it protects nothing if the key does not
// feed those parts. The order and the wait are harmless if it does not. Untested on both units.
inline constexpr Pin kSdramPower = {Port::A, 24};
// The wait between the mute key and the remaining quiet pins, in microseconds, timed on the cycle counter at the reset clock (clock::kFeiHz).
inline constexpr std::uint32_t kRailSettleUs = 2000u;
// Its PCR: pin mux 1 (GPIO, bits 10:8), DSE high drive (bit 6), SRE slew-rate limited (bit 2). The plain GPIO value 0x100 was used
// before OS-BUG-PADS step 3. Pinned by mk66_sdramc_config_test.cpp (the fields are decoded there from the register layout).
inline constexpr std::uint32_t kSdramPowerPcr = 0x144u;

// FT812 display controller (chip select 54 = PTD15, power-down 55 = PTD11: bits
// 15 and 11 of PORTD). Both are driven HIGH at boot: chip select released, power-down line released. (A reset of the chip is
// PD# low and then high.) The quiet-pin state leaves the FT812 in standby (it needs a host ACTIVE command to wake), not driving a
// display; the display bring-up (display/) takes it from there. Whether PD# is wired at all is an open hardware check (ADR 0011 amendment 1).
inline constexpr Pin kFt812ChipSelect = {Port::D, 15};
inline constexpr Pin kFt812PowerDown = {Port::D, 11};
// The FT812's SPI (OS-007c, display/): DSPI2 on PTD12 SCK (Teensy pin 53), PTD13 MOSI (pin 52), PTD14 MISO (pin 51), all
// pin-mux ALT2 (the Teensy 3.6 SPI2 pin map puts pins 52/51/53 at mux 2). Chip select stays the GPIO above; see
// display/dspi_config.h for the rest of the bus's facts.
inline constexpr Pin kFt812Sck = {Port::D, 12};
inline constexpr Pin kFt812Mosi = {Port::D, 13};
inline constexpr Pin kFt812Miso = {Port::D, 14};

// Analogue input switch: Teensy pin 24 = PTE26 (bit 26 of PORTE). It chooses what
// the codec's LINE input hears: HIGH = the line jack, LOW = the FM receiver (the polarity assumed here; this comment said the opposite until
// OS-011c, and the unit has not confirmed it yet, README.md). This image drives it LOW at
// boot: the receiver's position, with the receiver held in reset, so nothing is heard until a recorder source
// is chosen (track3r-os's recorder drove it HIGH for the line and the microphone and LOW for the radio; M2 records nothing). Whether the line jack is heard at boot in this position is a hardware check.
inline constexpr Pin kInputSourceSelect = {Port::E, 26};

// Si4703 FM radio reset: Teensy pin 67 = PTE27. LOW holds the radio in reset, HIGH runs it. At boot this image stops at the
// first state: the radio held in reset, silent and idle.
inline constexpr Pin kRadioReset = {Port::E, 27};

// MIDI DIN output: UART4 TX = Teensy pin 33 = PTE24. A UART's TX line idles HIGH; this image holds the pin there as a GPIO
// until the UART takes it over, so the DIN output idles instead of floating.
inline constexpr Pin kMidiTx = {Port::E, 24};
// MIDI DIN input: UART4 RX = Teensy pin 34 = PTE25 (pull-up, passive filter, ALT3). Both MIDI pins are ALT3 of port E (OS-005c2).
inline constexpr Pin kMidiRx = {Port::E, 25};
inline constexpr std::uint8_t kMidiPinMux = 3;

// ---- Audio: the I2S0 pins and the codec's I2C bus (OS-005a) --------------------------------------------------------------
// The five I2S0 signals are on port E at mux 4 (ALT4)
// (LRCLK, BCLK and MCLK on PTE11, PTE12 and PTE6, TXD0 on PTE10, RXD0 on PTE7). The K66 is the I2S master: it drives MCLK, BCLK and LRCLK (the codec is the slave,
// CHIP_I2S_CTRL MS = 0) and the codec's DOUT is RXD0. This board is not a stock Teensy 3.6 on these pins.
inline constexpr std::uint8_t kI2sPinMux = 4;
inline constexpr Pin kI2sMclk = {Port::E, 6};    // I2S0_MCLK
inline constexpr Pin kI2sRxd = {Port::E, 7};     // I2S0_RXD0
inline constexpr Pin kI2sTxd = {Port::E, 10};    // I2S0_TXD0
inline constexpr Pin kI2sLrclk = {Port::E, 11};  // I2S0_TX_FS (the receiver is synchronous to the transmitter)
inline constexpr Pin kI2sBclk = {Port::E, 12};   // I2S0_TX_BCLK

// The SGTL5000 is on I2C0 at Teensy pins 47 (SCL) and 48 (SDA). The Teensy 3.6 pin map
// puts pin 47 on PTD8 and pin 48 on PTD9, with
// I2C0 as ALT2 on both. (PTA12/PTA13, which an earlier version of this comment named, are the
// grid-pad bus's pins 3 and 4 on Wire2.) The same bus carries the control-button expander and the
// FM radio; this image only talks to the codec on it. External pull-ups: the pins are open drain, no internal
// pull, slew-rate limited and high drive.
inline constexpr std::uint8_t kCodecI2cMux = 2;
inline constexpr Pin kCodecScl = {Port::D, 8};   // I2C0_SCL
inline constexpr Pin kCodecSda = {Port::D, 9};   // I2C0_SDA
inline constexpr std::uint8_t kCodecI2cAddress = 0x0A;  // SGTL5000 with CTRL_ADR0_CS low

// What `boot::early_pins()` drives after the SDRAM power key (kSdramPower, HIGH, kSdramPowerPcr: the first pin action of the reset),
// in this order, and the level. Everything here is a quiet state (each described above); the amplifier mute comes first. The first
// kQuietPinsAtOnce entries (the mute) are driven right after the power key; then the sequence waits kRailSettleUs, then drives
// the rest in table order. The SDRAM power key is not in the list: it is driven before the list and held by the SDRAM bring-up
// (which drives it again, the same value).
struct QuietPin {
  Pin pin;
  bool high;
};
inline constexpr unsigned kQuietPinsAtOnce = 1;
inline constexpr QuietPin kQuietPins[] = {
    {kAudioMute, false},          // amplifier muted
    {kStatusLed, kStatusLedActiveLow},  // LED off (idles HIGH for an active-low LED)
    {kInputSourceSelect, false},  // the receiver's position; the receiver is in reset, so silent
    {kRadioReset, false},         // radio in reset
    {kFt812ChipSelect, true},     // FT812 chip select released
    {kFt812PowerDown, true},      // FT812 PD# released: standby
    {kMidiTx, true},              // MIDI DIN output idle (UART TX idle level)
};
inline constexpr unsigned kQuietPinCount = sizeof(kQuietPins) / sizeof(kQuietPins[0]);

// The Power switch (OS-POWER): Teensy pin 9 = PTC3, a plain input. M2 never reads it (the shutdown is M2b); in track3r-os one source read it once a
// pass of the main loop; nothing is ever driven on it. The two constants below are the board's unknowns, settled on the unit by `diag power`
// (the pin read with no pull, the pull-up and the pull-down, idle and pressed: QA-GLB-113, measurement M4): a tact switch needs a pull, so the default assumption is a pull-up with the switch pulling
// the pin LOW. The internal pull-up is ON: with an external pull-up it changes nothing, and a pin that nothing pulls would otherwise float and could read low for 2 s and commit a shutdown (review of
// slice 1). A wrong polarity is harmless: a switch that always reads "pressed" never arms (core/power_hold.h), and the worst case of a wrong constant in slice 1 is a restart, not a stop.
// kPowerPull: 0 none, 1 pull-up, 2 pull-down. `diag power` still reads the pin in all three pulls, whatever is configured.
inline constexpr Pin kPowerSwitch = {Port::C, 3};
inline constexpr bool kPowerActiveLow = true;
inline constexpr std::uint8_t kPowerPull = 1;

// The bootloader request pin: Teensy pin 81 = PTA1, pulled low by the app to hand over to the bootloader. Only track3r-os's `fw update` drove
// it; M2 never does (the update over USB is M2b). Recorded so nobody claims the pin for something else.
inline constexpr Pin kBootloaderRequest = {Port::A, 1};

}  // namespace board
}  // namespace o3sdk::platform::mk66
