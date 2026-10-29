// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `midi` report (console/midi_report.h): the words o3console and a hardware session read. Built from a MidiService that has run a few blocks, so the figures are the service's own.
#include <doctest/doctest.h>

#include <o3sdk/core/midi_types.h>
#include <o3sdk/platform/mk66/console/midi_report.h>
#include <o3sdk/platform/mk66/midi/midi_service.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace console = o3sdk::platform::mk66::console;
namespace midi = o3sdk::platform::mk66::midi;
namespace core = o3sdk::core;

namespace {

std::string report(const console::MidiFigures& f, std::size_t cap = 2048) {
  char buf[2048];
  const std::size_t n = console::format_midi_report(f, buf, cap < sizeof buf ? cap : sizeof buf);
  REQUIRE(n == std::strlen(buf));
  return std::string(buf, n);
}

bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

}  // namespace

TEST_CASE("midi report: without a service it says so") {
  console::MidiFigures f;
  CHECK(report(f) == "midi: not in this image\n");
}

TEST_CASE("midi report: a quiet service, the defaults, and every line starts with midi:") {
  midi::MidiService svc;
  core::MidiMessage in[8];
  core::ClockFrame clock;
  (void)svc.begin_block(64, in, 8, clock);
  svc.end_block(core::ClockFrame{}, nullptr, 0);
  midi::MidiService::Diag diag;
  svc.diag(diag);
  midi::DinRxStats rx;
  midi::DinTxStats tx;
  midi::UsbBindStats usb;
  console::MidiFigures f{&diag, &rx, &tx, &usb};
  const std::string text = report(f);
  CHECK(has(text, "midi: following no locked no tempo 0.00 bpm (loop 0.00 bpm, clocks 0, error 0 us)\n"));
  CHECK(has(text, "midi: config channel omni out 1 ports din+usb clock-out on transport-out on transport-in on follow off\n"));
  CHECK(has(text, "midi: rx 0 tx 0 dropped 0 messages 0 notes 0 held 0 polled-dropped 0 sent 0 bad-out 0\n"));
  CHECK(has(text, "midi: din rx isrs 0 bytes 0 errors 0 overruns 0 full 0 multi 0 tx irqs 0 bytes 0 rt 0 late-max 0 us no-time 0 not-ready 0\n"));
  CHECK(has(text, "midi: usb frames 0 events-out 0 events-in 0 bytes-in 0 discarded 0 no-time 0 in-full 0\n"));
  CHECK(has(text, "midi: loop clocks 0 reacquires 0 dropouts 0 missing 0\n"));
  CHECK(has(text, "midi: last rx\n"));
  CHECK(has(text, "midi: last tx\n"));
  // Every line is a `midi:` line.
  std::size_t at = 0;
  unsigned lines = 0;
  while (at < text.size()) {
    CHECK(text.compare(at, 6, "midi: ") == 0);
    const std::size_t nl = text.find('\n', at);
    REQUIRE(nl != std::string::npos);
    at = nl + 1u;
    ++lines;
  }
  CHECK(lines == 9u);
}

TEST_CASE("midi report: the configuration, the counters and the last bytes are the service's") {
  midi::MidiService svc;
  o3sdk::hal::midi::Config c;
  c.channel = 5;
  c.out_channel = 3;
  c.out_ports = core::kMidiUsb;
  c.clock_out = false;
  c.follow = true;
  c.follow_port = o3sdk::hal::midi::Port::Usb;
  svc.set_config(c);
  // A note on channel 6 from DIN (not omni: channel 5 is set, but the service hands every note to the engine and the route filters) and one on the USB port.
  const std::uint8_t bytes[] = {0x95, 0x3C, 0x64};
  for (const std::uint8_t b : bytes) {
    REQUIRE(svc.din_in().push(midi::RtItem{1000u, b, 0, 0}));
  }
  core::MidiMessage in[8];
  core::ClockFrame clock;
  (void)svc.begin_block(64, in, 8, clock);  // (the first block discards what was stamped before it)
  for (const std::uint8_t b : bytes) {
    REQUIRE(svc.din_in().push(midi::RtItem{1000u, b, 0, 0}));
  }
  (void)svc.begin_block(64, in, 8, clock);
  svc.end_block(core::ClockFrame{}, nullptr, 0);
  REQUIRE(svc.send(core::midi_note_on(3, 61, 90, 0, core::kMidiUsb)));
  (void)svc.begin_block(64, in, 8, clock);
  svc.end_block(core::ClockFrame{}, nullptr, 0);
  midi::MidiService::Diag diag;
  svc.diag(diag);
  console::MidiFigures f{&diag, nullptr, nullptr, nullptr};
  const std::string text = report(f);
  CHECK(has(text, "midi: config channel 5 out 3 ports usb clock-out off transport-out on transport-in on follow usb\n"));
  CHECK(has(text, "midi: rx 1 "));
  CHECK(has(text, " messages 1 notes 1 "));
  CHECK(has(text, "midi: last rx D:95 D:3C D:64\n"));
  CHECK(has(text, "midi: last tx 2:92:3D:5A\n"));
  CHECK_FALSE(has(text, "midi: din "));  // no driver counters given
  CHECK_FALSE(has(text, "midi: usb "));
}

TEST_CASE("midi report: a buffer too small gets a cut report, zero-terminated, never past the end") {
  midi::MidiService svc;
  midi::MidiService::Diag diag;
  svc.diag(diag);
  console::MidiFigures f{&diag, nullptr, nullptr, nullptr};
  for (const std::size_t cap : {std::size_t{1}, std::size_t{2}, std::size_t{17}, std::size_t{100}}) {
    char buf[128];
    std::memset(buf, 'x', sizeof buf);
    const std::size_t n = console::format_midi_report(f, buf, cap);
    CHECK(n < cap);
    CHECK(buf[n] == '\0');
    CHECK(buf[cap] == 'x');  // nothing past the capacity
  }
}

TEST_CASE("midi report: the worst case (every counter at its maximum, every mode string at its longest, the last bytes full) fits the console's buffer whole") {
  midi::MidiService::Diag diag;
  diag.status.following = true;
  diag.status.locked = true;
  diag.status.tempo_mbpm = 0xFFFFFFFFu;
  diag.status.rx = diag.status.tx = diag.status.dropped = 0xFFFFFFFFu;
  diag.config.channel = 16;
  diag.config.out_channel = 16;
  diag.config.out_ports = core::kMidiAllPorts;  // "din+usb", the longest
  diag.config.follow = true;
  diag.config.follow_port = o3sdk::hal::midi::Port::Usb;
  diag.dll_phase = true;
  diag.bpm_x100 = 0xFFFFFFFFu;
  diag.clocks = 0xFFFFFFFFu;
  diag.error_us = 0xFFFFFFFFu;
  std::uint32_t* words = reinterpret_cast<std::uint32_t*>(&diag.service);
  static_assert(sizeof(diag.service) % sizeof(std::uint32_t) == 0, "Stats is a run of 32-bit counters");
  for (std::size_t i = 0; i < sizeof(diag.service) / sizeof(std::uint32_t); ++i) {
    words[i] = 0xFFFFFFFFu;
  }
  diag.dll.clocks = diag.dll.ignored = diag.dll.dropouts = diag.dll.missing = diag.dll.reacquires = 0xFFFFFFFFu;
  diag.rx_log_n = 0xFFFFFFFFu;
  diag.tx_log_n = 0xFFFFFFFFu;
  for (auto& e : diag.rx_log) {
    e[0] = 1;
    e[1] = 0xFF;
  }
  for (auto& e : diag.tx_log) {
    e[0] = 3;
    e[1] = e[2] = e[3] = 0xFF;
  }
  midi::DinRxStats rx;
  rx.isrs = rx.bytes = rx.errors = rx.overruns = rx.ring_full = rx.multi = 0xFFFFFFFFu;
  midi::DinTxStats tx;
  tx.irqs = tx.bytes = tx.rt_bytes = tx.no_time = tx.not_ready = tx.timer_floor = tx.timer_ceiling = 0xFFFFFFFFu;
  tx.rt_late_max = ~std::uint64_t{0};
  midi::UsbBindStats usb;
  usb.frames = usb.events_out = usb.discarded = usb.no_time = usb.events_in = usb.bytes_in = usb.in_ring_full = 0xFFFFFFFFu;
  console::MidiFigures f{&diag, &rx, &tx, &usb};
  char buf[console::kMidiReportRoom];
  const std::size_t n = console::format_midi_report(f, buf, sizeof buf);
  INFO(n);
  CHECK(n < sizeof buf - 1u);  // not cut: the Text helper stops at the capacity
  CHECK(n > 900u);             // and it really is the long one
  CHECK(buf[n - 1] == '\n');
  CHECK(std::string(buf, n).find("midi: last tx 3:FF:FF:FF") != std::string::npos);
  unsigned lines = 0;
  for (std::size_t i = 0; i < n; ++i) {
    lines += buf[i] == '\n' ? 1u : 0u;
  }
  CHECK(lines == 9u);
}
