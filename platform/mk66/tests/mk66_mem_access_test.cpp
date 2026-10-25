// SPDX-License-Identifier: GPL-3.0-or-later
// OS-HW1: the argument parsing and the allow-lists of `peek`, `poke` and `rot` (usb/mem_access.h). The lists are the modules the image has clocked
// on: a read outside them is a bus fault on the unit, so the edges are tested one word either side.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/usb/mem_access.h>

#include <cstdint>
#include <cstring>

using namespace o3sdk::platform::mk66::console;

TEST_CASE("mk66 mem access: hex and decimal arguments") {
  std::uint32_t v = 0;
  CHECK((parse_hex32("4002F100", v) && v == 0x4002F100u));
  CHECK((parse_hex32("0x4002f100", v) && v == 0x4002F100u));
  CHECK((parse_hex32("0XE000E000", v) && v == 0xE000E000u));
  CHECK((parse_hex32("0", v) && v == 0u));
  CHECK((parse_hex32("FFFFFFFF", v) && v == 0xFFFFFFFFu));
  CHECK_FALSE(parse_hex32("", v));
  CHECK_FALSE(parse_hex32("0x", v));
  CHECK_FALSE(parse_hex32("123456789", v));   // nine digits
  CHECK_FALSE(parse_hex32("0x123456789", v));
  CHECK_FALSE(parse_hex32("-1", v));
  CHECK_FALSE(parse_hex32("12G", v));
  CHECK_FALSE(parse_hex32("1 2", v));
  CHECK((parse_dec("0", 7, v) && v == 0u));
  CHECK((parse_dec("7", 7, v) && v == 7u));
  CHECK((parse_dec("16", kPeekMaxWords, v) && v == 16u));
  CHECK_FALSE(parse_dec("8", 7, v));
  CHECK_FALSE(parse_dec("17", kPeekMaxWords, v));
  CHECK_FALSE(parse_dec("", 7, v));
  CHECK_FALSE(parse_dec("-1", 7, v));
  CHECK_FALSE(parse_dec("1x", 7, v));
  CHECK_FALSE(parse_dec("99999999999999999999", 7, v));
}

TEST_CASE("mk66 mem access: peek is allowed in exactly the listed modules") {
  struct Window {
    std::uint32_t first, last;
    const char* name;
  };
  // (the eDMA is not here: its registers are windows of one to eight words each, see the next test)
  const Window listed[] = {{0x4002F000u, 0x4002F1FFu, "I2S0"},   {0x40047000u, 0x40048FFFu, "SIM"},     {0x40064000u, 0x400640FFu, "MCG"},
                           {0x40021000u, 0x400210FFu, "DMAMUX"}, {0x40049000u, 0x4004DFFFu, "PORTA-E"}, {0x400FF000u, 0x400FFFFFu, "GPIO"},
                           {0xE000E000u, 0xE000EFFFu, "SCS"}};
  for (const Window& w : listed) {
    INFO(w.name);
    CHECK(check_peek(w.first, 1) == Access::Ok);
    CHECK(check_peek(w.last - 3u, 1) == Access::Ok);     // the last word
    // The first word after and the last word before are refused, unless the neighbour is itself a listed module (SIM and PORTA are adjacent).
    CHECK((check_peek(w.last + 1u, 1) == Access::Refused) == (peek_window_name(w.last + 1u) == nullptr));
    CHECK((check_peek(w.first - 4u, 1) == Access::Refused) == (peek_window_name(w.first - 4u) == nullptr));
    CHECK(check_peek(w.last - 7u, 2) == Access::Ok);
    CHECK(check_peek(w.last - 3u, 2) == Access::Refused);  // a range that runs out of the window
    CHECK(std::strcmp(peek_window_name(w.first), w.name) == 0);
    CHECK(std::strcmp(peek_window_name(w.last), w.name) == 0);
  }
  // Windows that are adjacent still do not join: the range check is per window (SIM ends at 0x40048FFF, PORTA starts at 0x40049000).
  CHECK(check_peek(0x40048FFCu, 1) == Access::Ok);
  CHECK(check_peek(0x40048FFCu, 2) == Access::Refused);
  CHECK(check_peek(0x4002F1FCu, 16) == Access::Refused);
  CHECK(check_peek(0x4002F1FDu, 1) == Access::Unaligned);
  CHECK(peek_window_name(0x4002F200u) == nullptr);
  CHECK(peek_window_name(0x4002EFFCu) == nullptr);
  // Count: 1..16 words.
  CHECK(check_peek(0x4002F100u, 16) == Access::Ok);
  CHECK(check_peek(0x4002F100u, 17) == Access::Refused);
  CHECK(check_peek(0x4002F100u, 0) == Access::Refused);
  // Unaligned: said before anything else.
  CHECK(check_peek(0x4002F101u, 1) == Access::Unaligned);
  CHECK(check_peek(0x4002F102u, 1) == Access::Unaligned);
  CHECK(check_peek(0x4002F103u, 1) == Access::Unaligned);
  // Modules that are not clocked on in this image, and ordinary memory: refused.
  const std::uint32_t refused[] = {0x00000000u, 0x1FFF0000u, 0x20000000u, 0x70000000u, 0x4002E000u, 0x4002F200u, 0x40020000u, 0x4006A000u,
                                   0x40072000u, 0x40066000u, 0x400E0000u, 0x4000C000u, 0x4006B000u, 0xE0000000u, 0xE000F000u, 0xFFFFFFFCu};
  for (const std::uint32_t a : refused) {
    INFO(a);
    CHECK(check_peek(a, 1) == Access::Refused);
    CHECK(peek_window_name(a) == nullptr);
  }
  CHECK(check_peek(0xFFFFFFC0u, 16) == Access::Refused);  // no wrap-around past the top of the address space
}

TEST_CASE("mk66 mem access: peek reaches the eDMA registers that exist and none of its reserved words") {
  constexpr std::uint32_t kEdma = 0x40008000u;  // (a definition: tools/check_dma_owner.py takes the base address only so)
  struct Reg {
    std::uint32_t offset;
    std::uint32_t words;
    const char* name;
  };
  // OS-DIAG1: the K66 eDMA map. CR, ES, ERQ, EEI, INT, ERR, HRS, EARS are one word; DCHPRI is 8 words (32 bytes); the TCDs are 256 words (32 x 32 bytes).
  const Reg regs[] = {{0x000, 1, "eDMA CR"},  {0x004, 1, "eDMA ES"},  {0x00C, 1, "eDMA ERQ"},      {0x014, 1, "eDMA EEI"},
                      {0x024, 1, "eDMA INT"}, {0x02C, 1, "eDMA ERR"}, {0x034, 1, "eDMA HRS"},      {0x044, 1, "eDMA EARS"},
                      {0x100, 8, "eDMA DCHPRI"}, {0x1000, 256, "eDMA TCD"}};
  for (const Reg& r : regs) {
    INFO(r.name);
    const std::uint32_t first = kEdma + r.offset;
    const std::uint32_t last = first + 4u * r.words - 1u;
    CHECK(check_peek(first, 1) == Access::Ok);
    CHECK(check_peek(last - 3u, 1) == Access::Ok);  // the last word
    CHECK(std::strcmp(peek_window_name(first), r.name) == 0);
    CHECK(std::strcmp(peek_window_name(last), r.name) == 0);
    // The word before and the word after are reserved or another register: refused unless that neighbour is itself a window (it is never the same one).
    CHECK(check_peek(last + 1u, 1) == (peek_window_name(last + 1u) == nullptr ? Access::Refused : Access::Ok));
    CHECK((peek_window_name(first - 4u) == nullptr) == (check_peek(first - 4u, 1) == Access::Refused));
    // A range never leaves its register, whatever is behind it.
    CHECK(check_peek(first, r.words < kPeekMaxWords ? r.words : kPeekMaxWords) == Access::Ok);
    if (r.words < kPeekMaxWords) {
      CHECK(check_peek(first, r.words + 1u) == Access::Refused);
    }
  }
  // The word that faulted on the unit, and every reserved word between the 32-bit registers.
  CHECK(check_peek(0x40008008u, 1) == Access::Refused);
  CHECK(check_peek(0x40008010u, 1) == Access::Refused);
  CHECK(check_peek(0x40008018u, 1) == Access::Refused);  // the write-only command bytes CEEI.. CINT: 0x018..0x01F
  CHECK(check_peek(0x4000801Cu, 1) == Access::Refused);
  CHECK(check_peek(0x40008020u, 1) == Access::Refused);
  CHECK(check_peek(0x40008028u, 1) == Access::Refused);
  CHECK(check_peek(0x40008030u, 1) == Access::Refused);
  CHECK(check_peek(0x40008038u, 1) == Access::Refused);
  CHECK(check_peek(0x40008040u, 1) == Access::Refused);
  CHECK(check_peek(0x40008048u, 1) == Access::Refused);
  CHECK(check_peek(0x400080FCu, 1) == Access::Refused);
  CHECK(check_peek(0x40008120u, 1) == Access::Refused);  // after DCHPRI
  CHECK(check_peek(0x40008FFCu, 1) == Access::Refused);
  CHECK(check_peek(0x40009400u, 1) == Access::Refused);  // after the last TCD
  CHECK(check_peek(0x40009FFCu, 1) == Access::Refused);
  // Ranges across a gap: CR + ES are two windows side by side (no gap), but a range does not join them; ES + the gap after it is refused too.
  CHECK(check_peek(kEdma + 0x000u, 2) == Access::Refused);
  CHECK(check_peek(0x40008004u, 2) == Access::Refused);
  CHECK(check_peek(kEdma + 0x100u, 9) == Access::Refused);
  CHECK(check_peek(0x40009000u, 16) == Access::Ok);
  CHECK(check_peek(0x400093FCu, 2) == Access::Refused);  // the last TCD word and the reserved one after it
}

TEST_CASE("mk66 mem access: poke reaches I2S0 and nothing else") {
  CHECK(check_poke(0x4002F000u) == Access::Ok);
  CHECK(check_poke(0x4002F100u) == Access::Ok);  // MCR
  CHECK(check_poke(0x4002F104u) == Access::Ok);  // MDR
  CHECK(check_poke(0x4002F1FCu) == Access::Ok);
  CHECK(check_poke(0x4002F200u) == Access::Refused);
  CHECK(check_poke(0x4002EFFCu) == Access::Refused);
  CHECK(check_poke(0x4002F101u) == Access::Unaligned);
  // Every other module of the peek list is read-only.
  for (const AccessWindow& w : kPeekWindows) {
    if (w.first != kPokeWindow.first) {
      INFO(w.name);
      CHECK(check_poke(w.first) == Access::Refused);
      CHECK(check_poke(w.last - 3u) == Access::Refused);
    }
  }
  CHECK(check_poke(0x400FF000u) == Access::Refused);  // GPIO
  CHECK(check_poke(0xE000ED00u) == Access::Refused);  // SCB
  CHECK(check_poke(0x20000000u) == Access::Refused);
  CHECK(check_poke(0x00000000u) == Access::Refused);
}
