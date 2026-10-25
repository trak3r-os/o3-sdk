// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The flash configuration field, bytes 0x400..0x40F of program flash (K66 Sub-Family Reference Manual, "Flash
// configuration field description"). The chip reads it at every reset: the backdoor comparison key (8 bytes), the
// program flash protection (FPROT, 4 bytes) and then FSEC, FOPT, FEPROT, FDPROT. FSEC decides whether the MCU is
// secured; **a wrong value can permanently lock the MCU**, and this tree has no way to take it back.
//
// The required image is the one the gate demands (the flashing-safety rules, skill fw-gate-check):
//
//     ff ff ff ff ff ff ff ff ff ff ff ff de f9 ff ff
//
//   FSEC 0xDE  SEC = 10b: unsecure. (Any other value in bits 1:0 secures the MCU; 0xDC, for one, is a locked variant.) FSLACC = 11b, MEEN = 01b (mass erase allowed), KEYEN = 11b (backdoor key disabled).
//   FOPT 0xF9  the flash options (bit layout in the same reference-manual table). This is the value every image of this OS has carried and the gate demands; this tree
//              does not decode its bits, and why this value was chosen is not recorded here (unknown). It is kept unchanged.
//   everything else erased (0xFF): no flash protection, no backdoor key.
//
// Four independent lines of defence, none of which trusts the others: the static_asserts below (compile time), the
// linker script (the section sits at exactly 0x400, 16 bytes, asserted), tools/check_image.py reading the Intel HEX
// back as a post-build step that deletes a failing image, and the `mk66_flashconfig` ctest, which carries its own copy
// of the expected bytes. Change this only together with the gate, on the maintainer's word.

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66 {

inline constexpr std::uint8_t kFsec = 0xDE;
inline constexpr std::uint8_t kFopt = 0xF9;

struct alignas(4) FlashConfig {
  std::uint8_t backdoor_key[8];  // 0x400: KEY0..7, all 0xFF (and FSEC[KEYEN] = 11b keeps the backdoor off anyway)
  std::uint8_t fprot[4];         // 0x408: program flash protection, 0xFF: nothing protected
  std::uint8_t fsec;             // 0x40C
  std::uint8_t fopt;             // 0x40D
  std::uint8_t feprot;           // 0x40E: EEPROM protection (FlexNVM), 0xFF
  std::uint8_t fdprot;           // 0x40F: data flash protection, 0xFF
};

inline constexpr FlashConfig kFlashConfig = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF}, kFsec, kFopt, 0xFF, 0xFF};

static_assert(sizeof(FlashConfig) == 16, "the flash configuration field is sixteen bytes");
static_assert(alignof(FlashConfig) == 4, "declared alignment (ADR 0011)");
static_assert(offsetof(FlashConfig, fsec) == 0x0C && offsetof(FlashConfig, fopt) == 0x0D, "FSEC at 0x40C, FOPT at 0x40D");
static_assert(offsetof(FlashConfig, feprot) == 0x0E && offsetof(FlashConfig, fdprot) == 0x0F, "FEPROT at 0x40E, FDPROT at 0x40F");

// FSEC[1:0] = 10b is the only unsecure encoding.
static_assert((kFsec & 0x03u) == 0x02u, "FSEC[SEC] must be 10b (unsecure): any other value secures the MCU");
static_assert(kFsec == 0xDE, "FSEC must be exactly 0xDE");
static_assert(kFopt == 0xF9, "FOPT must be exactly 0xF9");

constexpr bool flash_config_is_required(const FlashConfig& c) noexcept {
  for (std::uint8_t b : c.backdoor_key) {
    if (b != 0xFF) {
      return false;
    }
  }
  for (std::uint8_t b : c.fprot) {
    if (b != 0xFF) {
      return false;
    }
  }
  return c.fsec == 0xDE && c.fopt == 0xF9 && c.feprot == 0xFF && c.fdprot == 0xFF;
}
static_assert(flash_config_is_required(kFlashConfig), "ff ff ff ff ff ff ff ff ff ff ff ff de f9 ff ff");

}  // namespace o3sdk::platform::mk66
