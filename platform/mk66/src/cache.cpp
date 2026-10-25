// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LMEM code cache (OS-PERF3): enable, disable, invalidate. The facts and what the chip does and does not have (one cache, on the code bus; none on the system bus) are in lmem.h.
// Every command is bounded: the poll of GO gives up after a fixed count and says so. The interrupts are masked for the length of a command (a few microseconds at most), so that no
// handler fetches through the cache controller while it works; the audio DMA's margin is 11-22 us.
#include "internal.h"
#include "regs.h"

#include <o3sdk/platform/mk66/lmem.h>

namespace o3sdk::platform::mk66::cache {
namespace {

volatile std::uint32_t& pcccr() noexcept { return regs::at<std::uint32_t>(lmem::kPcccr); }
volatile std::uint32_t& pcclcr() noexcept { return regs::at<std::uint32_t>(lmem::kPcclcr); }
volatile std::uint32_t& pccsar() noexcept { return regs::at<std::uint32_t>(lmem::kPccsar); }
volatile std::uint32_t& pccrmr() noexcept { return regs::at<std::uint32_t>(lmem::kPccrmr); }

constexpr unsigned kPollLimit = 100000;  // reads of GO; a command is a few hundred cycles

struct IrqLock {
  IrqLock() noexcept {
    asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(saved_) : : "memory");
  }
  ~IrqLock() { asm volatile("msr primask, %0" : : "r"(saved_) : "memory"); }
  std::uint32_t saved_;
};

bool wait_go() noexcept {
  for (unsigned i = 0; i < kPollLimit; ++i) {
    if ((pcccr() & lmem::kGo) == 0u) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool enabled() noexcept { return (pcccr() & lmem::kEncache) != 0u; }
std::uint32_t control() noexcept { return pcccr(); }
std::uint32_t region_modes() noexcept { return pccrmr(); }

bool enable() noexcept {
  IrqLock lock;
  asm volatile("dsb" ::: "memory");
  pcccr() = lmem::kEnableAll;  // a single store: invalidate both ways, write buffer and cache on
  const bool done = wait_go();
  asm volatile("dsb\n\tisb" ::: "memory");
  return done;
}

bool disable() noexcept {
  IrqLock lock;
  asm volatile("dsb" ::: "memory");
  pcccr() = pcccr() & ~(lmem::kEncache | lmem::kEnwrbuf);
  asm volatile("dsb\n\tisb" ::: "memory");
  return true;
}

bool invalidate_all() noexcept {
  if (!enabled()) {
    return true;  // nothing is cached; enable() invalidates before it turns the cache on
  }
  IrqLock lock;
  asm volatile("dsb" ::: "memory");
  pcccr() = pcccr() | lmem::kInvalidateAll;
  const bool done = wait_go();
  asm volatile("dsb\n\tisb" ::: "memory");
  return done;
}

bool invalidate_range(std::uint32_t address, std::uint32_t bytes) noexcept {
  if (!enabled() || bytes == 0u || !lmem::cacheable_address(address)) {
    return true;
  }
  IrqLock lock;
  asm volatile("dsb" ::: "memory");
  bool ok = true;
  const std::uint32_t last = lmem::line_base(address + bytes - 1u);
  for (std::uint32_t line = lmem::line_base(address); ok; line += lmem::kLineBytes) {
    pcclcr() = lmem::kLineCmdInvalidate;
    pccsar() = line | lmem::kLineGo;  // the address (bits 31:2) and LGO in one store: the command starts
    unsigned polls = 0;
    while ((pccsar() & lmem::kLineGo) != 0u && polls < kPollLimit) {
      ++polls;
    }
    ok = polls < kPollLimit;
    if (line >= last) {
      break;
    }
  }
  asm volatile("dsb\n\tisb" ::: "memory");
  return ok;
}

}  // namespace o3sdk::platform::mk66::cache
