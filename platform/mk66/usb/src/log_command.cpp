// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/platform/mk66/usb/log_command.h>

namespace o3sdk::platform::mk66::console {
namespace {

namespace shell = o3sdk::usb::diag;

// A formatted line is at most 10 digits, " ms  ", 55 characters and a newline (CR LF on the wire): 72 bytes. The pager writes a line only
// while the output has room for twice that.
constexpr std::size_t kLineRoom = 144;
constexpr unsigned kLinesPerTick = 16;  // bounds one pass of the main loop

struct Pager {
  const shell::LogRing* ring = nullptr;
  std::uint32_t next = 0;
  std::uint32_t end = 0;
};
Pager g_pager;  // the shell has one stream, so there is one pager

bool pager_step(shell::Shell& sh, void*) {
  char buf[96];
  for (unsigned lines = 0; lines < kLinesPerTick && g_pager.next < g_pager.end && sh.out().room() >= kLineRoom; ++lines) {
    shell::LogEntry e;
    if (g_pager.ring->read(g_pager.next, e)) {  // a record that was overwritten meanwhile is skipped
      const std::size_t n = format_log_entry(e, buf, sizeof buf);
      sh.out().text(buf, n);
    }
    ++g_pager.next;
  }
  return g_pager.next < g_pager.end;
}

// Decimal argument, 0 if it is not one.
std::uint32_t parse_u32(const char* s) {
  std::uint32_t v = 0;
  for (; *s >= '0' && *s <= '9'; ++s) {
    v = v * 10u + static_cast<std::uint32_t>(*s - '0');
    if (v > 1'000'000u) {
      break;
    }
  }
  return v;
}

}  // namespace

void run_log_command(shell::Shell& sh, const shell::LogRing& ring, const shell::Args& a) noexcept {
  if (!ring.ready()) {
    sh.out().text("log is not running\n");
    return;
  }
  std::uint32_t want = kLogDefault;
  if (a.argc > 1) {
    want = parse_u32(a.argv[1]);
    if (want == 0u) {
      sh.out().text("usage: log [count]  (the last count lines, 1.. ").dec(kLogMax).text(")\n");
      return;
    }
    want = want > kLogMax ? kLogMax : want;
  }
  const std::uint32_t written = ring.written();
  std::uint32_t first = written > want ? written - want : 0u;
  if (first < ring.oldest()) {
    first = ring.oldest();
  }
  sh.out().text("log: ").dec(written).text(" lines written, showing ").dec(written - first).nl();
  g_pager.ring = &ring;
  g_pager.next = first;
  g_pager.end = written;
  sh.start_stream(&pager_step, nullptr, false);
}

}  // namespace o3sdk::platform::mk66::console
