// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The argument parsing and the allow-lists of the bring-up diagnostics `peek`, `poke` and `rot` (OS-HW1). Pure: the host tests run them,
// console.cpp does the access. The lists name the modules whose clock gate this image has switched on: a read of a gated module is a
// bus fault (a crash record, a reset), so everything not listed is refused. `poke` is narrower still: the audio clock registers only.

#include <cstdint>

namespace o3sdk::platform::mk66::console {

inline constexpr std::uint32_t kPeekMaxWords = 16;

struct AccessWindow {
  std::uint32_t first;  // inclusive
  std::uint32_t last;   // inclusive
  const char* name;
};

// The eDMA's base is defined here, not spelled in the table: tools/check_dma_owner.py accepts the base address only as a definition.
inline constexpr std::uint32_t kEdmaBase = 0x40008000u;
// OS-DIAG1: the eDMA is NOT one window. Its 8 KiB block is mostly reserved, and a read of a reserved word is a bus fault (0x40008008 faulted on the
// unit, 2026-10-14): one window per register that exists on the MK66 (K66 reference manual, eDMA memory map; every name but EARS is in NXP's device header):
// the 32-bit registers CR 0x000, ES 0x004, ERQ 0x00C, EEI 0x014, INT 0x024, ERR 0x02C, HRS 0x034, EARS 0x044 (EARS is from the manual only: the Teensy core's header omits it),
// DCHPRI 0x100..0x11F (32 byte registers; read here as words), the TCDs 0x1000..0x13FF (32 x 32 bytes). Left out: 0x018..0x01F (byte-wide write-only commands:
// CEEI SEEI CERQ SERQ CDNE SSRT CERR CINT) and every reserved gap. The windows are per register, so a multi-word peek never crosses a gap.
inline constexpr AccessWindow kPeekWindows[] = {
    {0x4002F000u, 0x4002F1FFu, "I2S0"},
    {0x40047000u, 0x40048FFFu, "SIM"},
    {0x40064000u, 0x400640FFu, "MCG"},
    {kEdmaBase + 0x000u, kEdmaBase + 0x003u, "eDMA CR"},
    {kEdmaBase + 0x004u, kEdmaBase + 0x007u, "eDMA ES"},
    {kEdmaBase + 0x00Cu, kEdmaBase + 0x00Fu, "eDMA ERQ"},
    {kEdmaBase + 0x014u, kEdmaBase + 0x017u, "eDMA EEI"},
    {kEdmaBase + 0x024u, kEdmaBase + 0x027u, "eDMA INT"},
    {kEdmaBase + 0x02Cu, kEdmaBase + 0x02Fu, "eDMA ERR"},
    {kEdmaBase + 0x034u, kEdmaBase + 0x037u, "eDMA HRS"},
    {kEdmaBase + 0x044u, kEdmaBase + 0x047u, "eDMA EARS"},
    {kEdmaBase + 0x100u, kEdmaBase + 0x11Fu, "eDMA DCHPRI"},
    {kEdmaBase + 0x1000u, kEdmaBase + 0x13FFu, "eDMA TCD"},
    {0x40021000u, 0x400210FFu, "DMAMUX"},
    {0x40049000u, 0x4004DFFFu, "PORTA-E"},
    {0x400FF000u, 0x400FFFFFu, "GPIO"},
    {0xE000E000u, 0xE000EFFFu, "SCS"},
};
inline constexpr AccessWindow kPokeWindow = {0x4002F000u, 0x4002F1FFu, "I2S0"};

enum class Access : std::uint8_t { Ok, Unaligned, Refused };

// `count` aligned 32-bit reads from `addr`: Ok when the address is word aligned and the whole range lies inside one listed window.
Access check_peek(std::uint32_t addr, std::uint32_t count) noexcept;
// One aligned 32-bit write: Ok only inside I2S0.
Access check_poke(std::uint32_t addr) noexcept;
// The window name of an address, or nullptr.
const char* peek_window_name(std::uint32_t addr) noexcept;

// "1A2B", "0x1A2B": 1..8 hex digits, an optional 0x. False for anything else (empty, a sign, a ninth digit).
bool parse_hex32(const char* text, std::uint32_t& out) noexcept;
// Decimal 0..max, digits only. False for anything else.
bool parse_dec(const char* text, std::uint32_t max, std::uint32_t& out) noexcept;

}  // namespace o3sdk::platform::mk66::console
