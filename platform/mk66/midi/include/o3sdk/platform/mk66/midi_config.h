// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The register values of the DIN MIDI port and its emitter timer, with their arithmetic, so the compiler and the host tests check them and the driver
// (src/midi_din.cpp) only writes them (the discipline of clock_config.h and audio_config.h). OS-005c2; ADR 0013 D1 (UART4 RX at priority 48, the one MIDI-out PIT at 96)
// and D5 (the PIT-compare emitter).
//
// Sources. Addresses and field positions: K66 Sub-Family Reference Manual chapters "UART", "PIT", "SIM" (cross-checked with NXP's MK66F18 device header, a fact
// table: UART4 at 0x400EA000, the C2 and S1 bits, the PIT registers and the clock
// gates). The values used:
// the baud divisor in 1/32 units from the BUS clock, `UART4_C1 = 0`: 8N1, `UART4_PFIFO = 0`: the FIFO off, pin 34 = PTE25 RX with pull-up, passive filter, ALT3; pin 33 =
// PTE24 TX with high drive, slew-rate limited, ALT3. The wiring is board.h's kMidiTx and kMidiRx.
//
// The baud rate. UART4 is clocked by the BUS clock (90 MHz here; UART0 and UART1 take the system clock): baud = bus / (16 * (SBR + BRFA / 32)). 31,250 baud is
// 90,000,000 / (16 * 180): SBR = 180, BRFA = 0, exact. A MIDI byte is 10 bit times = 320 us = 57,600 CPU cycles (midi/din.h); the emitter's byte model depends on it.
//
// The timer. PIT channel 0 counts down at the BUS clock (90 MHz), so one tick is two CPU cycles; a wake is programmed in ticks and the interrupt is entered after
// LDVAL + 1 of them. The channel is used as a ONE-SHOT: the interrupt stops it and, if the emitter wants another wake, loads it again (midi/din_io.h).

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/midi/din.h>
#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::midi_cfg {

static_assert(clock::kCoreHz == core::kCpuHz, "the cycle counts of the MIDI timing core are relative to the 180 MHz core clock of clock_config.h");

// ---- UART4 (K66 RM "UART", base 0x400E_A000) -------------------------------------------------------------------------------------
namespace uart4 {
inline constexpr std::uint32_t kBase = 0x400EA000u;
inline constexpr std::uint32_t kOffBdh = 0x0, kOffBdl = 0x1, kOffC1 = 0x2, kOffC2 = 0x3, kOffS1 = 0x4, kOffS2 = 0x5, kOffC3 = 0x6, kOffD = 0x7, kOffC4 = 0xA,
                               kOffPfifo = 0x10;

// SIM_SCGC1[UART4] (address 0x40048028, bit 10).
inline constexpr std::uint32_t kSimScgc1 = 0x40048028u;
inline constexpr std::uint32_t kScgc1Uart4 = 1u << 10;

// C2: the transmitter, the receiver, the receiver's interrupt. TIE and TCIE stay off: the transmit side is the PIT's, no UART interrupt is used for it.
inline constexpr std::uint8_t kC2Te = 0x08;
inline constexpr std::uint8_t kC2Re = 0x04;
inline constexpr std::uint8_t kC2Rie = 0x20;
inline constexpr std::uint8_t kC2Run = kC2Te | kC2Re | kC2Rie;

// Baud: the 1/32 divisor, rounded to nearest (`(F_BUS * 2 + baud / 2) / baud`), split into BDH:BDL = SBR (13 bits) and C4[BRFA] (5 bits).
constexpr std::uint32_t divisor32(std::uint32_t bus_hz, std::uint32_t baud) noexcept { return (bus_hz * 2u + baud / 2u) / baud; }
inline constexpr std::uint32_t kDivisor32 = divisor32(clock::kBusHz, midi::din::kBaud);
inline constexpr std::uint32_t kSbr = kDivisor32 >> 5;
inline constexpr std::uint32_t kBrfa = kDivisor32 & 0x1Fu;
inline constexpr std::uint8_t kBdh = static_cast<std::uint8_t>((kSbr >> 8) & 0x1Fu);  // LBKDIE, RXEDGIE, SBNS (one stop bit) all zero
inline constexpr std::uint8_t kBdl = static_cast<std::uint8_t>(kSbr & 0xFFu);
inline constexpr std::uint8_t kC4 = static_cast<std::uint8_t>(kBrfa);                 // M10 = 0, no match address
// The baud rate that comes out, from the registers (a test holds it to the nominal one).
constexpr std::uint32_t actual_baud_x32(std::uint32_t bus_hz, std::uint32_t sbr, std::uint32_t brfa) noexcept { return (bus_hz * 2u) / (sbr * 32u + brfa); }
static_assert(kSbr == 180u && kBrfa == 0u, "31,250 baud from the 90 MHz bus clock: SBR 180, no fine adjust");
static_assert(clock::kBusHz % (16u * midi::din::kBaud) == 0u, "the divisor is exact: no baud-rate error against a MIDI device");
static_assert(kSbr >= 1u && kSbr < 0x2000u, "SBR is a 13-bit field and must not be zero");

// PCR values (port control register: PE 1 << 1, PS 1 << 0, SRE 1 << 2, PFE 1 << 4, DSE 1 << 6, MUX at bit 8): RX with the internal pull-up and the passive filter (a
// floating or noisy input pin is a stream of false start bits), TX with high drive and slew-rate limiting.
inline constexpr std::uint32_t kPcrRx = (3u << 8) | (1u << 4) | (1u << 1) | (1u << 0);
inline constexpr std::uint32_t kPcrTx = (3u << 8) | (1u << 6) | (1u << 2);
static_assert(board::kMidiPinMux == 3u, "UART4's pins are ALT3 of port E");
}  // namespace uart4

// ---- PIT (K66 RM "PIT", base 0x4003_7000) -------------------------------------------------------------------------------------------
namespace pit {
inline constexpr std::uint32_t kBase = 0x40037000u;
inline constexpr std::uint32_t kMcr = kBase + 0x000u;
constexpr std::uint32_t ldval(unsigned channel) noexcept { return kBase + 0x100u + 0x10u * channel; }
constexpr std::uint32_t tctrl(unsigned channel) noexcept { return kBase + 0x108u + 0x10u * channel; }
constexpr std::uint32_t tflg(unsigned channel) noexcept { return kBase + 0x10Cu + 0x10u * channel; }
inline constexpr std::uint32_t kMcrEnable = 0;           // MDIS = 0: the module's clock runs; FRZ = 0: the timers keep counting under a debugger
inline constexpr std::uint32_t kTctrlTen = 1u << 0;
inline constexpr std::uint32_t kTctrlTie = 1u << 1;
inline constexpr std::uint32_t kTflgTif = 1u << 0;       // write 1 to clear
inline constexpr std::uint32_t kSimScgc6 = 0x4004803Cu;  // SIM_SCGC6_PIT
inline constexpr std::uint32_t kScgc6Pit = 1u << 23;
inline constexpr unsigned kChannel = 0;                  // resources.h: the MIDI-out emitter's

// CPU cycles per timer tick, as a shift (core clock / bus clock = 2): the interrupt shifts, it never divides.
inline constexpr unsigned kCyclesPerTickShift = 1;
static_assert((clock::kCoreHz >> kCyclesPerTickShift) == clock::kBusHz && (clock::kCoreHz & ((1u << kCyclesPerTickShift) - 1u)) == 0u,
              "one PIT tick is 2 CPU cycles: the timer runs at the bus clock");
// One MIDI byte in timer ticks: 28,800 (320 us). The emitter's commit-depth-1 wake after a byte.
inline constexpr std::uint32_t kByteTicks = static_cast<std::uint32_t>(midi::din::kByteCycles >> kCyclesPerTickShift);
static_assert(kByteTicks == 28800u, "a DIN byte is 320 us = 28,800 ticks of the 90 MHz bus clock");
}  // namespace pit

// ---- The audio timeline's lead -----------------------------------------------------------------------------------------------------
// The block the render works on is HEARD (kTxSlots - 1) blocks after the boundary that started its render: the TX ring renders ahead (hal/audio.h; slot_ring.h: "the
// block rendered for boundary s plays at s + N - 1"). The time base given to the MIDI timing core is the cycle counter at which a timeline sample is HEARD, so that a
// MIDI byte due at a sample leaves the wire with that sample's sound, and a clock byte that arrives is placed on the timeline where the sound of that instant is. The
// I2S and codec pipeline after the TX ring (a few frames) is not in it: a constant the user's offsets cancel, an HW-B row (audio-to-MIDI alignment on the scope).
inline constexpr std::uint32_t kAudioLeadCycles = (hal::audio::kTxSlots - 1u) * hal::audio::kCyclesPerBlock;

}  // namespace o3sdk::platform::mk66::midi_cfg
