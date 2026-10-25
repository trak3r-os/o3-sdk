// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The module ids the console's image adds to the crash record's (main.cpp: display 1, input 2, app 3, audio 4; 0 is the main loop), and
// the rule that keeps a USB crash from becoming a crash loop (docs/M2B-PLAN.md section 6, mitigation c).

#include <o3sdk/platform/mk66/crash_record.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

inline constexpr std::uint16_t kStorageModule = 5;  // set_module() while the SD card is brought up
inline constexpr std::uint16_t kUsbModule = 6;      // set_module() while USB is started

// IPSR 16 + 53: the exception number of the USB0 interrupt (irq_map.h: IRQ 53).
inline constexpr std::uint32_t kUsbIrqException = 16u + 53u;
inline constexpr std::uint32_t kIpsrMask = 0x1FFu;  // the exception number field of xPSR

// The previous run died in USB: the next boot then leaves USB off (usb0::start is not called) and the unit stays a working instrument (the console is out of reach until the
// next power-up). The record is cleared as always, so the boot after that tries USB again; a fault that comes back every time therefore costs one reset per power-up and no more.
//
// What a record has to say to be blamed on USB (nullptr: there was none; a record that fails its magic or CRC counts as none), any one of:
//   1. its module word is kUsbModule: the fault came while main was inside usb0::start(), the only time main sets that module;
//   2. its stacked IPSR is the USB interrupt's (69): the code that was running when the fault was taken was the USB ISR, which is where a fault of enumeration or of a stream
//      lands (the fault handler's own IPSR is 3, 4 or 5 then, never 69, and the module word is 0 by then, so only the stacked xPSR tells);
//   3. the handler's own IPSR is 69: the USB interrupt taken with no handler installed (a default handler's number).
// Not covered: a fault that USB causes but that is taken elsewhere (the main loop reading a ring the interrupt corrupted, the render line's tap), and a record whose stack frame was not
// usable (the stacked xPSR is then stored as 0, crash_record.h snapshot()). Unplugging the host ends a loop of the second kind: with no host there are no USB interrupts.
inline bool crash_blames_usb(const crash::CrashRecord* record) noexcept {
  if (record == nullptr || !crash::valid(*record)) {
    return false;
  }
  return crash::module_of(record->context) == kUsbModule || (record->xpsr & kIpsrMask) == kUsbIrqException || record->exception == kUsbIrqException;
}

}  // namespace o3sdk::platform::mk66::console
