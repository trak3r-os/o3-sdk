// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/console/midi_report.h>

namespace o3sdk::platform::mk66::console {
namespace {

using audio::diag::Text;

const char* yes(bool v) noexcept { return v ? "yes" : "no"; }
const char* on(bool v) noexcept { return v ? "on" : "off"; }

// Hundredths of a BPM as "120.00".
void bpm(Text& t, std::uint32_t x100) noexcept {
  t.dec(x100 / 100u).chr('.');
  const std::uint32_t frac = x100 % 100u;
  t.chr(static_cast<char>('0' + frac / 10u)).chr(static_cast<char>('0' + frac % 10u));
}

void hex8(Text& t, std::uint8_t v) noexcept {
  t.chr("0123456789ABCDEF"[(v >> 4) & 0xFu]).chr("0123456789ABCDEF"[v & 0xFu]);
}

void ports_text(Text& t, std::uint8_t ports) noexcept {
  const bool din = (ports & core::kMidiDin) != 0u;
  const bool usb = (ports & core::kMidiUsb) != 0u;
  t.str(din && usb ? "din+usb" : (din ? "din" : (usb ? "usb" : "none")));
}

}  // namespace

std::size_t format_midi_report(const MidiFigures& f, char* buf, std::size_t cap) noexcept {
  Text t(buf, cap);
  if (f.service == nullptr) {
    t.str("midi: not in this image\n");
    return t.size();
  }
  const midi::MidiService::Diag& d = *f.service;
  const hal::midi::Config& c = d.config;

  t.str("midi: following ").str(yes(d.status.following)).str(" locked ").str(yes(d.status.locked)).str(" tempo ");
  bpm(t, d.status.tempo_mbpm / 10u);
  t.str(" bpm (loop ");
  bpm(t, d.bpm_x100);
  t.str(" bpm, clocks ").dec(d.clocks).str(", error ").dec(d.error_us).str(" us)\n");

  t.str("midi: config channel ");
  if (c.channel == 0u) {
    t.str("omni");
  } else {
    t.dec(c.channel);
  }
  t.str(" out ").dec(c.out_channel).str(" ports ");
  ports_text(t, c.out_ports);
  t.str(" clock-out ").str(on(c.clock_out)).str(" transport-out ").str(on(c.transport_out)).str(" transport-in ").str(on(c.transport_in)).str(" follow ");
  t.str(!c.follow ? "off" : (c.follow_port == hal::midi::Port::Usb ? "usb" : "din")).nl();

  const midi::MidiService::Stats& s = d.service;
  t.str("midi: rx ").dec(d.status.rx).str(" tx ").dec(d.status.tx).str(" dropped ").dec(d.status.dropped).str(" messages ").dec(s.messages).str(" notes ").dec(s.notes);
  t.str(" held ").dec(s.held).str(" polled-dropped ").dec(s.poll_dropped).str(" sent ").dec(s.sent).str(" bad-out ").dec(s.bad_out).nl();
  t.str("midi: clock-out ").dec(s.clocks_out).str(" start-stop-out ").dec(s.starts_out).str(" transport-in ").dec(s.transport_events).str(" ignored ").dec(s.transport_ignored);
  t.str(" requests ").dec(s.requests).str(" attached ").dec(s.follow_attached).str(" released ").dec(s.stale_releases).nl();

  if (f.din_rx != nullptr && f.din_tx != nullptr) {
    const midi::DinRxStats& r = *f.din_rx;
    const midi::DinTxStats& x = *f.din_tx;
    t.str("midi: din rx isrs ").dec(r.isrs).str(" bytes ").dec(r.bytes).str(" errors ").dec(r.errors).str(" overruns ").dec(r.overruns).str(" full ").dec(r.ring_full);
    t.str(" multi ").dec(r.multi).str(" tx irqs ").dec(x.irqs).str(" bytes ").dec(x.bytes).str(" rt ").dec(x.rt_bytes);
    t.str(" late-max ").dec(static_cast<std::uint32_t>(x.rt_late_max / 180u)).str(" us no-time ").dec(x.no_time).str(" not-ready ").dec(x.not_ready).nl();
  }
  if (f.usb != nullptr) {
    const midi::UsbBindStats& u = *f.usb;
    t.str("midi: usb frames ").dec(u.frames).str(" events-out ").dec(u.events_out).str(" events-in ").dec(u.events_in).str(" bytes-in ").dec(u.bytes_in);
    t.str(" discarded ").dec(u.discarded).str(" no-time ").dec(u.no_time).str(" in-full ").dec(u.in_ring_full).nl();
  }
  t.str("midi: loop clocks ").dec(d.dll.clocks).str(" reacquires ").dec(d.dll.reacquires).str(" dropouts ").dec(d.dll.dropouts).str(" missing ").dec(d.dll.missing).nl();

  t.str("midi: last rx");
  const std::uint32_t rn = d.rx_log_n < midi::MidiService::kRxLog ? d.rx_log_n : midi::MidiService::kRxLog;
  for (std::uint32_t i = 0; i < rn; ++i) {
    t.chr(' ').chr(d.rx_log[i][0] == 0u ? 'D' : 'U').chr(':');
    hex8(t, d.rx_log[i][1]);
  }
  t.nl();
  t.str("midi: last tx");
  const std::uint32_t tn = d.tx_log_n < midi::MidiService::kTxLog ? d.tx_log_n : midi::MidiService::kTxLog;
  for (std::uint32_t i = 0; i < tn; ++i) {
    t.chr(' ').dec(d.tx_log[i][0]).chr(':');
    hex8(t, d.tx_log[i][1]);
    t.chr(':');
    hex8(t, d.tx_log[i][2]);
    t.chr(':');
    hex8(t, d.tx_log[i][3]);
  }
  t.nl();
  return t.size();
}

}  // namespace o3sdk::platform::mk66::console
