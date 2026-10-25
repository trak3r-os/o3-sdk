// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b review fix: the `diag` console's back-pressure, end to end. The real shell, the real console pump, the real `log` pager and the whole
// device side of the USB driver run against the fake controller and a host that pastes commands and reads replies at whatever speed the test says.
// Nothing may be lost: a pasted burst of `log 48` x3 returns every line of every reply, a host that stops reading for a long time loses nothing
// and is NAKed instead, and a flood of empty lines is answered one prompt each. Runs on the host and on QEMU (qemu_mk66).
#include <doctest/doctest.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <o3sdk/platform/mk66/usb/diag_text.h>
#include <o3sdk/platform/mk66/usb/log_command.h>
#include <o3sdk/usb/diag_log.h>
#include <o3sdk/usb/diag_pump.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace o3sdk::usb;
using namespace usb_rig;
namespace shell = o3sdk::usb::diag;
namespace console = o3sdk::platform::mk66::console;

namespace {

shell::LogRecord g_slots[256];
shell::LogRing g_log;
char g_capture[24 * 1024];

void cmd_log(shell::Shell& sh, const shell::Args& a) { console::run_log_command(sh, g_log, a); }
const shell::Command kTable[] = {shell::command("log", "log [count]", &cmd_log)};

// The main loop, the host and the interrupt, one millisecond per step.
struct Bench {
  explicit Bench(Rig& r)
      : rig(r),
        sh(kTable, 1, shell::Output(shell::Sink{&write, &flush, this, &room})),
        pump(sh, r.rx_ring) {}

  static void write(void* self, const char* d, std::size_t n) { static_cast<Bench*>(self)->rig.writer.write(d, n); }
  static void flush(void* self) { static_cast<Bench*>(self)->rig.writer.flush(); }
  static std::size_t room(void* self) { return static_cast<Bench*>(self)->rig.writer.room_bytes(); }

  Rig& rig;
  shell::Shell sh;
  shell::ConsolePump pump;
  const std::uint8_t* paste = nullptr;
  std::size_t paste_len = 0;
  std::size_t paste_off = 0;
  std::uint32_t naks = 0;
  std::size_t captured = 0;
  std::uint32_t max_rx_chunks = 0;

  // One millisecond: the paste (one packet if the device takes it), the main loop's pass, the start of frame, the host's reads.
  void step(unsigned reads) {
    if (paste_off < paste_len) {
      const std::size_t n = paste_len - paste_off < 64u ? paste_len - paste_off : 64u;
      if (rig.host.bulk_out(2, paste + paste_off, static_cast<std::uint32_t>(n)) == Reply::Ack) {
        paste_off += n;
      } else {
        ++naks;  // the device held the host off: the packet is retried on the next step, as a host does
      }
    }
    if (rig.rx_ring.size() > max_rx_chunks) {
      max_rx_chunks = rig.rx_ring.size();
    }
    pump.service();
    sh.tick();
    rig.writer.flush();
    rig.hw.sof();
    for (unsigned i = 0; i < reads; ++i) {
      std::uint8_t packet[64];
      std::uint32_t len = 0;
      if (rig.host.bulk_in(2, packet, sizeof packet, len) != Reply::Ack) {
        break;
      }
      for (std::uint32_t k = 0; k < len && captured + 1 < sizeof g_capture; ++k) {
        g_capture[captured++] = static_cast<char>(packet[k]);
      }
      g_capture[captured] = '\0';
    }
  }

  bool idle() const {
    return paste_off >= paste_len && pump.held_bytes() == 0u && rig.rx_ring.size() == 0u && rig.tx_ring.size() == 0u &&
           rig.writer.pending() == 0u && !sh.streaming();
  }
  // Runs until everything the paste asked for has been answered and read: `silent` steps with the host not reading at all, then `reads` per step.
  void run(unsigned silent, unsigned reads, unsigned max_steps) {
    unsigned quiet = 0;
    for (unsigned i = 0; i < max_steps && quiet < 8u; ++i) {
      step(i < silent ? 0u : reads);
      quiet = idle() && i >= silent ? quiet + 1u : 0u;
    }
  }
};

std::size_t count(const char* haystack, const char* needle) {
  std::size_t n = 0;
  const std::size_t len = std::strlen(needle);
  for (const char* p = std::strstr(haystack, needle); p != nullptr; p = std::strstr(p + len, needle)) {
    ++n;
  }
  return n;
}

// Fills the log with 200 lines of 45 characters: a `log 48` reply is then about 3 KB, more than the 1.6 KB the shell wants free to start a command.
void fill_log() {
  g_log.init(g_slots, 256);
  for (std::uint32_t i = 0; i < 200; ++i) {
    char text[56];
    std::snprintf(text, sizeof text, "line %04u abcdefghijklmnopqrstuvwxyz012345678", static_cast<unsigned>(i));
    g_log.write(i, text);
  }
}

// What one `log 48` reply is: the header and the last 48 lines (152..199), CR LF line ends.
const char* expected_reply() {
  static char reply[4096];
  std::size_t n = static_cast<std::size_t>(std::snprintf(reply, sizeof reply, "log: 200 lines written, showing 48\r\n"));
  for (unsigned i = 152; i < 200; ++i) {
    n += static_cast<std::size_t>(std::snprintf(reply + n, sizeof reply - n, "%u ms  line %04u abcdefghijklmnopqrstuvwxyz012345678\r\n", i, i));
  }
  return reply;
}

Bench& bench(Rig& rig) {
  alignas(Bench) static unsigned char storage[sizeof(Bench)];
  static Bench* current = nullptr;
  if (current != nullptr) {
    current->~Bench();
  }
  current = new (storage) Bench(rig);
  return *current;
}

}  // namespace

TEST_CASE("mk66 console: a reply of `log 48` is longer than the room the shell wants to start a command") {
  fill_log();
  const char* reply = expected_reply();
  CHECK(std::strlen(reply) > shell::kConsoleFeedRoomBytes);  // about 3 KB against 1.6 KB: one command alone can overrun what is free at its start
  CHECK(std::strlen(reply) < CdcTxRing::capacity() * kCdcTxChunkMax);
}

TEST_CASE("mk66 console: a pasted burst of log 48 x3 loses no reply bytes, whatever speed the host reads at") {
  const char* burst = "log 48\rlog 48\rlog 48\r";
  const unsigned reads_per_step[] = {1, 2, 5, 19};  // a full-speed bulk endpoint moves up to 19 packets per frame
  for (const unsigned reads : reads_per_step) {
    INFO("reads per millisecond: " << reads);
    fill_log();
    Rig& rig = fresh();
    USB_REQUIRE(rig.enumerate());
    g_capture[0] = '\0';
    Bench& b = bench(rig);
    b.paste = reinterpret_cast<const std::uint8_t*>(burst);
    b.paste_len = std::strlen(burst);
    b.run(0, reads, 4000);
    CHECK(b.paste_off == b.paste_len);
    CHECK(b.idle());
    // Each of the three replies arrived whole and in order: the header, the 48 lines, nothing missing, nothing cut.
    CHECK(count(g_capture, expected_reply()) == 3u);
    CHECK(count(g_capture, "log: 200 lines written, showing 48") == 3u);
    CHECK(count(g_capture, "line 0199 ") == 3u);
    CHECK(count(g_capture, "line 0152 ") == 3u);
    CHECK(rig.writer.dropped_chunks() == 0u);
    CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);
    CHECK(rig.host.toggle_errors() == 0u);
  }
}

TEST_CASE("mk66 console: a host that stops reading for a long time loses nothing") {
  const char* burst = "log 48\rlog 48\rlog 48\rlog 48\rlog 48\r";
  fill_log();
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  g_capture[0] = '\0';
  Bench& b = bench(rig);
  b.paste = reinterpret_cast<const std::uint8_t*>(burst);
  b.paste_len = std::strlen(burst);
  b.run(300, 3, 4000);  // 300 milliseconds with nobody reading: the transmit ring fills and the shell waits
  CHECK(b.paste_off == b.paste_len);
  CHECK(rig.writer.dropped_chunks() == 0u);
  CHECK(count(g_capture, expected_reply()) == 5u);
  CHECK(b.idle());
}

TEST_CASE("mk66 console: a flood of empty lines backs the host off with NAKs instead of overwriting anything") {
  fill_log();
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  g_capture[0] = '\0';
  Bench& b = bench(rig);
  static std::uint8_t flood[2000];
  std::memset(flood, '\r', sizeof flood);  // 2,000 empty lines (32 packets): each is answered with a prompt (6 bytes), 12 KB in all against a 3.9 KB ring
  b.paste = flood;
  b.paste_len = sizeof flood;
  b.run(200, 2, 6000);  // the host reads nothing for 200 ms
  CHECK(b.paste_off == b.paste_len);
  CHECK(b.naks > 0u);                                      // the device held the host off (the receive descriptors were not re-armed)
  CHECK(rig.dev.counters().cdc_rx_holds > 0u);
  CHECK(b.max_rx_chunks <= kCdcRxHoldChunks + 2u);          // the ring never held more than the hold plus the packets in flight
  CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);
  CHECK(rig.writer.dropped_chunks() == 0u);
  CHECK(count(g_capture, "o3> ") == 2000u);                // every empty line got its prompt, none lost
  CHECK(b.idle());
}

TEST_CASE("mk66 console: the pager keeps the terminal, a bad count is refused, a short log is whole") {
  fill_log();
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  g_capture[0] = '\0';
  Bench& b = bench(rig);
  const char* script = "log x\rlog 0\rlog 3\rlog 1000\r";
  b.paste = reinterpret_cast<const std::uint8_t*>(script);
  b.paste_len = std::strlen(script);
  b.run(0, 4, 2000);
  CHECK(count(g_capture, "usage: log [count]") == 2u);
  CHECK(count(g_capture, "log: 200 lines written, showing 3") == 1u);
  CHECK(count(g_capture, "log: 200 lines written, showing 200") == 1u);  // 1000 is under kLogMax (the whole ring): every line written is shown
  CHECK(count(g_capture, "line 0000 ") == 1u);                          // the first line, in the whole dump only (OS-DIAG1: the boot's early lines)
  CHECK(count(g_capture, "line 0197 ") == 2u);                          // in `log 3` and in the whole dump
  CHECK(b.idle());
  // An unlocked ring is refused politely.
  shell::LogRing off;
  char line[] = "log";
  shell::Args args;
  CHECK(shell::tokenize(line, args));
  b.sh.out().flush();
  const std::size_t before = std::strlen(g_capture);
  console::run_log_command(b.sh, off, args);
  b.rig.writer.flush();
  b.rig.hw.sof();
  std::uint8_t packet[64];
  std::uint32_t len = 0;
  CHECK(rig.host.bulk_in(2, packet, sizeof packet, len) == Reply::Ack);
  CHECK(std::strstr(reinterpret_cast<const char*>(packet), "log is not running") != nullptr);
  CHECK(before > 0u);
}
