// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// CRASH TEST (session only): the faults the MK66 platform's crash record is meant to catch, one per kind, and what the record and the status LED must
// say after each (platform/mk66/README.md, "Faults and blink codes"; docs/FRAMEWORK.md section 10). The table is data: the console command lists it, the
// screen draws it, CHECKLIST.md repeats it, and a host test pins the names.
//
// What the platform enables, and so what is offered: MemManage, BusFault and UsageFault have handlers of their own (SHCSR, boot.cpp) and so does every
// device interrupt (a weak default that records "IRQ n (no handler)"); CCR.DIV_0_TRP is set; CCR.UNALIGN_TRP is not (a plain unaligned LDR works: only a
// multi-word access traps); there is no MPU (the one XN rule that still faults is the default memory map's); there is no stack guard (an overflow runs
// into the free SRAM and .bss and faults nowhere: nothing to provoke); the watchdog is switched off and never re-enabled (nothing to provoke); the kit has
// no assert, only the platform's panic(), which the C++ runtime's pure-virtual handler (__cxa_pure_virtual) reaches.

#include <cstddef>
#include <cstdint>

namespace crashtest {

enum class Kind : std::uint8_t { Nmi, Hard, Mem, Bus, BusWr, Undef, BadFn, Unalign, Div0, Irq, Panic, Render };
inline constexpr std::size_t kKindCount = 12;

struct FaultInfo {
  const char* name;    // the console word: lower case letters
  const char* what;    // what the code does, at most 56 characters (the screen's column)
  const char* expect;  // what `crash` must show after the reset, one line (the numbers are the architecture's: the unit is the judge)
};

inline constexpr FaultInfo kFaults[kKindCount] = {
    {"nmi", "NMI pended through SCB ICSR", "NMI, cfsr 0, hfsr 0, pc just after the ICSR write"},
    {"hard", "BKPT with no debugger attached", "HardFault, hfsr 0x80000000 (DEBUGEVT), cfsr 0"},
    {"mem", "call to 0x40000001 (peripherals never execute)", "MemManage, cfsr 0x00000001 (IACCVIOL), pc 0x40000000, lr in the app"},
    {"bus", "read of the reserved eDMA word 0x40008008", "BusFault, cfsr 0x00008200 (PRECISERR, BFARVALID), bfar 0x40008008, pc at the load"},
    {"buswr", "write to 0x40008008, then DSB (imprecise)", "BusFault, cfsr 0x00000400 (IMPRECISERR), bfar not valid; no fault at all: the write is ignored"},
    {"undef", "UDF, an undefined instruction", "UsageFault, cfsr 0x00010000 (UNDEFINSTR), pc at the UDF"},
    {"badfn", "call through a pointer with the Thumb bit clear", "UsageFault, cfsr 0x00020000 (INVSTATE), pc = the even target"},
    {"unalign", "LDRD from an odd address (multi-word access)", "UsageFault, cfsr 0x01000000 (UNALIGNED), pc at the LDRD"},
    {"div0", "SDIV by zero (DIV_0_TRP is set at boot)", "UsageFault, cfsr 0x02000000 (DIVBYZERO), pc at the SDIV"},
    {"irq", "pends device interrupt 69, which has no handler", "IRQ 69 (no handler), exception 85, pc near the NVIC write"},
    {"panic", "the pure-virtual handler, which ends in panic()", "panic (0xFF), exc_return 0, frame unreadable, pc = lr = inside __cxa_pure_virtual"},
    {"render", "the bus read, inside a clocked hook (render ISR)", "as bus, but exc_return ...F1/E1 (handler mode) and xpsr low byte 0x50 (render line)"},
};

constexpr const FaultInfo& info(Kind k) noexcept { return kFaults[static_cast<std::size_t>(k)]; }

// Provokes `kind` here and now, in whatever context the caller is (the main loop; for Render, the hook calls it with Kind::Bus). Returns only when the
// core took no fault; the host build returns at once (there is nothing to fault).
void provoke(Kind kind) noexcept;

}  // namespace crashtest
