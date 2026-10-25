// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the MK66's SDHC module (the SD host controller) and the Tracker's SD slot need from the platform (OS-007e): register addresses and
// bit values, the pins, the clock arithmetic and the transfer-type word of each command, as constants whose arithmetic the compiler checks
// and tests/mk66_storage_soc_test.cpp pins against the two fact tables this tree reads. The code that writes them is the image's
// (src/sdhc_bus.cpp); the protocol on top of it (sd_card.h) sees none of this.
//
// SOURCES. Addresses, offsets and field positions are the K66 Sub-Family Reference Manual's chapter "Secured Digital Host Controller
// (SDHC)" (DSADDR, BLKATTR, CMDARG, XFERTYP, CMDRSP0-3, DATPORT, PRSSTAT, PROCTL, SYSCTL, IRQSTAT, IRQSTATEN, IRQSIGEN, WML), "System
// Integration Module (SIM)" (SCGC3 gate, SOPT2[SDHCSRC]) and "Port control" (PCR), cross-checked against NXP's MK66F18 device header
// (`*_MASK`), a fact table, not copied. Board and card facts (the code is written fresh):
//   * the pins: PTE0 D1, PTE1 D0, PTE2 CLK, PTE3 CMD, PTE4 D3, PTE5 D2, all ALT4, high drive; CMD and the four data lines with their pull-ups
//     on; the card-detect switch is a plain GPIO, Teensy pin 2 = PTD0, read with the internal pull-up, LOW = card in;
//   * the clock: the SDHC runs from the core clock (SDHCSRC = 0), 180 MHz; 400 kHz for the
//     identification, 4 data lines, high speed 50 MHz after CMD6; the divider search lands on
//     SDCLKFS 0x10 / DVS 14 for 400 kHz (375 kHz) and SDCLKFS 0 / DVS 3 for 50 MHz (45 MHz), which `plan_clock` computes;
//   * no card power switch: the card is powered with the board.
// The reference manual itself was not machine-readable for this slice: a value that is not in the header fact tables is marked "(RM)" and is
// on the hardware-session checklist (README.md, "SD card").

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/clock_config.h>
#include <o3sdk/platform/mk66/irq_map.h>
#include <o3sdk/platform/mk66/storage/sd_protocol.h>

#include <cstdint>

namespace o3sdk::platform::mk66::storage::sdhc {

// ---- Pins -------------------------------------------------------------------------------------------------------------------------------
inline constexpr std::uint8_t kPinMux = 4;  // ALT4 on all six
inline constexpr Pin kD1 = {Port::E, 0};
inline constexpr Pin kD0 = {Port::E, 1};
inline constexpr Pin kClk = {Port::E, 2};
inline constexpr Pin kCmd = {Port::E, 3};
inline constexpr Pin kD3 = {Port::E, 4};
inline constexpr Pin kD2 = {Port::E, 5};
inline constexpr Pin kCardDetect = {Port::D, 0};  // GPIO input, pull-up, LOW = a card is in
inline constexpr bool kCardDetectActiveLow = true;

// PORTx_PCRn fields (MK66F18.h PORT_PCR_*_MASK).
inline constexpr std::uint32_t kPcrPs = 1u << 0;   // pull select: up
inline constexpr std::uint32_t kPcrPe = 1u << 1;   // pull enable
inline constexpr std::uint32_t kPcrDse = 1u << 6;  // high drive strength
inline constexpr std::uint32_t kPcrMuxShift = 8;
constexpr std::uint32_t pcr_alt(unsigned mux) noexcept { return static_cast<std::uint32_t>(mux) << kPcrMuxShift; }
inline constexpr std::uint32_t kPcrClk = pcr_alt(kPinMux) | kPcrDse;                        // the clock line: driven, no pull
inline constexpr std::uint32_t kPcrCmdData = pcr_alt(kPinMux) | kPcrDse | kPcrPe | kPcrPs;  // CMD and D0..D3: driven, pulled up
inline constexpr std::uint32_t kPcrCardDetect = pcr_alt(1) | kPcrPe | kPcrPs;               // GPIO, pulled up

// ---- Clock gate and source ----------------------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kSimScgc3 = 0x40048030u;
inline constexpr std::uint32_t kScgc3Sdhc = 1u << 17;  // SIM_SCGC3[SDHC] (kinetis.h SIM_SCGC3_SDHC 0x00020000)
inline constexpr std::uint32_t kSopt2SdhcSrcMask = 0x3u << 28;  // SIM_SOPT2[SDHCSRC]: 0 = core / system clock, 1 = MCGPLLCLK or MCGFLLCLK (PLLFLLSEL), 2 = OSCERCLK, 3 = SDHC_CLKIN
inline constexpr std::uint32_t kSdhcSourceHz = clock::kCoreHz;  // SDHCSRC = 0: 180 MHz

// ---- Registers (SDHC at 0x400B_1000) ------------------------------------------------------------------------------------------------
inline constexpr std::uintptr_t kBase = 0x400B1000u;
inline constexpr std::uint32_t kOffDsaddr = 0x00;
inline constexpr std::uint32_t kOffBlkattr = 0x04;
inline constexpr std::uint32_t kOffCmdarg = 0x08;
inline constexpr std::uint32_t kOffXfertyp = 0x0C;
inline constexpr std::uint32_t kOffCmdrsp0 = 0x10;  // CMDRSP0..3 at +4 each
inline constexpr std::uint32_t kOffDatport = 0x20;
inline constexpr std::uint32_t kOffPrsstat = 0x24;
inline constexpr std::uint32_t kOffProctl = 0x28;
inline constexpr std::uint32_t kOffSysctl = 0x2C;
inline constexpr std::uint32_t kOffIrqstat = 0x30;
inline constexpr std::uint32_t kOffIrqstaten = 0x34;
inline constexpr std::uint32_t kOffIrqsigen = 0x38;
inline constexpr std::uint32_t kOffAc12err = 0x3C;
inline constexpr std::uint32_t kOffWml = 0x44;
inline constexpr std::uint32_t kOffVendor = 0xC0;

// BLKATTR: BLKSIZE 12:0, BLKCNT 31:16.
constexpr std::uint32_t blkattr(std::uint32_t block_size, std::uint32_t block_count) noexcept { return (block_size & 0x1FFFu) | (block_count << 16); }

// XFERTYP fields.
inline constexpr std::uint32_t kXferDmaen = 1u << 0;
inline constexpr std::uint32_t kXferBcen = 1u << 1;    // block count enable (a multiple-block transfer)
inline constexpr std::uint32_t kXferAc12en = 1u << 2;  // the controller sends CMD12 itself after the last block
inline constexpr std::uint32_t kXferDtdsel = 1u << 4;  // data transfer direction: 1 = read (card to host)
inline constexpr std::uint32_t kXferMsbsel = 1u << 5;  // multiple blocks
inline constexpr std::uint32_t kXferRsptypShift = 16;  // 0 none, 1 136 bits, 2 48 bits, 3 48 bits with busy
inline constexpr std::uint32_t kXferCccen = 1u << 19;  // check the response's CRC
inline constexpr std::uint32_t kXferCicen = 1u << 20;  // check the response's command index
inline constexpr std::uint32_t kXferDpsel = 1u << 21;  // data present
inline constexpr std::uint32_t kXferCmdtypShift = 22;  // 0 normal, 3 abort (CMD12 sent by the host)
inline constexpr std::uint32_t kXferCmdinxShift = 24;
inline constexpr std::uint32_t kRsptypNone = 0;
inline constexpr std::uint32_t kRsptyp136 = 1;
inline constexpr std::uint32_t kRsptyp48 = 2;
inline constexpr std::uint32_t kRsptyp48Busy = 3;
inline constexpr std::uint32_t kCmdtypAbort = 3;

// The transfer-type word of a command. One function for every command the driver has, so the table of what each one needs (the response
// kind and whether it is checked, whether data follows, whether the controller ends a multiple-block read) is in one place:
//   R1, R6, R7       48 bits, CRC and index checked      R1b   48 bits and the busy wait, both checked
//   R2               136 bits, CRC checked (it has no index to check)      R3   48 bits, nothing checked (the OCR has no CRC, no index)
//   data commands    DPSEL, direction read; CMD18 also MSBSEL, BCEN and AC12EN (the controller stops the transfer)
// CMD12 sent by hand (error recovery) is an abort-type command.
constexpr std::uint32_t xfertyp(sd::Cmd cmd) noexcept {
  std::uint32_t x = static_cast<std::uint32_t>(sd::wire_index(cmd)) << kXferCmdinxShift;
  switch (sd::response_of(cmd)) {
    case sd::Resp::None: break;
    case sd::Resp::R1:
    case sd::Resp::R6:
    case sd::Resp::R7: x |= (kRsptyp48 << kXferRsptypShift) | kXferCccen | kXferCicen; break;
    case sd::Resp::R1b: x |= (kRsptyp48Busy << kXferRsptypShift) | kXferCccen | kXferCicen; break;
    case sd::Resp::R2: x |= (kRsptyp136 << kXferRsptypShift) | kXferCccen; break;
    case sd::Resp::R3: x |= kRsptyp48 << kXferRsptypShift; break;
  }
  if (sd::writes_data(cmd)) {
    x |= kXferDpsel;  // OS-006c: data present, direction write (DTDSEL 0), a single block
  }
  if (sd::reads_data(cmd)) {
    x |= kXferDpsel | kXferDtdsel;
    if (cmd == sd::Cmd::ReadMultipleBlock) {
      x |= kXferMsbsel | kXferBcen | kXferAc12en;
    }
  }
  if (cmd == sd::Cmd::StopTransmission) {
    x |= kCmdtypAbort << kXferCmdtypShift;
  }
  return x;
}

// PRSSTAT.
inline constexpr std::uint32_t kPrsCihb = 1u << 0;   // command inhibit (CMD line busy)
inline constexpr std::uint32_t kPrsCdihb = 1u << 1;  // command inhibit (DAT lines busy)
inline constexpr std::uint32_t kPrsDla = 1u << 2;    // data line active
inline constexpr std::uint32_t kPrsSdstb = 1u << 3;  // SD clock stable
inline constexpr std::uint32_t kPrsBwen = 1u << 10;  // buffer write enable (OS-006c): room for at least WRWML words in the FIFO
inline constexpr std::uint32_t kPrsBren = 1u << 11;  // buffer read enable: at least RDWML words are in the FIFO
inline constexpr std::uint32_t kPrsDat0 = 1u << 24;  // DLSL: the level of DAT0 (low = the card is busy)

// PROCTL: DTW 2:1 (0 = 1 bit, 1 = 4 bits), EMODE 5:4 (2 = little endian: the first byte of the card is the lowest byte of a word).
inline constexpr std::uint32_t kProctlDtwShift = 1;
inline constexpr std::uint32_t kProctlDtwMask = 0x3u << kProctlDtwShift;
inline constexpr std::uint32_t kProctlEmodeShift = 4;
inline constexpr std::uint32_t kProctlEmodeMask = 0x3u << kProctlEmodeShift;
inline constexpr std::uint32_t kProctlEmodeLittle = 2;
constexpr std::uint32_t proctl_bus_width(std::uint32_t proctl, unsigned bits) noexcept {
  return (proctl & ~(kProctlDtwMask | kProctlEmodeMask)) | ((bits == 4u ? 1u : 0u) << kProctlDtwShift) | (kProctlEmodeLittle << kProctlEmodeShift);
}

// SYSCTL: IPGEN 0, HCKEN 1, PEREN 2 (clock gating of the module: left at their reset value), SDCLKEN 3, DVS 7:4, SDCLKFS 15:8,
// DTOCV 19:16, RSTA 24, RSTC 25, RSTD 26, INITA 27.
inline constexpr std::uint32_t kSysSdclken = 1u << 3;
inline constexpr std::uint32_t kSysDvsShift = 4;
inline constexpr std::uint32_t kSysSdclkfsShift = 8;
inline constexpr std::uint32_t kSysDtocvShift = 16;
inline constexpr std::uint32_t kSysDividerMask = (0xFu << kSysDvsShift) | (0xFFu << kSysSdclkfsShift);
inline constexpr std::uint32_t kSysDtocvMask = 0xFu << kSysDtocvShift;
inline constexpr std::uint32_t kSysRsta = 1u << 24;
inline constexpr std::uint32_t kSysRstc = 1u << 25;
inline constexpr std::uint32_t kSysRstd = 1u << 26;
inline constexpr std::uint32_t kSysInita = 1u << 27;
inline constexpr std::uint32_t kDataTimeoutExponent = 0xE;  // DTOCV: the controller's own data timeout is SDCLK x 2^(13 + 14) = 2^27 clocks: the software deadline cuts it first

// IRQSTAT / IRQSTATEN (the same positions; all write-one-to-clear in IRQSTAT).
inline constexpr std::uint32_t kIrqCc = 1u << 0;     // command complete
inline constexpr std::uint32_t kIrqTc = 1u << 1;     // transfer complete
inline constexpr std::uint32_t kIrqBrr = 1u << 5;    // buffer read ready
inline constexpr std::uint32_t kIrqCtoe = 1u << 16;  // command timeout
inline constexpr std::uint32_t kIrqCce = 1u << 17;   // command CRC error
inline constexpr std::uint32_t kIrqCebe = 1u << 18;  // command end bit error
inline constexpr std::uint32_t kIrqCie = 1u << 19;   // command index error
inline constexpr std::uint32_t kIrqDtoe = 1u << 20;  // data timeout
inline constexpr std::uint32_t kIrqDce = 1u << 21;   // data CRC error
inline constexpr std::uint32_t kIrqDebe = 1u << 22;  // data end bit error
inline constexpr std::uint32_t kIrqAc12e = 1u << 24; // auto CMD12 error
inline constexpr std::uint32_t kIrqCommandErrors = kIrqCtoe | kIrqCce | kIrqCebe | kIrqCie;
inline constexpr std::uint32_t kIrqDataErrors = kIrqDtoe | kIrqDce | kIrqDebe | kIrqAc12e;
// The flags the driver looks at: enabled in IRQSTATEN so that IRQSTAT shows them. No signal is enabled (IRQSIGEN stays 0, the NVIC line
// stays off): the driver polls, and the SDHC interrupt line of irq_map.h (Completion, priority 80) is reserved for a later DMA version.
inline constexpr std::uint32_t kIrqEnabled = kIrqCc | kIrqTc | kIrqBrr | kIrqCommandErrors | kIrqDataErrors;
inline constexpr std::uint32_t kIrqAll = 0xFFFFFFFFu;  // what a clear writes

// WML: RDWML 7:0, WRWML 23:16, in 32-bit words. A read is moved in groups of this many words: the FIFO holds 128, a block is 128. A write (OS-006c) is
// filled in groups of kWriteWatermarkWords the same way (WRWML is set to it together with RDWML at power-up).
inline constexpr std::uint32_t kReadWatermarkWords = 16;
inline constexpr std::uint32_t kWriteWatermarkWords = 16;
static_assert(sd::kBlockSize / 4u % kReadWatermarkWords == 0u, "a block is a whole number of watermark groups: no partial group to wait for");
static_assert(sd::kBlockSize / 4u % kWriteWatermarkWords == 0u, "a written block is a whole number of watermark groups");

// ---- The clock divider ------------------------------------------------------------------------------------------------------------
// SDCLK = source / (prescaler x divisor): SDCLKFS (a single bit, or 0) gives the prescaler 1, 2, 4 ... 256 (the field value is the
// prescaler / 2, 0 for 1) and DVS + 1 the divisor 1..16 (RM). plan_clock picks the pair with the highest frequency not above `max_hz`
// (the smallest total division at or above ceil(source / max)), the smallest prescaler on a tie; failed when even 256 x 16 is not enough.
struct ClockPlan {
  bool ok;
  std::uint8_t sdclkfs;  // register field value
  std::uint8_t dvs;      // register field value
  std::uint32_t hz;      // the resulting SDCLK
};

constexpr ClockPlan plan_clock(std::uint32_t source_hz, std::uint32_t max_hz) noexcept {
  ClockPlan best{false, 0, 0, 0};
  if (source_hz == 0u || max_hz == 0u) {
    return best;
  }
  std::uint32_t best_total = 0;
  for (std::uint32_t prescaler = 1; prescaler <= 256u; prescaler <<= 1) {
    for (std::uint32_t divisor = 1; divisor <= 16u; ++divisor) {
      const std::uint32_t total = prescaler * divisor;
      if (source_hz / total <= max_hz && (!best.ok || total < best_total)) {
        best.ok = true;
        best_total = total;
        best.sdclkfs = static_cast<std::uint8_t>(prescaler >> 1);
        best.dvs = static_cast<std::uint8_t>(divisor - 1u);
        best.hz = source_hz / total;
      }
    }
  }
  return best;
}

// The divider and data-timeout fields of SYSCTL for a plan (the caller keeps the other bits); SDCLKEN is set by the caller after the clock is
// stable (the divider must not change while it runs).
constexpr std::uint32_t sysctl_fields(const ClockPlan& p) noexcept {
  return (static_cast<std::uint32_t>(p.dvs) << kSysDvsShift) | (static_cast<std::uint32_t>(p.sdclkfs) << kSysSdclkfsShift) |
         (kDataTimeoutExponent << kSysDtocvShift);
}

inline constexpr std::uint32_t kIdentifyMaxHz = 400'000;       // the spec's identification clock: at most 400 kHz
inline constexpr std::uint32_t kDefaultSpeedMaxHz = 25'000'000;
inline constexpr std::uint32_t kHighSpeedMaxHz = 50'000'000;
inline constexpr unsigned kInitClockCount = 80;                  // INITA sends 80 clocks (spec: at least 74)

static_assert(plan_clock(kSdhcSourceHz, kIdentifyMaxHz).ok && plan_clock(kSdhcSourceHz, kIdentifyMaxHz).hz <= kIdentifyMaxHz, "400 kHz is reachable");
static_assert(plan_clock(kSdhcSourceHz, kHighSpeedMaxHz).hz <= kHighSpeedMaxHz && plan_clock(kSdhcSourceHz, kHighSpeedMaxHz).hz > 40'000'000u,
              "the 50 MHz step is within 20 % of what it asks");

// The interrupt line of the module (irq_map.h): a Completion line (priority 80), not enabled by this driver.
inline constexpr Irq kIrq = Irq::SDHC;
static_assert(role_of(kIrq) == hal::irq::Role::Completion, "the SDHC line is in the Completion row of the priority table");

}  // namespace o3sdk::platform::mk66::storage::sdhc
