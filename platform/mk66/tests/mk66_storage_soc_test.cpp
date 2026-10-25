// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SDHC facts the image's hardware half (storage/src/sdhc_bus.cpp) writes, against a table of values each cited from the two fact sources
// this tree reads: NXP's MK66F18 device header (`*_MASK`) and the Teensy core header.
// The register writes themselves cannot run on the host or on QEMU, so a wrong bit position (the SDHC clock gate at
// a neighbour's bit, a response type in the wrong field) would be a card that never answers: this table is what stops it. Then the arithmetic that
// does run: the clock divider search (against the dividers for the same two clocks), and the transfer-type word of every command,
// by hand. Runs on the host and on QEMU's Cortex-M4F.
#include <doctest/doctest.h>

#include "mk66_storage_need.h"

#include <o3sdk/platform/mk66/storage/sd_policy.h>
#include <o3sdk/platform/mk66/storage/sd_protocol.h>
#include <o3sdk/platform/mk66/storage/sdhc_facts.h>

#include <cstddef>
#include <cstdint>

namespace sdhc = o3sdk::platform::mk66::storage::sdhc;
namespace sd = o3sdk::platform::mk66::storage::sd;

namespace {

struct Fact {
  const char* name;
  std::uint32_t ours;
  std::uint32_t expected;
  const char* cited;  // where the expected value is read
};

}  // namespace

TEST_CASE("sdhc soc: every register bit and field is the value the fact tables give") {
  const Fact facts[] = {
      {"SIM_SCGC3 SDHC", sdhc::kScgc3Sdhc, 0x00020000u, "MK66F18.h SIM_SCGC3_SDHC_MASK, kinetis.h SIM_SCGC3_SDHC"},
      {"SIM_SOPT2 SDHCSRC", sdhc::kSopt2SdhcSrcMask, 0x30000000u, "MK66F18.h SIM_SOPT2_SDHCSRC_MASK"},
      {"PORT_PCR PS", sdhc::kPcrPs, 0x1u, "MK66F18.h PORT_PCR_PS_MASK"},
      {"PORT_PCR PE", sdhc::kPcrPe, 0x2u, "MK66F18.h PORT_PCR_PE_MASK"},
      {"PORT_PCR DSE", sdhc::kPcrDse, 0x40u, "MK66F18.h PORT_PCR_DSE_MASK"},
      {"XFERTYP DMAEN", sdhc::kXferDmaen, 0x1u, "MK66F18.h SDHC_XFERTYP_DMAEN_MASK"},
      {"XFERTYP BCEN", sdhc::kXferBcen, 0x2u, "MK66F18.h SDHC_XFERTYP_BCEN_MASK"},
      {"XFERTYP AC12EN", sdhc::kXferAc12en, 0x4u, "MK66F18.h SDHC_XFERTYP_AC12EN_MASK"},
      {"XFERTYP DTDSEL", sdhc::kXferDtdsel, 0x10u, "MK66F18.h SDHC_XFERTYP_DTDSEL_MASK"},
      {"XFERTYP MSBSEL", sdhc::kXferMsbsel, 0x20u, "MK66F18.h SDHC_XFERTYP_MSBSEL_MASK"},
      {"XFERTYP RSPTYP (field)", 0x3u << sdhc::kXferRsptypShift, 0x30000u, "MK66F18.h SDHC_XFERTYP_RSPTYP_MASK"},
      {"XFERTYP CCCEN", sdhc::kXferCccen, 0x80000u, "MK66F18.h SDHC_XFERTYP_CCCEN_MASK"},
      {"XFERTYP CICEN", sdhc::kXferCicen, 0x100000u, "MK66F18.h SDHC_XFERTYP_CICEN_MASK"},
      {"XFERTYP DPSEL", sdhc::kXferDpsel, 0x200000u, "MK66F18.h SDHC_XFERTYP_DPSEL_MASK"},
      {"XFERTYP CMDTYP (field)", 0x3u << sdhc::kXferCmdtypShift, 0xC00000u, "MK66F18.h SDHC_XFERTYP_CMDTYP_MASK"},
      {"XFERTYP CMDINX (field)", 0x3Fu << sdhc::kXferCmdinxShift, 0x3F000000u, "MK66F18.h SDHC_XFERTYP_CMDINX_MASK"},
      {"PRSSTAT CIHB", sdhc::kPrsCihb, 0x1u, "MK66F18.h SDHC_PRSSTAT_CIHB_MASK"},
      {"PRSSTAT CDIHB", sdhc::kPrsCdihb, 0x2u, "MK66F18.h SDHC_PRSSTAT_CDIHB_MASK"},
      {"PRSSTAT DLA", sdhc::kPrsDla, 0x4u, "MK66F18.h SDHC_PRSSTAT_DLA_MASK"},
      {"PRSSTAT SDSTB", sdhc::kPrsSdstb, 0x8u, "MK66F18.h SDHC_PRSSTAT_SDSTB_MASK"},
      {"PRSSTAT BWEN", sdhc::kPrsBwen, 0x400u, "MK66F18.h SDHC_PRSSTAT_BWEN_MASK"},
      {"PRSSTAT BREN", sdhc::kPrsBren, 0x800u, "MK66F18.h SDHC_PRSSTAT_BREN_MASK"},
      {"PRSSTAT DLSL bit 0 (DAT0)", sdhc::kPrsDat0, 0x01000000u, "MK66F18.h SDHC_PRSSTAT_DLSL_MASK 0xFF000000, DAT0 is its lowest bit (RM)"},
      {"PROCTL DTW (field)", sdhc::kProctlDtwMask, 0x6u, "MK66F18.h SDHC_PROCTL_DTW_MASK"},
      {"PROCTL EMODE (field)", sdhc::kProctlEmodeMask, 0x30u, "MK66F18.h SDHC_PROCTL_EMODE_MASK"},
      {"SYSCTL SDCLKEN", sdhc::kSysSdclken, 0x8u, "MK66F18.h SDHC_SYSCTL_SDCLKEN_MASK"},
      {"SYSCTL DVS (field)", 0xFu << sdhc::kSysDvsShift, 0xF0u, "MK66F18.h SDHC_SYSCTL_DVS_MASK"},
      {"SYSCTL SDCLKFS (field)", 0xFFu << sdhc::kSysSdclkfsShift, 0xFF00u, "MK66F18.h SDHC_SYSCTL_SDCLKFS_MASK"},
      {"SYSCTL DTOCV (field)", sdhc::kSysDtocvMask, 0xF0000u, "MK66F18.h SDHC_SYSCTL_DTOCV_MASK"},
      {"SYSCTL RSTA", sdhc::kSysRsta, 0x1000000u, "MK66F18.h SDHC_SYSCTL_RSTA_MASK"},
      {"SYSCTL RSTC", sdhc::kSysRstc, 0x2000000u, "MK66F18.h SDHC_SYSCTL_RSTC_MASK"},
      {"SYSCTL RSTD", sdhc::kSysRstd, 0x4000000u, "MK66F18.h SDHC_SYSCTL_RSTD_MASK"},
      {"SYSCTL INITA", sdhc::kSysInita, 0x8000000u, "MK66F18.h SDHC_SYSCTL_INITA_MASK"},
      {"IRQSTAT CC", sdhc::kIrqCc, 0x1u, "MK66F18.h SDHC_IRQSTAT_CC_MASK"},
      {"IRQSTAT TC", sdhc::kIrqTc, 0x2u, "MK66F18.h SDHC_IRQSTAT_TC_MASK"},
      {"IRQSTAT BRR", sdhc::kIrqBrr, 0x20u, "MK66F18.h SDHC_IRQSTAT_BRR_MASK"},
      {"IRQSTAT CTOE", sdhc::kIrqCtoe, 0x10000u, "MK66F18.h SDHC_IRQSTAT_CTOE_MASK"},
      {"IRQSTAT CCE", sdhc::kIrqCce, 0x20000u, "MK66F18.h SDHC_IRQSTAT_CCE_MASK"},
      {"IRQSTAT CEBE", sdhc::kIrqCebe, 0x40000u, "MK66F18.h SDHC_IRQSTAT_CEBE_MASK"},
      {"IRQSTAT CIE", sdhc::kIrqCie, 0x80000u, "MK66F18.h SDHC_IRQSTAT_CIE_MASK"},
      {"IRQSTAT DTOE", sdhc::kIrqDtoe, 0x100000u, "MK66F18.h SDHC_IRQSTAT_DTOE_MASK"},
      {"IRQSTAT DCE", sdhc::kIrqDce, 0x200000u, "MK66F18.h SDHC_IRQSTAT_DCE_MASK"},
      {"IRQSTAT DEBE", sdhc::kIrqDebe, 0x400000u, "MK66F18.h SDHC_IRQSTAT_DEBE_MASK"},
      {"IRQSTAT AC12E", sdhc::kIrqAc12e, 0x1000000u, "MK66F18.h SDHC_IRQSTAT_AC12E_MASK"},
      {"BLKATTR BLKSIZE (field)", 0x1FFFu, 0x1FFFu, "MK66F18.h SDHC_BLKATTR_BLKSIZE_MASK"},
      {"BLKATTR BLKCNT (field)", sdhc::blkattr(0, 0xFFFFu), 0xFFFF0000u, "MK66F18.h SDHC_BLKATTR_BLKCNT_MASK"},
  };
  for (const Fact& f : facts) {
    CHECK_MESSAGE(f.ours == f.expected, f.name << ": " << f.cited);
  }
  // Addresses: SDHC_BASE (MK66F18.h) and the register offsets of the struct (MK66F18.h SDHC_Type, offsets in its comments).
  CHECK(sdhc::kBase == 0x400B1000u);
  CHECK(sdhc::kOffDsaddr == 0x00u);
  CHECK(sdhc::kOffBlkattr == 0x04u);
  CHECK(sdhc::kOffCmdarg == 0x08u);
  CHECK(sdhc::kOffXfertyp == 0x0Cu);
  CHECK(sdhc::kOffCmdrsp0 == 0x10u);
  CHECK(sdhc::kOffDatport == 0x20u);
  CHECK(sdhc::kOffPrsstat == 0x24u);
  CHECK(sdhc::kOffProctl == 0x28u);
  CHECK(sdhc::kOffSysctl == 0x2Cu);
  CHECK(sdhc::kOffIrqstat == 0x30u);
  CHECK(sdhc::kOffIrqstaten == 0x34u);
  CHECK(sdhc::kOffIrqsigen == 0x38u);
  CHECK(sdhc::kOffWml == 0x44u);
  CHECK(sdhc::kSimScgc3 == 0x40048030u);  // kinetis.h
  CHECK(static_cast<unsigned>(sdhc::kIrq) == 81u);  // kinetis.h IRQ_SDHC
}

TEST_CASE("sdhc soc: the pins are PTE0-5 at ALT4, the detect switch on PTD0, and the CMD/data pull-ups are on") {
  using namespace o3sdk::platform::mk66;
  // the pin setup: PORTE_PCR0 D1, 1 D0, 2 CLK, 3 CMD, 4 D3, 5 D2; PCR = MUX(4) | DSE, plus PE | PS on everything but the clock.
  CHECK(sdhc::kD1 == Pin{Port::E, 0});
  CHECK(sdhc::kD0 == Pin{Port::E, 1});
  CHECK(sdhc::kClk == Pin{Port::E, 2});
  CHECK(sdhc::kCmd == Pin{Port::E, 3});
  CHECK(sdhc::kD3 == Pin{Port::E, 4});
  CHECK(sdhc::kD2 == Pin{Port::E, 5});
  CHECK(sdhc::kPcrClk == 0x440u);       // MUX 4 (4 << 8 = 0x400) | DSE (0x40)
  CHECK(sdhc::kPcrCmdData == 0x443u);   // ... | PE | PS
  CHECK(sdhc::kPcrCardDetect == 0x103u);  // GPIO (MUX 1), pulled up
  // The card detect is Teensy pin 2 = PTD0, read inverted (a card pulls it low).
  CHECK(sdhc::kCardDetect == Pin{Port::D, 0});
  CHECK(sdhc::kCardDetectActiveLow);
  // The six lines are distinct and none is another driver's (board.h lists the pins already claimed).
  const Pin lines[] = {sdhc::kD1, sdhc::kD0, sdhc::kClk, sdhc::kCmd, sdhc::kD3, sdhc::kD2, sdhc::kCardDetect};
  for (std::size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
    for (std::size_t j = i + 1; j < sizeof lines / sizeof lines[0]; ++j) {
      CHECK(!(lines[i] == lines[j]));
    }
    CHECK(!(lines[i] == board::kI2sMclk));
    CHECK(!(lines[i] == board::kI2sRxd));
    CHECK(!(lines[i] == board::kI2sTxd));
    CHECK(!(lines[i] == board::kI2sLrclk));
    CHECK(!(lines[i] == board::kI2sBclk));
    CHECK(!(lines[i] == board::kMidiTx));
    CHECK(!(lines[i] == board::kSdramPower));
  }
}

TEST_CASE("sdhc clock: the divider search lands on the known dividers for 400 kHz and 50 MHz") {
  // 400 kHz: SDCLKFS 0x10, DVS 14 (375 kHz); the NXP SDHC_SetSdClock for the 50 MHz step at 180 MHz: SDCLKFS 0, DVS 3 (45 MHz).
  const sdhc::ClockPlan slow = sdhc::plan_clock(180'000'000u, 400'000u);
  CHECK(slow.ok);
  CHECK(slow.sdclkfs == 0x10);
  CHECK(slow.dvs == 14);
  CHECK(slow.hz == 375'000u);
  const sdhc::ClockPlan fast = sdhc::plan_clock(180'000'000u, 50'000'000u);
  CHECK(fast.ok);
  CHECK(fast.sdclkfs == 0);
  CHECK(fast.dvs == 3);
  CHECK(fast.hz == 45'000'000u);
  const sdhc::ClockPlan normal = sdhc::plan_clock(180'000'000u, 25'000'000u);
  CHECK(normal.ok);
  CHECK(normal.sdclkfs == 0);
  CHECK(normal.dvs == 7);
  CHECK(normal.hz == 22'500'000u);
  // SYSCTL's fields for the 400 kHz plan: DVS 14 << 4 | SDCLKFS 0x10 << 8 | DTOCV 0xE << 16.
  CHECK(sdhc::sysctl_fields(slow) == 0x000E10E0u);
}

TEST_CASE("sdhc clock: for every target the plan is the fastest clock not above the limit, and the extremes are refused") {
  for (std::uint32_t max_hz = 100'000u; max_hz <= 60'000'000u; max_hz += 99'991u) {
    const sdhc::ClockPlan p = sdhc::plan_clock(180'000'000u, max_hz);
    NEED(p.ok);
    CHECK(p.hz <= max_hz);
    // Exhaustive: no prescaler / divisor pair gives a higher frequency that is still not above the limit.
    for (std::uint32_t prescaler = 1; prescaler <= 256u; prescaler <<= 1) {
      for (std::uint32_t divisor = 1; divisor <= 16u; ++divisor) {
        const std::uint32_t hz = 180'000'000u / (prescaler * divisor);
        if (hz <= max_hz) {
          CHECK(hz <= p.hz);
        }
      }
    }
    // The register fields stand for the frequency: prescaler is 2 * SDCLKFS (1 for 0), divisor DVS + 1.
    const std::uint32_t prescaler = p.sdclkfs == 0 ? 1u : 2u * p.sdclkfs;
    CHECK(180'000'000u / (prescaler * (p.dvs + 1u)) == p.hz);
    CHECK(((p.sdclkfs & (p.sdclkfs - 1u)) == 0u));  // one bit or none
  }
  CHECK(!sdhc::plan_clock(180'000'000u, 0).ok);
  CHECK(!sdhc::plan_clock(0, 400'000u).ok);
  CHECK(!sdhc::plan_clock(180'000'000u, 40'000u).ok);  // 180 MHz / (256 * 16) = 43.9 kHz: not reachable below that
}

TEST_CASE("sdhc xfertyp: the transfer-type word of every command, worked out by hand") {
  using sd::Cmd;
  struct Row {
    Cmd cmd;
    std::uint32_t expected;
    const char* what;
  };
  const Row rows[] = {
      {Cmd::GoIdle, 0x00000000u, "CMD0: no response"},
      {Cmd::AllSendCid, 0x02090000u, "CMD2: R2 (136 bits, CRC checked, no index)"},
      {Cmd::SendRelativeAddr, 0x031A0000u, "CMD3: R6 (48 bits, CRC and index)"},
      {Cmd::SwitchFunc, 0x063A0010u, "CMD6: R1 + data present, read"},
      {Cmd::SelectCard, 0x071B0000u, "CMD7: R1b"},
      {Cmd::SendIfCond, 0x081A0000u, "CMD8: R7"},
      {Cmd::SendCsd, 0x09090000u, "CMD9: R2"},
      {Cmd::StopTransmission, 0x0CDB0000u, "CMD12: R1b, abort type"},
      {Cmd::SendStatus, 0x0D1A0000u, "CMD13: R1"},
      {Cmd::SetBlockLen, 0x101A0000u, "CMD16: R1"},
      {Cmd::ReadSingleBlock, 0x113A0010u, "CMD17: R1 + data present, read"},
      {Cmd::ReadMultipleBlock, 0x123A0036u, "CMD18: ... + multiple blocks, block count, auto CMD12"},
      {Cmd::WriteBlock, 0x183A0000u, "CMD24: R1 + data present, direction write (DTDSEL 0), a single block"},
      {Cmd::AppCmd, 0x371A0000u, "CMD55: R1"},
      {Cmd::AcmdSetBusWidth, 0x061A0000u, "ACMD6: R1 (index 6 on the wire)"},
      {Cmd::AcmdSdSendOpCond, 0x29020000u, "ACMD41: R3 (48 bits, nothing checked)"},
  };
  for (const Row& r : rows) {
    CHECK_MESSAGE(sdhc::xfertyp(r.cmd) == r.expected, r.what);
  }
  // No command asks for DMA: the driver is polled.
  for (const Row& r : rows) {
    CHECK((sdhc::xfertyp(r.cmd) & sdhc::kXferDmaen) == 0u);
  }
}

TEST_CASE("sdhc: the mode of PROCTL and the flags the driver enables") {
  // 4 data lines: DTW 1; little endian: EMODE 2; everything else is kept.
  CHECK(sdhc::proctl_bus_width(0x00000020u, 4) == 0x00000022u);
  CHECK(sdhc::proctl_bus_width(0x00000022u, 1) == 0x00000020u);
  CHECK(sdhc::proctl_bus_width(0xFFFFFFFFu, 4) == (0xFFFFFFFFu & ~0x36u) + 0x22u);
  // The flags the driver reads are all enabled in IRQSTATEN, and no interrupt signal is (IRQSIGEN stays 0: polled).
  CHECK((sdhc::kIrqEnabled & sdhc::kIrqCc) != 0u);
  CHECK((sdhc::kIrqEnabled & sdhc::kIrqTc) != 0u);
  CHECK((sdhc::kIrqEnabled & sdhc::kIrqBrr) != 0u);
  CHECK((sdhc::kIrqEnabled & sdhc::kIrqCommandErrors) == sdhc::kIrqCommandErrors);
  CHECK((sdhc::kIrqEnabled & sdhc::kIrqDataErrors) == sdhc::kIrqDataErrors);
  // A block is a whole number of watermark groups, and the 64-byte switch status is one.
  CHECK((sd::kBlockSize / 4u) % sdhc::kReadWatermarkWords == 0u);
  CHECK((sd::kSwitchStatusBytes / 4u) % sdhc::kReadWatermarkWords == 0u);
  CHECK((sd::kBlockSize / 4u) % sdhc::kWriteWatermarkWords == 0u);
}

TEST_CASE("sd policy: a Deadline survives a wrap of the counter, and the limits bound every call") {
  namespace pol = sd::policy;
  const pol::Deadline d{0xFFFFFF00u, 0x200u};  // starts 256 ticks before the wrap, lasts 512
  CHECK(!d.expired(0xFFFFFF00u));
  CHECK(!d.expired(0xFFFFFFFFu));
  CHECK(!d.expired(0x000000FFu));  // 511 ticks in
  CHECK(d.expired(0x00000100u));   // 512
  CHECK(d.expired(0x00001000u));
  // Microseconds to cycles at 180 MHz, saturating below half the counter range.
  CHECK(pol::to_ticks(1000u, 180u) == 180'000u);
  CHECK(pol::to_ticks(0xFFFFFFFFu, 180u) == 0x7FFFFFFFu);
  CHECK(pol::to_ticks(1'000'000u, 180u) == 180'000'000u);
  // Every limit the driver uses is well inside the saturation, so none is silently cut short.
  const std::uint32_t limits[] = {pol::kControllerResetUs, pol::kClockStableUs, pol::kInitClocksUs, pol::kInhibitUs, pol::kCommandUs, pol::kBusyUs,
                                  pol::data_timeout_us(pol::kMaxBlocksPerCommand), pol::kStopBusyUs, pol::kAcmd41TotalUs};
  for (const std::uint32_t us : limits) {
    CHECK(pol::to_ticks(us, 180u) < 0x7FFFFFFFu);
    CHECK(us > 0u);
  }
  // The time one call can hold the main loop, when the card fails: under half a second for a read, under three for the whole init.
  CHECK(pol::worst_read_call_us() < 500'000u);
  CHECK(pol::worst_init_us() < 3'000'000u);
  CHECK(pol::worst_write_block_us() < 2'000'000u);  // OS-006c: one block of a write that never completes
  CHECK(pol::to_ticks(pol::kWriteBusyUs, 180u) < 0x7FFFFFFFu);
  CHECK(pol::kWriteBusyUs >= 250'000u);             // the spec's SDHC programming time is inside it
  CHECK(pol::worst_init_us() >= pol::kAcmd41TotalUs);  // the spec's second is in it
  // The chunk: 16 KB, a data timeout that grows with the blocks.
  CHECK(pol::kMaxBlocksPerCommand * sd::kBlockSize == 16u * 1024u);
  CHECK(pol::data_timeout_us(32) > pol::data_timeout_us(1));
  CHECK(pol::data_timeout_us(1) >= 100'000u);  // the spec's read timeout (100 ms) is inside it
}
