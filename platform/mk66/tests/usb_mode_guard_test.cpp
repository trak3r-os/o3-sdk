// SPDX-License-Identifier: GPL-3.0-or-later
// The two safety nets under the USB console: the mode guard (the host does not configure the AUDIO set within 10 s: back to STANDARD) and
// the crash-loop rule (a crash record naming USB keeps USB off for the next boot).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/console/modules.h>
#include <o3sdk/platform/mk66/console/usb_mode_guard.h>
#include <o3sdk/platform/mk66/crash_record.h>

#include <cstdint>
#include <initializer_list>

namespace console = o3sdk::platform::mk66::console;
namespace crash = o3sdk::platform::mk66::crash;
using Guard = console::UsbModeGuard;
using Action = Guard::Action;
using Layout = Guard::Layout;

// tick(now, configured, bus_resets, changing): the driver's reset count since its last attach, and whether a change of set is on its way.
TEST_CASE("mode guard: disarmed until a set with audio is asked for") {
  Guard g;
  CHECK_FALSE(g.armed());
  CHECK(g.tick(0, false, 1, false) == Action::None);
  CHECK(g.tick(100'000, false, 1, false) == Action::None);
  g.layout_set(Layout::Standard);
  CHECK_FALSE(g.armed());
  CHECK(g.tick(500'000, false, 1, false) == Action::None);
  CHECK(g.fallbacks() == 0u);
}

TEST_CASE("mode guard: a host that resets the bus and never configures gets STANDARD 10 s after it showed up, once") {
  Guard g;
  g.layout_set(Layout::Audio);
  CHECK(g.armed());
  CHECK(g.tick(1000, false, 1, false) == Action::None);  // the first pass that sees the reset starts the clock
  CHECK(g.tick(10'999, false, 1, false) == Action::None);
  CHECK(g.armed());
  CHECK(g.tick(11'000, false, 1, false) == Action::RequestStandard);
  CHECK_FALSE(g.armed());
  CHECK(g.fallbacks() == 1u);
  CHECK(g.tick(11'001, false, 3, false) == Action::None);  // once
  CHECK(g.tick(60'000, false, 3, false) == Action::None);
  CHECK(g.fallbacks() == 1u);
}

TEST_CASE("mode guard: no host (no bus reset), no fallback: the unit waits, however long, and the clock starts when a host shows up") {
  Guard g;
  g.layout_set(Layout::Audio);
  for (std::uint32_t t = 0; t < 120'000; t += 100) {
    CHECK(g.tick(t, false, 0, false) == Action::None);
  }
  CHECK(g.armed());
  CHECK(g.fallbacks() == 0u);
  // A cable plugged in at 2 minutes: the host resets the bus and does not configure the set.
  CHECK(g.tick(120'000, false, 1, false) == Action::None);
  CHECK(g.tick(129'999, false, 1, false) == Action::None);
  CHECK(g.tick(130'000, false, 1, false) == Action::RequestStandard);
}

TEST_CASE("mode guard: a host that configures in time settles it for good") {
  Guard g;
  g.layout_set(Layout::Audio);
  CHECK(g.tick(10, false, 1, false) == Action::None);
  CHECK(g.tick(2000, false, 1, false) == Action::None);
  CHECK(g.tick(2500, true, 1, false) == Action::None);
  CHECK_FALSE(g.armed());
  CHECK(g.settled() == 1u);
  CHECK(g.tick(10'000, false, 1, false) == Action::None);  // later events are not its business
  CHECK(g.tick(30'000, false, 1, false) == Action::None);
  CHECK(g.fallbacks() == 0u);
}

TEST_CASE("mode guard: while the change is on its way nothing counts: the old set's counters and configuration are not the host's") {
  Guard g;
  // The change is asked for while the host still has the old set configured and its count in the driver; the device drains, detaches, waits and attaches.
  g.layout_set(Layout::Audio);
  for (std::uint32_t t = 50'000; t < 70'000; t += 100) {
    CHECK(g.tick(t, true, 4, true) == Action::None);  // changing: ignored, however long it takes, whatever the counters say
  }
  CHECK(g.armed());
  CHECK(g.settled() == 0u);
  CHECK(g.tick(70'000, false, 0, false) == Action::None);  // attached again: the driver zeroed its counters, no host yet
  CHECK(g.armed());
  CHECK(g.tick(70'100, false, 1, false) == Action::None);  // the host reset the bus
  CHECK(g.tick(71'500, true, 1, false) == Action::None);   // and configured the new set
  CHECK_FALSE(g.armed());
  CHECK(g.settled() == 1u);
  CHECK(g.fallbacks() == 0u);
}

TEST_CASE("mode guard: `usb mode standard` and then `usb mode audio` with a host that never configures: STANDARD again (the count of the first host is the count of the second)") {
  Guard g;
  // Boot in AUDIO: the host resets the bus once and configures.
  g.layout_set(Layout::Audio);
  CHECK(g.tick(1000, false, 1, false) == Action::None);
  CHECK(g.tick(1300, true, 1, false) == Action::None);
  CHECK_FALSE(g.armed());

  // `usb mode standard`: the guard is off. The driver detaches and attaches (its counters are zeroed), the host resets once and configures.
  g.layout_set(Layout::Standard);
  CHECK(g.tick(20'000, true, 1, true) == Action::None);
  CHECK(g.tick(21'000, false, 0, false) == Action::None);
  CHECK(g.tick(21'200, true, 1, false) == Action::None);
  CHECK_FALSE(g.armed());

  // `usb mode audio`: the old count is 1 and the new set's first reset makes it 1 again. A base taken at the request would hide the host; the guard has none.
  g.layout_set(Layout::Audio);
  CHECK(g.tick(40'000, true, 1, true) == Action::None);  // draining, detached: the old configuration and count
  CHECK(g.tick(40'900, false, 0, false) == Action::None);  // attached with the AUDIO descriptors: counters zeroed
  CHECK(g.tick(41'000, false, 1, false) == Action::None);  // the host resets the bus (1 again) and does not configure the set: the clock starts
  CHECK(g.tick(50'999, false, 1, false) == Action::None);
  CHECK(g.tick(51'000, false, 1, false) == Action::RequestStandard);
  CHECK(g.fallbacks() == 1u);
}

TEST_CASE("mode guard: configured on the very pass the deadline falls wins over the deadline") {
  Guard g;
  g.layout_set(Layout::Audio);
  CHECK(g.tick(9000, false, 1, false) == Action::None);
  CHECK(g.tick(19'000, true, 1, false) == Action::None);
  CHECK(g.fallbacks() == 0u);
  CHECK(g.settled() == 1u);
}

TEST_CASE("mode guard: a Standard request disarms it, a new Audio request starts it over") {
  Guard g;
  g.layout_set(Layout::Audio);
  CHECK(g.tick(5000, false, 1, false) == Action::None);
  g.layout_set(Layout::Standard);
  CHECK(g.tick(20'000, false, 1, false) == Action::None);
  CHECK(g.fallbacks() == 0u);

  g.layout_set(Layout::Audio);
  CHECK(g.tick(100'000, false, 1, false) == Action::None);
  CHECK(g.tick(108'000, false, 1, false) == Action::None);
  g.layout_set(Layout::Audio);  // asked again: the clock starts over at the next sight of the host
  CHECK(g.tick(110'000, false, 1, false) == Action::None);
  CHECK(g.tick(119'999, false, 1, false) == Action::None);
  CHECK(g.tick(120'000, false, 1, false) == Action::RequestStandard);
}

TEST_CASE("mode guard: the uptime counter may wrap") {
  Guard g;
  const std::uint32_t start = 0xFFFFFF00u;
  g.layout_set(Layout::Audio);
  CHECK(g.tick(start, false, 1, false) == Action::None);
  CHECK(g.tick(start + 9999u, false, 1, false) == Action::None);
  CHECK(g.tick(start + 10'000u, false, 1, false) == Action::RequestStandard);  // (the sum wraps)

  Guard h;
  h.layout_set(Layout::Audio);
  CHECK(h.tick(start, false, 1, false) == Action::None);
  CHECK(h.tick(start + 9000u, false, 1, false) == Action::None);
  CHECK(h.tick(start + 9500u, true, 1, false) == Action::None);
  CHECK(h.settled() == 1u);
}

TEST_CASE("mode guard: armed at boot with a host that does not take the set: STANDARD after 10 s, then quiet") {
  Guard g;
  g.layout_set(Layout::Audio);  // the image starts in its default set
  Action first = Action::None;
  std::uint32_t at = 0;
  for (std::uint32_t t = 1000; t <= 60'000; t += 100) {  // the host resets the bus at 1 s
    const Action a = g.tick(t, false, 1, false);
    if (a != Action::None && first == Action::None) {
      first = a;
      at = t;
    } else {
      CHECK(a == Action::None);
    }
  }
  CHECK(first == Action::RequestStandard);
  CHECK(at == 11'000u);
  CHECK(g.fallbacks() == 1u);
}

namespace {

// A valid record of a crash taken with the handler's IPSR `exception`, in the context of `module`, with `xpsr` stacked (the interrupted code's: 0x01000000 + its IPSR, the Thumb bit set).
crash::CrashRecord record_of(std::uint32_t exception, std::uint16_t module, std::uint32_t xpsr = 0x01000000u) {
  crash::FaultSnapshot s{};
  s.exception = exception;
  s.context = crash::make_context(module, crash::Stage::Running);
  s.pc = 0x1000;
  s.xpsr = xpsr;
  crash::CrashRecord none{};
  return crash::encode(s, none);
}

constexpr std::uint32_t kThumb = 0x01000000u;

}  // namespace

TEST_CASE("crash-loop rule: a record naming the USB module keeps USB off") {
  CHECK_FALSE(console::crash_blames_usb(nullptr));
  const crash::CrashRecord in_usb = record_of(3, console::kUsbModule);  // a HardFault while usb0::start() ran
  CHECK(console::crash_blames_usb(&in_usb));
  const crash::CrashRecord bus = record_of(5, console::kUsbModule);
  CHECK(console::crash_blames_usb(&bus));
}

TEST_CASE("crash-loop rule: a fault taken while the USB interrupt was running keeps USB off: the stacked IPSR is 69, the handler's is 3, 4 or 5, the module is 0") {
  for (const std::uint32_t exception : {3u, 4u, 5u, 6u}) {
    CAPTURE(exception);
    const crash::CrashRecord isr = record_of(exception, 0, kThumb | 69u);
    CHECK(console::crash_blames_usb(&isr));
  }
  // The other bits of the stacked xPSR (flags, the Thumb bit, the stack alignment bit, the IT state) do not matter, the exception number field does.
  const crash::CrashRecord flags = record_of(3, 0, 0x61000000u | 0x200u | 69u);
  CHECK(console::crash_blames_usb(&flags));
}

TEST_CASE("crash-loop rule: the handler's own IPSR 69 (the USB interrupt with no handler) keeps USB off") {
  const crash::CrashRecord irq = record_of(69, 0);
  CHECK(console::crash_blames_usb(&irq));
  const crash::CrashRecord both = record_of(69, console::kUsbModule, kThumb | 69u);
  CHECK(console::crash_blames_usb(&both));
}

TEST_CASE("crash-loop rule: any other crash leaves USB on") {
  const crash::CrashRecord audio = record_of(3, 4);
  CHECK_FALSE(console::crash_blames_usb(&audio));
  const crash::CrashRecord storage = record_of(5, console::kStorageModule);
  CHECK_FALSE(console::crash_blames_usb(&storage));
  const crash::CrashRecord loop = record_of(3, 0);  // thread mode (stacked IPSR 0)
  CHECK_FALSE(console::crash_blames_usb(&loop));
  // Module 0 with a stacked IPSR that is some other code: SysTick (15), the audio DMA (16 + 34... any line), the render line, the neighbours of 69.
  for (const std::uint32_t ipsr : {1u, 15u, 16u, 66u, 68u, 70u, 80u, 0x1FFu}) {
    CAPTURE(ipsr);
    const crash::CrashRecord other = record_of(3, 0, kThumb | ipsr);
    CHECK_FALSE(console::crash_blames_usb(&other));
  }
  const crash::CrashRecord next_irq = record_of(70, 0);
  CHECK_FALSE(console::crash_blames_usb(&next_irq));
  const crash::CrashRecord prev_irq = record_of(68, 0);
  CHECK_FALSE(console::crash_blames_usb(&prev_irq));
  const crash::CrashRecord panic = record_of(crash::kPanicException, 0);
  CHECK_FALSE(console::crash_blames_usb(&panic));

  crash::FaultSnapshot s{};
  s.exception = 3;
  s.context = crash::kUnknownContext;  // not trustworthy: module_of() is 0xFFFF
  s.xpsr = 0;                          // and no usable frame: the stacked xPSR is stored as 0
  const crash::CrashRecord unknown = crash::encode(s, crash::CrashRecord{});
  CHECK_FALSE(console::crash_blames_usb(&unknown));
}

TEST_CASE("crash-loop rule: a record without its magic or CRC is no record") {
  crash::CrashRecord bad = record_of(69, console::kUsbModule, kThumb | 69u);
  bad.crc ^= 1u;
  CHECK_FALSE(console::crash_blames_usb(&bad));
  crash::CrashRecord zero{};
  CHECK_FALSE(console::crash_blames_usb(&zero));
}

TEST_CASE("module ids: the new ones follow the image's 1..4") {
  CHECK(console::kStorageModule == 5);
  CHECK(console::kUsbModule == 6);
  CHECK(console::kUsbIrqException == 69u);
  CHECK(console::kIpsrMask == 0x1FFu);
}
