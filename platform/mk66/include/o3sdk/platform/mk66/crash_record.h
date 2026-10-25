// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The crash record: what a fault handler writes into RAM that survives the reset it then causes, and what the next boot
// reads back (ADR 0011 "persistent crash record", ADR 0002 "containment"). Everything here is a pure function over a
// plain struct, so it is tested on the host and on QEMU; the fault handlers (src/fault.cpp) only collect the registers
// and store the result.
//
// Where it lives (src/crash_store.cpp): a 128-byte `.noinit` block at the very bottom of SRAM_U (0x20000000), which
// neither the C runtime's .data copy nor its .bss clear touches, so it outlives SYSRESETREQ and a watchdog reset as RAM
// does on this chip. The reference manual does not promise it for every reset source, so the record carries a magic and
// a CRC-32: a record that is not both is "no record", whatever the RAM held at power-up. Surviving a watchdog and a
// lockup reset is an open hardware check (README.md); if SRAM does not, the same record moves into the VBAT register
// file with no change to this header.

#include <o3sdk/platform/mk66/mem_map.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::crash {

// "O3CR" read as bytes in memory (little endian): 'O' '3' 'C' 'R'.
inline constexpr std::uint32_t kMagic = 0x5243334Fu;
inline constexpr std::uint16_t kVersion = 1;

// Where the OS was when it died. `Stage` is the coarse boot/run position, `module` the loaded module being run (ADR 0002,
// 0 = none), both written ahead of time by the running code, so a fault handler only reads them.
enum class Stage : std::uint16_t {
  Reset = 0,      // first instructions after reset, before anything is configured
  Clocks = 1,     // crystal, HSRUN, PLL
  Memory = 2,     // .data / .bss / .ramfunc_l initialised
  Interrupts = 3, // NVIC priorities, fault handlers, SysTick
  Sdram = 4,      // SDRAM controller and its self-test
  Running = 5,    // main loop
  Unknown = 0xFFFF,
};

constexpr std::uint32_t make_context(std::uint16_t module, Stage stage) noexcept {
  return (static_cast<std::uint32_t>(stage) << 16) | module;
}
constexpr std::uint16_t module_of(std::uint32_t context) noexcept { return static_cast<std::uint16_t>(context & 0xFFFFu); }
constexpr Stage stage_of(std::uint32_t context) noexcept { return static_cast<Stage>(context >> 16); }
inline constexpr std::uint32_t kUnknownContext = 0xFFFFFFFFu;

// 16 words, all 32-bit, nothing implicit: no padding and no field that the compiler could place differently.
struct alignas(8) CrashRecord {
  std::uint32_t magic;       //  0: kMagic
  std::uint16_t version;     //  4: kVersion
  std::uint16_t size;        //  6: sizeof(CrashRecord), so a reader of a newer layout can refuse an older record
  std::uint32_t count;       //  8: crashes recorded since the record was last cleared (a boot counter for quarantine)
  std::uint32_t exception;   // 12: IPSR of the handler that ran: 2 NMI, 3 HardFault, 4 MemManage, 5 BusFault,
                             //     6 UsageFault, 16 + n an interrupt nobody installed a handler for, 0xFF software panic
  std::uint32_t exc_return;  // 16: EXC_RETURN in LR on entry: which stack, which mode, FPU frame or not
  std::uint32_t cfsr;        // 20: Configurable Fault Status Register (MMFSR, BFSR, UFSR)
  std::uint32_t hfsr;        // 24: HardFault Status Register
  std::uint32_t mmfar;       // 28: MemManage Fault Address Register (meaningful when CFSR.MMARVALID)
  std::uint32_t bfar;        // 32: BusFault Address Register (meaningful when CFSR.BFARVALID)
  std::uint32_t pc;          // 36: stacked PC: the instruction that faulted (or the next one, for an imprecise fault)
  std::uint32_t lr;          // 40: stacked LR
  std::uint32_t xpsr;        // 44: stacked xPSR
  std::uint32_t sp;          // 48: address of the exception frame; kInvalidFrame when it was not usable
  std::uint32_t context;     // 52: make_context(module, stage), kUnknownContext if it was not trustworthy
  std::uint32_t uptime_ms;   // 56: SysTick milliseconds since boot, low 32 bits
  std::uint32_t crc;         // 60: CRC-32 of the first 60 bytes
};
static_assert(sizeof(CrashRecord) == 64, "16 words");
static_assert(alignof(CrashRecord) == 8, "declared alignment (ADR 0011)");
static_assert(sizeof(CrashRecord) <= mem::kCrashBlockSize, "the record fits the noinit block");
static_assert(offsetof(CrashRecord, count) == 8 && offsetof(CrashRecord, cfsr) == 20 && offsetof(CrashRecord, pc) == 36 &&
                  offsetof(CrashRecord, context) == 52 && offsetof(CrashRecord, crc) == 60,
              "the layout is the documented one");

inline constexpr std::uint32_t kInvalidFrame = 0xFFFFFFFFu;
inline constexpr std::uint32_t kPanicException = 0xFFu;

// What the handler captured. Plain values: the registers it read and the frame it was able to read.
struct FaultSnapshot {
  std::uint32_t exception;
  std::uint32_t exc_return;
  std::uint32_t cfsr;
  std::uint32_t hfsr;
  std::uint32_t mmfar;
  std::uint32_t bfar;
  std::uint32_t pc;
  std::uint32_t lr;
  std::uint32_t xpsr;
  std::uint32_t sp;
  std::uint32_t context;
  std::uint32_t uptime_ms;
};

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF), bit by bit: 64 bytes
// cost about 500 loop steps, nothing to place in flash or RAM, safe in a fault handler. crc32("123456789") = 0xCBF43926.
std::uint32_t crc32(const void* data, std::size_t size) noexcept;

// True if `r` is a record this version wrote: magic, version, size and CRC all match.
bool valid(const CrashRecord& r) noexcept;

// Builds the record for a snapshot. `previous` is what the store held before: a valid record's count carries on, anything
// else starts at 1. The count saturates rather than wrapping to 0.
CrashRecord encode(const FaultSnapshot& snapshot, const CrashRecord& previous) noexcept;

// The snapshot a valid record was built from (for `diag crash` to print and for the tests to compare).
FaultSnapshot decode(const CrashRecord& r) noexcept;

// The exception frame the core pushes (ARMv7-M ARM B1.5.6), lowest address first: r0, r1, r2, r3, r12, lr, the return
// address (the PC of the faulting instruction, or the one after for an imprecise fault) and xPSR. (These indices are pinned by a test.) With the FPU in use the core pushes more
// words above these; the first eight are the same.
inline constexpr unsigned kFrameWords = 8;
inline constexpr unsigned kFrameLr = 5;
inline constexpr unsigned kFramePc = 6;
inline constexpr unsigned kFrameXpsr = 7;

// A usable exception frame is a word-aligned run of eight words wholly inside SRAM. A fault caused by a corrupt stack
// pointer must not make the handler fault again by reading through it.
constexpr bool frame_plausible(std::uint32_t address) noexcept {
  return (address & 3u) == 0u && mem::in_sram(address, kFrameWords * 4u);
}

// What a handler reads from the core's registers (not from the frame).
struct FaultRegisters {
  std::uint32_t exception;
  std::uint32_t exc_return;
  std::uint32_t cfsr;
  std::uint32_t hfsr;
  std::uint32_t mmfar;
  std::uint32_t bfar;
  std::uint32_t context;
  std::uint32_t uptime_ms;
};

// Joins the registers with the frame. `frame` is read only if `frame_address` (the numeric address `frame` points to) is
// plausible; otherwise the snapshot carries kInvalidFrame as its sp, `pc_if_no_frame` as pc and lr, and 0 as xPSR, and
// `frame` is never dereferenced.
FaultSnapshot snapshot(const FaultRegisters& registers, const std::uint32_t* frame, std::uint32_t frame_address,
                       std::uint32_t pc_if_no_frame) noexcept;

}  // namespace o3sdk::platform::mk66::crash
