// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-POWER P2: the record of the last shutdown, kept in the noinit SRAM so that it survives the software reset that ends every shutdown (the way the crash record does: a magic and a check,
// because a power-up leaves garbage there). The diag log is in SDRAM and the console ends at USB off (stage D), so on a run with no hands nothing else says how far the stages got; `diag power`
// prints this record after the boot. 28 bytes. The state machine (power_service.cpp) writes it as it goes, the image's off platform adds the result of the VLPR off (power_off.h), the host test
// reads it. Main loop only; every writer seals it, so a record is whole after each store.
//
// It holds: the last stage entered (Stage), the time each stage was entered in milliseconds from the start of stage A (saturated at 65535), the notes of the run (flags), the last refusal, and
// the result of the off (OffMark and OffStep packed in one byte). A new shutdown clears it; so does a refusal (the last event wins). It is read-only diagnostics: the boot path never reads it.

#include <cstdint>

namespace o3sdk::platform::mk66::power {

inline constexpr std::uint32_t kRecordMagic = 0x50575231u;  // "PWR1"
// Notes of a run (ShutdownRecord::flags).
inline constexpr std::uint8_t kRecDry = 1u << 0;              // `diag power off dry`
inline constexpr std::uint8_t kRecDeadline = 1u << 1;         // the global deadline fired
inline constexpr std::uint8_t kRecSaveCancelled = 1u << 2;    // a save in flight was cancelled after kSaveWaitMs
inline constexpr std::uint8_t kRecSaveCap = 1u << 3;          // a save was still running at the hard cap
inline constexpr std::uint8_t kRecCardBusy = 1u << 4;         // the card was released (or left) while something held it
inline constexpr std::uint8_t kRecLightsCap = 1u << 5;        // the lights stage ran out its cap
inline constexpr std::uint8_t kRecExportCancelled = 1u << 6;  // the second hold stopped an export

struct alignas(4) ShutdownRecord {  // (no member initialisers: the image keeps one in the noinit SRAM, where nothing may be initialised)
  std::uint32_t magic;
  std::uint8_t stage;    // the last Stage entered (0: none)
  std::uint8_t flags;    // kRec*
  std::uint8_t refusal;  // the last refusal (Refusal: 1 firmware, 2 export, 3 disk), 0 none
  std::uint8_t off;      // OffMark << 4 | OffStep, 0: no off was tried
  std::uint16_t t[8];    // t[s - 1]: Stage s was entered this many ms after stage A began (Quiet 1 .. Done 8, where Done is the call of enter_off)
  std::uint32_t check;

  void start(bool dry) noexcept;                       // a new shutdown: everything cleared
  void refused(std::uint8_t why) noexcept;             // a refusal: everything cleared, then the reason
  void enter(std::uint8_t stage_no, std::uint32_t ms_from_a) noexcept;
  void note(std::uint8_t bits) noexcept;               // flags |= bits
  void set_off(std::uint8_t mark, std::uint8_t step) noexcept;
  bool valid() const noexcept;                         // false: not a record (a power-up's garbage)
  void seal() noexcept;
  std::uint32_t sum() const noexcept;
};
static_assert(sizeof(ShutdownRecord) == 28 && alignof(ShutdownRecord) == 4, "28 bytes of noinit SRAM, word aligned (declared, not placed)");

}  // namespace o3sdk::platform::mk66::power
