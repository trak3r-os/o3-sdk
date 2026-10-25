// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Peripheral registers of the MK66 and the Cortex-M4 core that the platform layer touches, with their field positions.
// Private to src/: nothing outside this directory sees a register. Written from the K66 Sub-Family Reference Manual
// (chapters named at each block) and the ARMv7-M Architecture Reference Manual; addresses and positions were
// cross-checked against NXP's MK66F18 device header (a fact table, not copied).

#include <o3sdk/platform/mk66/board.h>
#include <o3sdk/platform/mk66/edma.h>
#include <o3sdk/platform/mk66/i2c_config.h>

#include <cstdint>

namespace o3sdk::platform::mk66::regs {

template <typename T>
inline volatile T& at(std::uintptr_t address) noexcept {
  return *reinterpret_cast<volatile T*>(address);
}

// ---- Watchdog (RM "Watchdog Timer (WDOG)"), 16-bit registers at 0x4005_2000 -----------------------------------------
// Only what the reset sequence needs: unlock and switch off. Enabling it again is left to a later slice, see main.cpp.
inline constexpr std::uintptr_t kWdog = 0x40052000u;
inline volatile std::uint16_t& wdog_stctrlh() noexcept { return at<std::uint16_t>(kWdog + 0x0); }
inline volatile std::uint16_t& wdog_unlock() noexcept { return at<std::uint16_t>(kWdog + 0xE); }
inline constexpr std::uint16_t kWdogUnlock1 = 0xC520;
inline constexpr std::uint16_t kWdogUnlock2 = 0xD928;
inline constexpr std::uint16_t kWdogAllowUpdate = 1u << 4;  // STCTRLH: WDOGEN = 0, later writes to the control registers stay allowed
inline constexpr std::uint16_t kWdogEn = 1u << 0;           // [RM 26.8.1 p615] STCTRLH[WDOGEN]: the watchdog runs (OS-POWER P2 reads it; it is never set today)

// ---- SIM (RM "System Integration Module (SIM)") --------------------------------------------------------------------
inline volatile std::uint32_t& sim_sopt2() noexcept { return at<std::uint32_t>(0x40048004u); }
inline volatile std::uint32_t& sim_scgc5() noexcept { return at<std::uint32_t>(0x40048038u); }
inline volatile std::uint32_t& sim_scgc7() noexcept { return at<std::uint32_t>(0x40048040u); }
inline volatile std::uint32_t& sim_clkdiv1() noexcept { return at<std::uint32_t>(0x40048044u); }
inline constexpr std::uint32_t kScgc5PortA = 1u << 9;
inline constexpr std::uint32_t kScgc5PortB = 1u << 10;
inline constexpr std::uint32_t kScgc5PortC = 1u << 11;
inline constexpr std::uint32_t kScgc5PortD = 1u << 12;
inline constexpr std::uint32_t kScgc5PortE = 1u << 13;
inline constexpr std::uint32_t kScgc5Ports = kScgc5PortA | kScgc5PortB | kScgc5PortC | kScgc5PortD | kScgc5PortE;
inline constexpr std::uint32_t kScgc7Flexbus = 1u << 0;
inline constexpr std::uint32_t kScgc7Sdramc = 1u << 3;
inline constexpr std::uint32_t kSopt2ClkoutselMask = 0x7u << 5;
inline constexpr std::uint32_t kSopt2FbslMask = 0x3u << 8;

// ---- Oscillator, MCG, SMC (RM "OSC", "Multipurpose Clock Generator (MCG)", "System Mode Controller (SMC)") ----------
inline volatile std::uint8_t& osc0_cr() noexcept { return at<std::uint8_t>(0x40065000u); }
inline constexpr std::uint8_t kOscErclken = 1u << 7;  // external reference clock enable
inline constexpr std::uint8_t kOscSc2p = 1u << 3;     // 2 pF
inline constexpr std::uint8_t kOscSc8p = 1u << 1;     // 8 pF (with SC2P: 10 pF load)

inline constexpr std::uintptr_t kMcg = 0x40064000u;
inline volatile std::uint8_t& mcg_c1() noexcept { return at<std::uint8_t>(kMcg + 0x0); }
inline volatile std::uint8_t& mcg_c2() noexcept { return at<std::uint8_t>(kMcg + 0x1); }
inline volatile std::uint8_t& mcg_c5() noexcept { return at<std::uint8_t>(kMcg + 0x4); }
inline volatile std::uint8_t& mcg_c6() noexcept { return at<std::uint8_t>(kMcg + 0x5); }
inline volatile std::uint8_t& mcg_s() noexcept { return at<std::uint8_t>(kMcg + 0x6); }
inline constexpr std::uint8_t kMcgC1ClksShift = 6;
inline constexpr std::uint8_t kMcgC1FrdivShift = 3;
inline constexpr std::uint8_t kMcgC2Erefs = 1u << 2;       // external reference is a crystal (oscillator on)
inline constexpr std::uint8_t kMcgC2Lp = 1u << 1;          // [RM C2[LP] p632, BLPE p648, Table 6-1 p120] MCG_C2[LP]: low power, with CLKS = external and IREFS = 0 the MCG is in BLPE (PLL and FLL off, the crystal runs)
inline constexpr std::uint8_t kMcgC2RangeShift = 4;        // 2 = very high frequency range, 8-32 MHz
inline constexpr std::uint8_t kMcgC6Plls = 1u << 6;
inline constexpr std::uint8_t kMcgC6Cme0 = 1u << 5;       // MCG_C6[CME0]: the OSC0 clock monitor; it must be 0 for the low-power MCG modes (OS-POWER P2)
inline constexpr std::uint8_t kMcgSOscinit0 = 1u << 1;
inline constexpr std::uint8_t kMcgSClkstMask = 0x3u << 2;
inline constexpr std::uint8_t kMcgSIrefst = 1u << 4;
inline constexpr std::uint8_t kMcgSPllst = 1u << 5;
inline constexpr std::uint8_t kMcgSLock0 = 1u << 6;
inline constexpr std::uint8_t kClkstExternal = 2u << 2;  // MCGOUTCLK = external reference
inline constexpr std::uint8_t kClkstPll = 3u << 2;       // MCGOUTCLK = PLL

inline constexpr std::uintptr_t kSmc = 0x4007E000u;
inline volatile std::uint8_t& smc_pmprot() noexcept { return at<std::uint8_t>(kSmc + 0x0); }
inline volatile std::uint8_t& smc_pmctrl() noexcept { return at<std::uint8_t>(kSmc + 0x1); }
inline volatile std::uint8_t& smc_pmstat() noexcept { return at<std::uint8_t>(kSmc + 0x3); }
inline constexpr std::uint8_t kPmprotAhsrun = 0x80;  // allow HSRUN
inline constexpr std::uint8_t kPmprotAvlp = 0x20;    // allow very-low-power modes
inline constexpr std::uint8_t kPmprotAlls = 0x08;    // allow low-leakage stop
inline constexpr std::uint8_t kPmprotAvlls = 0x02;   // allow very-low-leakage stop
inline constexpr std::uint8_t kPmctrlRunmHsrun = 0x3u << 5;
inline constexpr std::uint8_t kPmstatHsrun = 0x80;
// OS-POWER P2, the VLPR off. [RM 16.3.2 p342, 16.3.4 p344-345; spec 4.7 RF2, RF3, checked against the reference manual 2026-10-17]: PMCTRL[RUNM] (bits 6:5) 00 RUN, 10 VLPR, 11 HSRUN, STOPM (2:0) left 0;
// PMSTAT is one-hot: 0x01 RUN, 0x04 VLPR, 0x80 HSRUN. HSRUN -> RUN -> VLPR are the only transitions used (a stop mode and VLPR cannot be entered from HSRUN); a reset returns the SMC to RUN.
inline constexpr std::uint8_t kPmctrlRunmRun = 0x0u << 5;
inline constexpr std::uint8_t kPmctrlRunmVlpr = 0x2u << 5;
inline constexpr std::uint8_t kPmstatRun = 0x01;
inline constexpr std::uint8_t kPmstatVlpr = 0x04;
// The encodings Mk66OffPlatform writes and waits for, pinned at compile time because it touches registers and has no host test. The RF numbers are the rows of the table in docs/specs/OS-POWER.md 4.7; each
// row was checked against the reference manual on 2026-10-17 (the pages are in the table), so a failure here means a typo in a constant.
static_assert(kPmprotAhsrun == 0x80u && kPmprotAvlp == 0x20u, "RF1: SMC_PMPROT AHSRUN is bit 7, AVLP bit 5");
static_assert(kPmctrlRunmRun == 0x00u && kPmctrlRunmVlpr == 0x40u && kPmctrlRunmHsrun == 0x60u, "RF2: SMC_PMCTRL[RUNM] (bits 6:5): 00 RUN, 10 VLPR, 11 HSRUN");
static_assert(((kPmctrlRunmRun | kPmctrlRunmVlpr | kPmctrlRunmHsrun) & ~0x60u) == 0u, "RF2: only RUNM is written, STOPM (bits 2:0) and every other bit stay 0");
static_assert(kPmstatRun == 0x01u && kPmstatVlpr == 0x04u && kPmstatHsrun == 0x80u, "RF3: SMC_PMSTAT is one-hot: 0x01 RUN, 0x04 VLPR, 0x80 HSRUN");
static_assert(kClkstExternal == 0x08u && kClkstPll == 0x0Cu && kMcgSClkstMask == 0x0Cu, "RF5: MCG_S[CLKST] (bits 3:2): 10b the external reference (PBE), 11b the PLL (PEE)");
static_assert(kMcgSPllst == 0x20u && kMcgC6Plls == 0x40u && kMcgC6Cme0 == 0x20u, "RF6: MCG_S[PLLST] is bit 5, MCG_C6[PLLS] bit 6, MCG_C6[CME0] bit 5");
static_assert(kMcgC2Lp == 0x02u && kMcgC2Erefs == 0x04u, "RF7: MCG_C2[LP] is bit 1 (EREFS stays set: the crystal runs)");
static_assert(kWdogEn == 0x0001u, "RF12: WDOG_STCTRLH[WDOGEN] is bit 0");

// ---- Reset control (RM "Reset Control Module (RCM)") --------------------------------------------------------------
inline volatile std::uint8_t& rcm_srs0() noexcept { return at<std::uint8_t>(0x4007F000u); }
inline volatile std::uint8_t& rcm_srs1() noexcept { return at<std::uint8_t>(0x4007F001u); }

// ---- Ports and GPIO (RM "Port control and interrupts (PORT)", "General-Purpose Input/Output (GPIO)") ---------------
inline constexpr std::uintptr_t kPortBase[5] = {0x40049000u, 0x4004A000u, 0x4004B000u, 0x4004C000u, 0x4004D000u};
inline constexpr std::uintptr_t kGpioBase[5] = {0x400FF000u, 0x400FF040u, 0x400FF080u, 0x400FF0C0u, 0x400FF100u};
inline constexpr std::uint32_t kPcrMuxShift = 8;
inline constexpr std::uint32_t kPcrMuxGpio = 1u << kPcrMuxShift;

inline volatile std::uint32_t& port_pcr(Pin pin) noexcept {
  return at<std::uint32_t>(kPortBase[static_cast<unsigned>(pin.port)] + 4u * pin.bit);
}
inline volatile std::uint32_t& gpio_psor(Pin pin) noexcept { return at<std::uint32_t>(kGpioBase[static_cast<unsigned>(pin.port)] + 0x4u); }
inline volatile std::uint32_t& gpio_pcor(Pin pin) noexcept { return at<std::uint32_t>(kGpioBase[static_cast<unsigned>(pin.port)] + 0x8u); }
inline volatile std::uint32_t& gpio_ptor(Pin pin) noexcept { return at<std::uint32_t>(kGpioBase[static_cast<unsigned>(pin.port)] + 0xCu); }
inline volatile std::uint32_t& gpio_pddr(Pin pin) noexcept { return at<std::uint32_t>(kGpioBase[static_cast<unsigned>(pin.port)] + 0x14u); }
constexpr std::uint32_t mask_of(Pin pin) noexcept { return 1u << pin.bit; }

// ---- Flexbus (RM "External Bus Interface (FlexBus)") ---------------------------------------------------------------
inline volatile std::uint32_t& fb_cspmcr() noexcept { return at<std::uint32_t>(0x4000C060u); }

// ---- Cortex-M4 core (ARMv7-M ARM, B3) -----------------------------------------------------------------------------
inline constexpr std::uintptr_t kScb = 0xE000ED00u;
inline volatile std::uint32_t& scb_aircr() noexcept { return at<std::uint32_t>(kScb + 0x0C); }
inline volatile std::uint32_t& scb_ccr() noexcept { return at<std::uint32_t>(kScb + 0x14); }
inline volatile std::uint8_t& scb_shp(unsigned exception_number) noexcept { return at<std::uint8_t>(kScb + 0x18 + (exception_number - 4u)); }
inline volatile std::uint32_t& scb_shcsr() noexcept { return at<std::uint32_t>(kScb + 0x24); }
inline volatile std::uint32_t& scb_cfsr() noexcept { return at<std::uint32_t>(kScb + 0x28); }
inline volatile std::uint32_t& scb_hfsr() noexcept { return at<std::uint32_t>(kScb + 0x2C); }
inline volatile std::uint32_t& scb_mmfar() noexcept { return at<std::uint32_t>(kScb + 0x34); }
inline volatile std::uint32_t& scb_bfar() noexcept { return at<std::uint32_t>(kScb + 0x38); }
inline volatile std::uint32_t& scb_cpacr() noexcept { return at<std::uint32_t>(kScb + 0x88); }
inline volatile std::uint32_t& scb_vtor() noexcept { return at<std::uint32_t>(kScb + 0x08); }
inline constexpr std::uint32_t kAircrVectkey = 0x5FAu << 16;
inline constexpr std::uint32_t kAircrPrigroupMask = 0x7u << 8;
inline constexpr std::uint32_t kAircrSysresetreq = 1u << 2;
inline constexpr std::uint32_t kCcrDiv0Trp = 1u << 4;
inline constexpr std::uint32_t kShcsrFaultsEnable = (1u << 16) | (1u << 17) | (1u << 18);  // MemManage, BusFault, UsageFault
inline constexpr std::uint32_t kCpacrFullFpu = 0xFu << 20;  // CP10 and CP11 full access

inline constexpr std::uintptr_t kNvicIpr = 0xE000E400u;
inline volatile std::uint8_t& nvic_ipr(unsigned irq) noexcept { return at<std::uint8_t>(kNvicIpr + irq); }

inline constexpr std::uintptr_t kSysTick = 0xE000E010u;
inline volatile std::uint32_t& syst_csr() noexcept { return at<std::uint32_t>(kSysTick + 0x0); }
inline volatile std::uint32_t& syst_rvr() noexcept { return at<std::uint32_t>(kSysTick + 0x4); }
inline volatile std::uint32_t& syst_cvr() noexcept { return at<std::uint32_t>(kSysTick + 0x8); }
inline volatile std::uint32_t& scb_icsr() noexcept { return at<std::uint32_t>(kScb + 0x04); }
inline constexpr std::uint32_t kSystCsrEnable = 1u << 0;
inline constexpr std::uint32_t kSystCsrTickint = 1u << 1;
inline constexpr std::uint32_t kSystCsrClksource = 1u << 2;  // processor clock
inline constexpr std::uint32_t kIcsrPendstset = 1u << 26;

inline volatile std::uint32_t& dwt_ctrl() noexcept { return at<std::uint32_t>(0xE0001000u); }
inline volatile std::uint32_t& dwt_cyccnt() noexcept { return at<std::uint32_t>(0xE0001004u); }
inline volatile std::uint32_t& demcr() noexcept { return at<std::uint32_t>(0xE000EDFCu); }
inline constexpr std::uint32_t kDemcrTrcena = 1u << 24;
inline constexpr std::uint32_t kDwtCyccntena = 1u << 0;


// ---- Audio (OS-005a): clock gates, I2S0, eDMA, DMAMUX, I2C0, NVIC enable and software pend ---------------------------------
// Addresses and fields: K66 Sub-Family Reference Manual ("SIM", "I2S", "eDMA", "DMAMUX", "I2C", ARMv7-M B3.4); the block addresses
// are edma.h's and i2c_config.h's (base library), the values written to them the drivers' own configuration headers'.
inline volatile std::uint32_t& sim_scgc4() noexcept { return at<std::uint32_t>(0x40048034u); }
inline volatile std::uint32_t& sim_scgc6() noexcept { return at<std::uint32_t>(0x4004803Cu); }
inline constexpr std::uint32_t kScgc4I2c0 = 1u << 6;
inline constexpr std::uint32_t kScgc6Dmamux = 1u << 1;
inline constexpr std::uint32_t kScgc6I2s = 1u << 15;
inline constexpr std::uint32_t kScgc7Dma = 1u << 1;

inline volatile std::uint32_t& reg32(std::uint32_t address) noexcept { return at<std::uint32_t>(address); }

inline volatile std::uint32_t& gpio_pdir(Pin pin) noexcept { return at<std::uint32_t>(kGpioBase[static_cast<unsigned>(pin.port)] + 0x10u); }

// eDMA control and the per-channel TCD (32 bytes at 0x4000_9000 + 32 x channel). The TCD is written field by field: its 16-bit
// fields need 16-bit stores.
inline volatile std::uint32_t& dma_cr() noexcept { return at<std::uint32_t>(edma::kBase + 0x00u); }
inline volatile std::uint32_t& dma_err() noexcept { return at<std::uint32_t>(edma::kBase + 0x2Cu); }
inline volatile std::uint8_t& dma_ceei() noexcept { return at<std::uint8_t>(edma::kBase + 0x18u); }
inline volatile std::uint8_t& dma_cerq() noexcept { return at<std::uint8_t>(edma::kBase + 0x1Au); }
inline volatile std::uint8_t& dma_serq() noexcept { return at<std::uint8_t>(edma::kBase + 0x1Bu); }
inline volatile std::uint8_t& dma_cdne() noexcept { return at<std::uint8_t>(edma::kBase + 0x1Cu); }
inline volatile std::uint8_t& dma_cerr() noexcept { return at<std::uint8_t>(edma::kBase + 0x1Eu); }
inline volatile std::uint8_t& dma_cint() noexcept { return at<std::uint8_t>(edma::kBase + 0x1Fu); }
inline constexpr std::uint8_t kDmaClearAll = 0x40;  // CERR[CAEI] / CINT[CAIR]: clear every channel's flag
// DCHPRIn: the byte registers are swizzled within each group of four (channel n is at offset n ^ 3).
inline volatile std::uint8_t& dma_dchpri(unsigned channel) noexcept { return at<std::uint8_t>(edma::kBase + 0x100u + (channel ^ 3u)); }
inline volatile edma::Tcd& dma_tcd(unsigned channel) noexcept {
  return at<edma::Tcd>(edma::kTcdBase + 32u * channel);
}
inline volatile std::uint8_t& dmamux_chcfg(unsigned channel) noexcept { return at<std::uint8_t>(edma::kMuxBase + channel); }

// I2C0 registers (byte wide).
inline volatile std::uint8_t& i2c0_f() noexcept { return at<std::uint8_t>(i2c_cfg::kI2c0Base + 0x1u); }
inline volatile std::uint8_t& i2c0_c1() noexcept { return at<std::uint8_t>(i2c_cfg::kI2c0Base + 0x2u); }
inline volatile std::uint8_t& i2c0_s() noexcept { return at<std::uint8_t>(i2c_cfg::kI2c0Base + 0x3u); }
inline volatile std::uint8_t& i2c0_d() noexcept { return at<std::uint8_t>(i2c_cfg::kI2c0Base + 0x4u); }
inline constexpr std::uint8_t kI2cC1Iicen = 0x80, kI2cC1Mst = 0x20, kI2cC1Tx = 0x10, kI2cC1Txak = 0x08, kI2cC1Rsta = 0x04;
inline constexpr std::uint8_t kI2cSBusy = 0x20, kI2cSArbl = 0x10, kI2cSIicif = 0x02, kI2cSRxak = 0x01;
inline constexpr std::uint32_t kPcrOdeDseSre = (1u << 5) | (1u << 6) | (1u << 2);  // open drain, high drive, slew-rate limited (the I2C pins)

// NVIC: set-enable, clear-pending and the software trigger register (STIR: pends an interrupt by number, privileged).
inline volatile std::uint32_t& nvic_iser(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E100u + 4u * (irq / 32u)); }
inline volatile std::uint32_t& nvic_icer(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E180u + 4u * (irq / 32u)); }
inline volatile std::uint32_t& nvic_icpr(unsigned irq) noexcept { return at<std::uint32_t>(0xE000E280u + 4u * (irq / 32u)); }
inline volatile std::uint32_t& nvic_stir() noexcept { return at<std::uint32_t>(0xE000EF00u); }
constexpr std::uint32_t irq_bit(unsigned irq) noexcept { return 1u << (irq % 32u); }

}  // namespace o3sdk::platform::mk66::regs
