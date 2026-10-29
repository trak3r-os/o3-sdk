// SPDX-License-Identifier: GPL-3.0-or-later
//
// The faults of faults.h. Each is one or two instructions the compiler must not remove or move, so the ones that matter are inline assembly or a volatile access;
// each returns when the core took no fault (the caller reports that: it is a finding). Only the Cortex-M4 build provokes anything: on the host nothing can fault.
#include "faults.h"

#if defined(__arm__)

namespace crashtest {
namespace {

constexpr std::uintptr_t kReservedEdmaWord = 0x40008008u;  // eDMA 0x008 is reserved: a read of it faulted on the unit (2026-10-14, platform/mk66/usb mem_access.h)
constexpr std::uintptr_t kPeripheralAlias = 0x40000001u;   // the peripheral region is execute-never in the default memory map (the MPU is off); the Thumb bit is set
constexpr std::uintptr_t kScbIcsr = 0xE000ED04u;           // bit 31 NMIPENDSET
constexpr std::uintptr_t kNvicIser2 = 0xE000E108u;         // enable bits of interrupts 64..95
constexpr std::uintptr_t kNvicStir = 0xE000EF00u;          // software trigger: pends the interrupt whose number is written
constexpr unsigned kUnhandledIrq = 69;                     // Reserved85: in the device's table, nothing behind it, no handler of ours (irq_list.h)
static_assert(kUnhandledIrq >= 64 && kUnhandledIrq < 96, "interrupt 69 is enabled through ISER2");

volatile std::uint32_t& reg(std::uintptr_t address) noexcept { return *reinterpret_cast<volatile std::uint32_t*>(address); }

// Time for a fault that is taken a few cycles late (a write buffer, a pended interrupt) to arrive before the caller concludes there was none.
void settle() noexcept {
  asm volatile("dsb" ::: "memory");
  asm volatile("isb" ::: "memory");
  for (unsigned i = 0; i < 64; ++i) {
    asm volatile("nop");
  }
}

[[gnu::noinline]] void hard() noexcept { asm volatile("bkpt #0"); }

[[gnu::noinline]] void mem() noexcept {
  using Fn = void (*)() noexcept;
  Fn const target = reinterpret_cast<Fn>(kPeripheralAlias);
  target();
}

[[gnu::noinline]] void bus() noexcept {
  std::uint32_t value = reg(kReservedEdmaWord);
  asm volatile("" : "+r"(value));  // the value is used, so the load is not dropped even if the volatile read were
}

[[gnu::noinline]] void bus_write() noexcept {
  reg(kReservedEdmaWord) = 0u;
  settle();
}

[[gnu::noinline]] void undef() noexcept { asm volatile("udf #0"); }

[[gnu::noinline]] void nop_target() noexcept {}

[[gnu::noinline]] void bad_function() noexcept {
  using Fn = void (*)() noexcept;
  // The address of a real function with bit 0 cleared: BLX to it sets EPSR.T to 0, which the core refuses to execute from (INVSTATE).
  Fn const target = reinterpret_cast<Fn>(reinterpret_cast<std::uintptr_t>(&nop_target) & ~static_cast<std::uintptr_t>(1u));
  target();
}

// SRAM, doubleword aligned by declaration: the access below is at +1.
struct alignas(8) Scratch {
  volatile std::uint8_t bytes[16];
};
static_assert(alignof(Scratch) >= 4, "the scratch is word aligned, so +1 is not");
Scratch g_scratch;

[[gnu::noinline]] void unaligned() noexcept {
  std::uint32_t lo = 0;
  std::uint32_t hi = 0;
  const volatile std::uint8_t* const odd = g_scratch.bytes + 1;
  asm volatile("ldrd %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(odd) : "memory");
  asm volatile("" ::"r"(lo), "r"(hi));
}

[[gnu::noinline]] void divide_by_zero() noexcept {
  volatile std::uint32_t zero = 0;
  const std::uint32_t divisor = zero;
  std::uint32_t quotient = 0;
  asm volatile("sdiv %0, %1, %2" : "=r"(quotient) : "r"(1u), "r"(divisor));
  asm volatile("" ::"r"(quotient));
}

[[gnu::noinline]] void nmi() noexcept {
  reg(kScbIcsr) = 1u << 31;
  settle();
}

[[gnu::noinline]] void unhandled_irq() noexcept {
  reg(kNvicIser2) = 1u << (kUnhandledIrq - 64u);
  reg(kNvicStir) = kUnhandledIrq;
  settle();
}

// The C++ runtime's pure-virtual handler, which platform/mk66/src/startup.cpp defines as mk66::panic() (record, reset). A real pure virtual call ends here, but the compiler
// removes one it can see (at -O2 the call in a constructor became nothing, checked in the disassembly), so the handler is called by its name.
}  // namespace

extern "C" [[noreturn]] void __cxa_pure_virtual();

namespace {

[[gnu::noinline]] void panic() noexcept { __cxa_pure_virtual(); }

}  // namespace

void provoke(Kind kind) noexcept {
  switch (kind) {
    case Kind::Nmi: nmi(); break;
    case Kind::Hard: hard(); break;
    case Kind::Mem: mem(); break;
    case Kind::Bus: bus(); break;
    case Kind::BusWr: bus_write(); break;
    case Kind::Undef: undef(); break;
    case Kind::BadFn: bad_function(); break;
    case Kind::Unalign: unaligned(); break;
    case Kind::Div0: divide_by_zero(); break;
    case Kind::Irq: unhandled_irq(); break;
    case Kind::Panic: panic(); break;
    case Kind::Render: bus(); break;  // the app moves the call into the render context; here it is the same read
  }
}

}  // namespace crashtest

#else  // the host: the simulator and the tests run the app, and nothing here can fault

namespace crashtest {
void provoke(Kind) noexcept {}
}  // namespace crashtest

#endif
