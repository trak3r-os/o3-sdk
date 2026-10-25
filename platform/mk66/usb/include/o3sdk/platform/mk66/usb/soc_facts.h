// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the MK66's USB0 (the USB OTG FS module, device mode) needs from the platform: register addresses and bit values, the clock
// choice and why, the interrupt line and the unique-ID serial number, as constants with their arithmetic checked by the compiler
// and pinned by tests/mk66_usb_soc_test.cpp. The code that writes them (src/usb0.cpp) is the image's; the logic that uses the
// controller is the pure usb library (usb/device.h), which sees none of this.
//
// SOURCES. Addresses, offsets and field positions are the K66 Sub-Family Reference Manual's chapters "USB OTG FS (USBFS)" (register
// map: ISTAT, INTEN, ERRSTAT, ERREN, STAT, CTL, ADDR, BDTPAGE1-3, ENDPTn, USBCTRL, CONTROL, USBTRC0, CLK_RECOVER_*), "System
// Integration Module (SIM)" (SOPT2, SCGC4, CLKDIV2, UIDH..UIDL) and "System MPU (SYSMPU)" (RGDAAC0), cross-checked against NXP's
// MK66F18 device header, a fact table, not copied. The code is written
// fresh. The reference manual itself was not machine-readable for this slice: every value below that is not in the header
// fact tables is marked "(RM)" and is on the hardware-session checklist (README.md, USB).

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/irq_map.h>
#include <o3sdk/usb/device.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::usb0::soc {

// ---- The clock, and why it is the IRC48M ----------------------------------------------------------------------------------
// The full-speed module needs a 48 MHz clock, within 0.25 % (USB 2.0 7.1.11: 12 Mbit/s +-0.25 %). Two sources can feed it
// (SIM_SOPT2[USBSRC] = 1 selects the SIM's divided clock; PLLFLLSEL picks what is divided):
//   * the PLL: MCGPLLCLK through the SIM_CLKDIV2 divider, out = in x (USBFRAC + 1) / (USBDIV + 1), USBFRAC 1 bit, USBDIV 3 bits (RM);
//   * the 48 MHz internal reference oscillator IRC48M (PLLFLLSEL = 3), disciplined by the module's own USB clock recovery from the
//     bus' start-of-frame packets, so it needs no crystal and is untouched by the system clock tree.
// At the OS's 180 MHz in HSRUN the PLL output is 180 MHz, and no divider setting makes 48 MHz from it (the compile-time proof
// below): the PLL path would need a 192 or 144 MHz tree. The IRC48M does not depend on the system clock at all, so HSRUN, the
// 90 MHz bus clock and the SDRAM clock do not matter to it.
inline constexpr std::uint32_t kUsbClockHz = 48'000'000u;
inline constexpr std::uint32_t kUsbClockToleranceHz = kUsbClockHz / 400u;  // 0.25 %

constexpr bool divider_makes_usb_clock(std::uint32_t source_hz) noexcept {
  for (std::uint64_t frac = 0; frac < 2u; ++frac) {
    for (std::uint64_t div = 0; div < 8u; ++div) {
      const std::uint64_t out = static_cast<std::uint64_t>(source_hz) * (frac + 1u) / (div + 1u);
      const std::uint64_t diff = out > kUsbClockHz ? out - kUsbClockHz : kUsbClockHz - out;
      if (diff <= kUsbClockToleranceHz) {
        return true;
      }
    }
  }
  return false;
}
static_assert(!divider_makes_usb_clock(clock::mcgout_hz()), "at 180 MHz the PLL cannot make the 48 MHz USB clock: the IRC48M is the source");
static_assert(divider_makes_usb_clock(48'000'000u), "the IRC48M needs no division");
static_assert(divider_makes_usb_clock(192'000'000u) && divider_makes_usb_clock(144'000'000u),
              "the check finds the PLL frequencies that would have worked");

// ---- Register addresses (USB0 at 0x4007_2000) ---------------------------------------------------------------------------
inline constexpr std::uintptr_t kBase = 0x40072000u;
inline constexpr std::uint32_t kOffIstat = 0x80;
inline constexpr std::uint32_t kOffInten = 0x84;
inline constexpr std::uint32_t kOffErrstat = 0x88;
inline constexpr std::uint32_t kOffErren = 0x8C;
inline constexpr std::uint32_t kOffStat = 0x90;
inline constexpr std::uint32_t kOffCtl = 0x94;
inline constexpr std::uint32_t kOffAddr = 0x98;
inline constexpr std::uint32_t kOffBdtPage1 = 0x9C;
inline constexpr std::uint32_t kOffBdtPage2 = 0xB0;
inline constexpr std::uint32_t kOffBdtPage3 = 0xB4;
inline constexpr std::uint32_t kOffFrmNumL = 0xA0;  // frame number, low byte (reading it latches the high register); OS-006d: the SOF's frame number for the rate estimator
inline constexpr std::uint32_t kOffFrmNumH = 0xA4;  // frame number, bits 10..8 in bits 2..0
inline constexpr std::uint32_t kOffEndpt0 = 0xC0;  // ENDPTn at kOffEndpt0 + 4 n
inline constexpr std::uint32_t kOffUsbCtrl = 0x100;
inline constexpr std::uint32_t kOffControl = 0x108;
inline constexpr std::uint32_t kOffOtgIstat = 0x10;
inline constexpr std::uint32_t kOffClkRecoverCtrl = 0x140;
inline constexpr std::uint32_t kOffClkRecoverIrcEn = 0x144;
inline constexpr std::uint32_t kEndptStride = 4;

// ---- Bit values -------------------------------------------------------------------------------------------------------------
// CONTROL: DPPULLUPNONOTG (bit 4): the D+ pull-up of the module "if the module is configured in non-OTG device mode" (RM). It is what
// tells the host a full-speed device is attached; it is switched on last (usb0.cpp), after the buffer descriptor table and endpoint 0
// are ready.
inline constexpr std::uint8_t kControlPullUp = 0x10;
// USBCTRL: 0 = transceiver not suspended, no pull-downs (SUSP bit 7, PDE bit 6): the device never suspends its transceiver (see
// README.md "VBUS and suspend").
inline constexpr std::uint8_t kUsbCtrlRun = 0x00;
// CLK_RECOVER_IRC_EN: IRC_EN (bit 1) switches the 48 MHz oscillator on, REG_EN (bit 0) its regulator. CLK_RECOVER_CTRL:
// CLOCK_RECOVER_EN (bit 7) enables the recovery, RESTART_IFRTRIM_EN (bit 5) restarts the trim from its factory value at every bus reset,
// resume and suspend exit. The driver writes exactly these two values.
inline constexpr std::uint8_t kClkRecoverIrcEnable = 0x03;
inline constexpr std::uint8_t kClkRecoverCtrlEnable = 0xA0;
static_assert(kClkRecoverIrcEnable == (0x02 | 0x01) && kClkRecoverCtrlEnable == (0x80 | 0x20), "IRC_EN|REG_EN, CLOCK_RECOVER_EN|RESTART_IFRTRIM_EN");

// SIM_SOPT2: USBSRC (bit 18) = 1, PLLFLLSEL (bits 17..16) = 3 selects the IRC48M. PLLFLLSEL is SHARED with the peripherals that take
// "the FLL or PLL clock": SIM_SOPT2[SDHCSRC], [LPUARTSRC], [TPMSRC] and [TIMESRC] (the Ethernet timer); the other UARTs run from the system clock: a later slice that gives one of them that source must know the
// OS runs it from the IRC48M. SIM_CLKDIV2 = 0: USBDIV 0, USBFRAC 0, divide by one.
inline constexpr std::uint32_t kSopt2UsbSrc = 1u << 18;
inline constexpr std::uint32_t kSopt2PllFllSelMask = 3u << 16;
inline constexpr std::uint32_t kSopt2PllFllSelIrc48 = 3u << 16;
inline constexpr std::uint32_t kSopt2UsbBits = kSopt2UsbSrc | kSopt2PllFllSelIrc48;
inline constexpr std::uint32_t kClkdiv2 = 0;
inline constexpr std::uint32_t kScgc4UsbOtg = 1u << 18;  // SIM_SCGC4 clock gate of the module

// The USB module is a bus master (it reads and writes the buffer descriptors and the packet buffers by DMA). The system MPU grants
// bus masters their rights per region; at reset the USB master has none. RGDAAC0 is the access control word of region 0, which
// covers the whole address space: bits 24 (M4WE) and 25 (M4RE) are master 4, the USB FS module on this chip (RM "SYSMPU", master
// numbering; both bits set is 0x03000000). Without it every USB DMA access is refused and ERRSTAT[DMAERR] is
// set: the first hardware-session check (README.md).
inline constexpr std::uintptr_t kMpuRgdaac0 = 0x4000D800u;
inline constexpr std::uint32_t kMpuUsbMaster = (1u << 24) | (1u << 25);

// ---- The interrupt ---------------------------------------------------------------------------------------------------------------
inline constexpr Irq kIrq = Irq::USB0;
inline constexpr unsigned kIrqNumber = 53;
static_assert(static_cast<unsigned>(kIrq) == kIrqNumber, "USB0 is device interrupt 53 (irq_list.h)");
static_assert(role_of(kIrq) == hal::irq::Role::Usb && hal::irq::priority(hal::irq::Role::Usb) == 64,
              "USB0 sits in the Usb row of the priority table (ADR 0013 D1), priority 64");
// What that priority means for the neighbours (each also asserted by hal/irq.h for the pairs it names):
//   * the I2S DMA slot-boundary interrupt (16), SysTick (32) and the MIDI UART (48) preempt it, so their timestamps and the audio
//     deadline never wait for USB;
//   * the SDHC / SPI2-DMA (display) / I2C completions (80) cannot preempt it: they are flags only, and the transfers they report are
//     done by DMA hardware, so a completion flag delayed by the USB interrupt delays only the next display or SD chunk;
//   * the render (128) is preempted by it, as D1 requires (USB isochronous at 1 ms, bulk acknowledgements).
static_assert(hal::irq::preempts(hal::irq::Role::AudioDma, hal::irq::Role::Usb) && hal::irq::preempts(hal::irq::Role::SysTick, hal::irq::Role::Usb) &&
                  hal::irq::preempts(hal::irq::Role::MidiUart, hal::irq::Role::Usb),
              "audio DMA, SysTick and the MIDI UART preempt USB");
static_assert(hal::irq::preempts(hal::irq::Role::Usb, hal::irq::Role::Completion) && hal::irq::preempts(hal::irq::Role::Usb, hal::irq::Role::Render),
              "USB preempts the completions' flags and the render");

// The cost bound as arithmetic (an ESTIMATE until `diag usb` has been read on a unit, USB-11 of the hardware checklist): one pass of the
// interrupt loop is at most a 64-byte copy, a ring read of one chunk, a few descriptor writes and a handful of USB0 register accesses (each
// a peripheral-bus access of several cycles): taken as 600 cycles. The loop is capped at kMaxIsrLoops passes, so one entry can never run
// longer than the product, and that is held under 5 % of one audio block (the DMA slot period, ADR 0013 D1), so even a worst entry could
// not by itself threaten the 1.45 ms deadline of the I2S DMA interrupt (which in any case preempts it).
inline constexpr std::uint32_t kEstimatedPassCycles = 600;
inline constexpr std::uint32_t kEstimatedWorstEntryCycles = static_cast<std::uint32_t>(o3sdk::usb::kMaxIsrLoops) * kEstimatedPassCycles;
static_assert(kEstimatedWorstEntryCycles * 20u <= hal::audio::kCyclesPerBlock, "a worst USB interrupt entry is under 5 % of one audio block");

// ---- Addresses of the registers outside USB0 ---------------------------------------------------------------------------------
// SIM unique ID (RM SIM: UIDH, UIDMH, UIDML, UIDL, read only, 128 bits, 0x4004_8054..60).
inline constexpr std::uintptr_t kSimUidh = 0x40048054u;
inline constexpr std::uintptr_t kSimUidmh = 0x40048058u;
inline constexpr std::uintptr_t kSimUidml = 0x4004805Cu;
inline constexpr std::uintptr_t kSimUidl = 0x40048060u;
inline constexpr std::uintptr_t kSimSopt2 = 0x40048004u;
inline constexpr std::uintptr_t kSimScgc4 = 0x40048034u;
inline constexpr std::uintptr_t kSimClkdiv2 = 0x40048048u;

// BDT_PAGE1..3 hold address bits 15..9, 23..16 and 31..24 of the table (which is 512-byte aligned, so bit 8 and below are zero).
constexpr std::uint8_t bdt_page1(std::uint32_t address) noexcept { return static_cast<std::uint8_t>((address >> 8) & 0xFEu); }
constexpr std::uint8_t bdt_page2(std::uint32_t address) noexcept { return static_cast<std::uint8_t>((address >> 16) & 0xFFu); }
constexpr std::uint8_t bdt_page3(std::uint32_t address) noexcept { return static_cast<std::uint8_t>((address >> 24) & 0xFFu); }

// ---- The serial number (ADR 0011 item 6) -------------------------------------------------------------------------------------
// Windows keys its driver bindings on VID, PID and serial; our early test images used a constant (4294967295, not unique; docs/hw/tracker-hardware-notes.md), so
// two units were indistinguishable. This one comes from the chip: the 128-bit unique ID, folded with FNV-1a (64-bit) over its 16
// bytes (little endian words, UIDH first) into 16 upper-case hex digits. The hash keeps all 128 bits in play (a 24-character
// descriptor limit would otherwise cut the ID) and is deterministic: the same chip always reports the same serial.
inline constexpr std::size_t kSerialChars = 16;
constexpr std::uint64_t fnv1a64_words(const std::uint32_t* words, std::size_t count) noexcept {
  std::uint64_t h = 14695981039346656037ull;
  for (std::size_t i = 0; i < count; ++i) {
    for (unsigned b = 0; b < 4u; ++b) {
      h ^= (words[i] >> (8u * b)) & 0xFFu;
      h *= 1099511628211ull;
    }
  }
  return h;
}
// Writes kSerialChars digits and a NUL into `out` (at least kSerialChars + 1 bytes).
inline void serial_from_uid(const std::uint32_t uid[4], char* out) noexcept {
  const std::uint64_t h = fnv1a64_words(uid, 4);
  for (std::size_t i = 0; i < kSerialChars; ++i) {
    const unsigned nibble = static_cast<unsigned>((h >> (60u - 4u * i)) & 0xFu);
    out[i] = static_cast<char>(nibble < 10u ? '0' + nibble : 'A' + (nibble - 10u));
  }
  out[kSerialChars] = '\0';
}

// The configuration descriptor buffer: STANDARD's is 175 bytes and AUDIO's 384 (exactly 6 x 64, usb_descriptor_test.cpp goldens); 400 is
// o3sdk::usb::kMaxConfigDescriptorSize, which usb0.cpp asserts. The builder refuses a buffer that is too small and the driver does not pull the line
// up if it did.
inline constexpr std::size_t kConfigBufferBytes = 400;

}  // namespace o3sdk::platform::mk66::usb0::soc
