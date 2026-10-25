// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the USB interrupt's logic (usb/device.h) against the fake controller: a whole enumeration with the descriptors a host
// would read, the CDC byte streams in both directions with their drop policies, USB-MIDI event packets, bus reset, halt and
// clear-halt with the toggle and ping-pong consequences, suspend and resume, error flags, the bound on one interrupt's work. Runs
// on the host and on QEMU (qemu_usb_device).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/device.h>
#include <o3sdk/usb/validate.h>

#include "usb_rig.h"

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb;
using namespace usb_rig;

namespace {

// Everything the device has armed on a bulk IN endpoint, as a host collects it with IN tokens until NAK.
struct Collected {
  std::uint8_t bytes[1024];
  std::uint32_t length = 0;
  std::uint32_t packets = 0;
};

Collected drain(Rig& rig, unsigned ep) {
  Collected c;
  for (unsigned guard = 0; guard < 256u; ++guard) {
    std::uint32_t len = 0;
    if (rig.host.bulk_in(ep, c.bytes + c.length, static_cast<std::uint32_t>(sizeof c.bytes) - c.length, len) != Reply::Ack) {
      break;
    }
    c.length += len;
    ++c.packets;
  }
  return c;
}

void write_line(Rig& rig, const char* s) {
  rig.writer.write(s, std::strlen(s));
  rig.writer.flush();
}

struct Recorder {
  UsbEvent events[32];
  std::uint32_t args[32];
  unsigned count = 0;
  static void hook(void* self, UsbEvent e, std::uint32_t arg) {
    Recorder* r = static_cast<Recorder*>(self);
    if (r->count < 32u) {
      r->events[r->count] = e;
      r->args[r->count] = arg;
      ++r->count;
    }
  }
};

}  // namespace

TEST_CASE("usb device: a full enumeration, descriptors validated as the host read them") {
  Rig& rig = fresh();
  Recorder rec;
  rig.dev.set_event_hook(&Recorder::hook, &rec);
  CHECK(rig.hw.inten() == sie::kIntenWorkingSet);  // prepare() armed the controller before the pull-up
  CHECK((rig.hw.endpt(0) == kEndpt0 && rig.hw.endpt(2) == 0));
  CHECK((bd::owned(rig.bdt.entry[bdt_index(0, false, false)].desc) && bd::owned(rig.bdt.entry[bdt_index(0, false, true)].desc)));

  rig.hw.bus_reset();
  std::uint8_t buf[512];
  Host::Result r = rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf);
  USB_REQUIRE(r.ok);
  CHECK(validate_device_descriptor(buf, 18, rig.cfg).ok());
  CHECK(rig.hw.address() == 0u);
  USB_REQUIRE(rig.set_address(7).ok);
  CHECK(rig.hw.address() == 7u);  // after the status stage
  r = rig.get_descriptor(2, 0, 0, 255, buf, sizeof buf);
  USB_REQUIRE(r.ok);
  CHECK(r.length == rig.cfg_len);
  CHECK(r.packets == 3u);
  CHECK(r.toggle_errors == 0u);
  CHECK(std::memcmp(buf, rig.cfg_desc, rig.cfg_len) == 0);
  CHECK(validate_config_descriptor(buf, r.length, rig.cfg).ok());
  for (std::uint8_t i = 0; i < string_count(rig.cfg); ++i) {  // every string the descriptors name can be read
    r = rig.get_descriptor(3, i, 0x0409, 255, buf, sizeof buf);
    CHECK(r.ok);
    CHECK((r.length >= 2u && buf[0] == r.length && buf[1] == 3));
  }
  USB_REQUIRE(rig.set_configuration(1).ok);
  CHECK(rig.pipe.state() == DeviceState::Configured);
  CHECK(rig.hw.endpt(1) == 0x15);
  CHECK(rig.hw.endpt(2) == 0x1D);
  CHECK(rig.hw.endpt(3) == 0x1D);
  CHECK(rig.hw.endpt(4) == 0);
  // The bulk OUT endpoints are armed, both descriptors, DATA0 then DATA1.
  const std::uint32_t o0 = rig.bdt.entry[bdt_index(2, false, false)].desc;
  const std::uint32_t o1 = rig.bdt.entry[bdt_index(2, false, true)].desc;
  CHECK((o0 == bd::arm(64, false) && o1 == bd::arm(64, true)));
  CHECK(rig.bdt.entry[bdt_index(3, false, false)].desc == bd::arm(64, false));
  CHECK_FALSE(bd::owned(rig.bdt.entry[bdt_index(2, true, false)].desc));  // nothing to send yet
  CHECK(rig.host.toggle_errors() == 0u);
  CHECK(rig.hw.toggle_mismatches() == 0u);
  // The events the log would show.
  USB_REQUIRE(rec.count == 3u);
  CHECK((rec.events[0] == UsbEvent::BusReset && rec.events[1] == UsbEvent::Addressed && rec.args[1] == 7u));
  CHECK((rec.events[2] == UsbEvent::Configured));
}

TEST_CASE("usb device: enumeration the way a host with a 64-byte EP0 does it, repeated, never wedges") {
  Rig& rig = fresh();
  for (int round = 0; round < 3; ++round) {  // hosts reset and enumerate again and again (a port that is reset, a driver reload)
    USB_REQUIRE(rig.enumerate(static_cast<std::uint8_t>(5 + round)));
    CHECK(rig.hw.address() == 5u + static_cast<unsigned>(round));
    CHECK(rig.pipe.configured());
  }
  CHECK(rig.host.toggle_errors() == 0u);
  CHECK(rig.dev.counters().bus_resets == 3u);  // the host's three; prepare() is not counted
  CHECK(rig.dev.counters().tokens_stray == 0u);
  CHECK(rig.dev.counters().pingpong_resyncs == 0u);
}

TEST_CASE("usb device: AUDIO descriptors enumerate through the same engine, the isochronous endpoints are enabled but not driven") {
  Config cfg;
  cfg.mode = Mode::Audio;
  Rig& rig = fresh(cfg);
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.cfg_len == 384u);  // six packets exactly: the host reads them with wLength 384, no zero-length packet
  CHECK((rig.hw.endpt(2) == 0x1D && rig.hw.endpt(3) == 0x1D));  // CDC and MIDI as in STANDARD
  CHECK(rig.hw.endpt(4) == 0x1C);                               // isochronous both ways: no handshake
  CHECK(rig.hw.endpt(5) == 0x14);                               // the feedback endpoint
  CHECK_FALSE(bd::owned(rig.bdt.entry[bdt_index(4, true, false)].desc));  // nothing armed on them: that is a later slice
  // The AudioStreaming alternate settings are tracked (and counted) by the pipe, not acted on.
  std::uint8_t raw[8];
  make_setup(raw, 0x01, 0x0B, 1, layout::kIfAudioIn, 0);
  CHECK(rig.host.control_none(raw).ok);
  CHECK(rig.pipe.audio().in_active());
  CHECK(rig.dev.counters().unsupported_interface_sets == 1u);
  // CDC still works in this mode.
  write_line(rig, "diag\n");
  rig.hw.sof();
  CHECK(drain(rig, 2).length == 5u);
  CHECK(rig.host.toggle_errors() == 0u);
}

TEST_CASE("usb device: an unsupported request stalls endpoint 0 and the next request is served") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  std::uint8_t raw[8];
  make_setup(raw, 0x40, 0x01, 0, 0, 0);  // vendor request, no data
  Host::Result r = rig.host.control_none(raw);
  CHECK(r.stalled);
  CHECK(rig.dev.counters().ep0_stalls == 1u);
  CHECK(rig.pipe.counters().stalls == 1u);
  CHECK(rig.hw.endpt(0) == kEndpt0);  // the STALL interrupt un-stalled it (the one-shot): the lasting part is the transmit descriptors below
  CHECK(rig.bdt.entry[bdt_index(0, true, false)].desc == (bd::kOwn | bd::kStall));
  CHECK(rig.bdt.entry[bdt_index(0, true, true)].desc == (bd::kOwn | bd::kStall));
  std::uint8_t buf[64];
  CHECK(rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf).ok);
  // A stall nobody consumed (the host sends a SETUP at once) is cleared by that SETUP.
  CHECK(rig.hw.setup(raw) == Reply::Ack);  // the vendor request again: stalls, but the host does not follow up with a token
  CHECK((rig.hw.endpt(0) & kEndptStall) != 0u);
  std::uint8_t good[8];
  make_setup(good, 0x80, 0x06, 0x0100, 0, 18);
  CHECK(rig.host.control_in(good, buf, sizeof buf).ok);
  // A GET_DESCRIPTOR for something we do not have, mid-enumeration, stalls the data stage and the status never comes.
  make_setup(raw, 0x80, 0x06, 0x0600, 0, 10);  // device qualifier
  CHECK(rig.host.control_in(raw, buf, sizeof buf).stalled);
  CHECK(rig.get_descriptor(2, 0, 0, 9, buf, sizeof buf).ok);
}

namespace {

// How many entries of the trace carry the tag.
unsigned trace_count(const Ep0Trace& trace, Ep0Trace::Tag tag) {
  unsigned n = 0;
  for (unsigned i = 0; i < trace.count(); ++i) {
    n += trace.entry(i).tag == static_cast<std::uint8_t>(tag) ? 1u : 0u;
  }
  return n;
}

}  // namespace

TEST_CASE("usb device: the endpoint 0 protocol stall lasts until the next SETUP: a retried IN is stalled again, never NAKed, counted, and the SETUP buffers stay armed") {
  // OS-USBDISK int.14 usbmon: a device-qualifier IN that got no completion (the host's retried IN was NAKed, EPSTALL being gone, and it retried until its 5 s timeout). USB 2.0 9.2.7: a control pipe in the
  // stall condition answers STALL to every data and status token until the next SETUP. The one-shot EPSTALL is as it was; the transmit descriptors (OWN|STALL) carry the rest.
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  std::uint8_t qualifier[8];
  make_setup(qualifier, 0x80, 0x06, 0x0600, 0, 10);
  CHECK(rig.hw.setup(qualifier) == Reply::Ack);
  CHECK(rig.hw.endpt(0) == (kEndpt0 | kEndptStall));  // the one-shot is up until the host's first token
  std::uint8_t data[64];
  std::uint32_t len = 0;
  bool data1 = false;
  for (unsigned token = 0; token < 4u; ++token) {  // the data stage's IN, then three retries
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    CHECK(rig.hw.endpt(0) == kEndpt0);  // the first handshake cleared EPSTALL
    CHECK(bd::owned(rig.bdt.entry[bdt_index(0, true, false)].desc));  // not consumed
    CHECK(bd::owned(rig.bdt.entry[bdt_index(0, true, true)].desc));
  }
  CHECK(rig.dev.counters().ep0_stalls == 1u);  // one stall of the control pipe, four handshakes
  CHECK(bd::owned(rig.bdt.entry[bdt_index(0, false, false)].desc));  // the receive descriptors are armed for the SETUP: nothing consumed them
  CHECK(bd::owned(rig.bdt.entry[bdt_index(0, false, true)].desc));
  CHECK(trace.stall_retries() == 3u);
  CHECK(trace_count(trace, Ep0Trace::Tag::StallSeen) == 1u);  // the first handshake only is traced
  CHECK(trace_count(trace, Ep0Trace::Tag::SetupArmed) == 1u);
  CHECK(trace_count(trace, Ep0Trace::Tag::StallCleared) == 0u);
  // As before this change, an OUT after the one-shot is acknowledged and dropped (the control pipe is idle): the receive side is not stalled, the SETUP descriptors must stay takeable.
  CHECK(rig.hw.out(0, nullptr, 0, true) == Reply::Ack);
  CHECK(rig.dev.counters().tokens_stray == 0u);
  // The next SETUP is taken, ends the stall, and is served.
  std::uint8_t good[8];
  make_setup(good, 0x80, 0x06, 0x0100, 0, 18);
  CHECK(rig.host.control_in(good, data, sizeof data).ok);
  CHECK(rig.hw.endpt(0) == kEndpt0);
  USB_REQUIRE(trace_count(trace, Ep0Trace::Tag::StallCleared) == 1u);
  for (unsigned i = 1; i < trace.count(); ++i) {  // the event follows the SETUP that ended the stall
    if (trace.entry(i).tag == static_cast<std::uint8_t>(Ep0Trace::Tag::StallCleared)) {
      CHECK(trace.entry(i - 1u).tag == static_cast<std::uint8_t>(Ep0Trace::Tag::Setup));
      CHECK(trace.entry(i - 1u).w0 == 0x01000680u);
    }
  }
  CHECK(rig.dev.counters().tokens_stray == 0u);
  CHECK(trace.stall_retries() == 3u);  // the good transfer added none
  // And a SETUP when nothing is stalled records no clear.
  trace.arm();
  CHECK(rig.host.control_in(good, data, sizeof data).ok);
  CHECK(trace_count(trace, Ep0Trace::Tag::Setup) == 1u);
  CHECK(trace_count(trace, Ep0Trace::Tag::StallCleared) == 0u);
  CHECK(trace.stall_retries() == 0u);
}

TEST_CASE("usb device: a bus reset also ends the endpoint 0 stall, and the enumeration that follows is served") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  std::uint8_t vendor[8];
  make_setup(vendor, 0x40, 0x01, 0, 0, 0);
  CHECK(rig.hw.setup(vendor) == Reply::Ack);
  std::uint8_t data[64];
  std::uint32_t len = 0;
  bool data1 = false;
  CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);  // the stall handshake: the one-shot is gone, the transmit descriptors keep the stall
  CHECK(rig.hw.endpt(0) == kEndpt0);
  CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);  // a retry
  CHECK(trace.stall_retries() == 1u);
  rig.hw.bus_reset();
  CHECK(rig.hw.endpt(0) == kEndpt0);
  CHECK(rig.bdt.entry[bdt_index(0, true, false)].desc == 0u);  // the reset took the stalled descriptors back
  CHECK(rig.bdt.entry[bdt_index(0, true, true)].desc == 0u);
  CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Nak);  // not stalled any more: an idle control pipe NAKs
  rig.hw.raise_stall_flag();                                           // and a stray STALL flag is nobody's retry
  CHECK(trace.stall_retries() == 1u);
  CHECK(trace_count(trace, Ep0Trace::Tag::StallCleared) == 0u);  // the Reset event says it
  USB_REQUIRE(rig.enumerate());
}

namespace {

// The way a Linux host enumerates: the device descriptor in two reads, SET_ADDRESS, then the device-qualifier read three times (stalled each time, and the data-stage IN tried once more after the
// STALL, which a lost handshake or a retrying hub makes real), the 9-byte header and the whole configuration, SET_CONFIGURATION.
void enumerate_like_linux(Rig& rig, std::uint8_t address) {
  rig.hw.bus_reset();
  std::uint8_t buf[512];
  USB_REQUIRE(rig.get_descriptor(1, 0, 0, 8, buf, sizeof buf).ok);
  USB_REQUIRE(rig.set_address(address).ok);
  USB_REQUIRE(rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf).ok);
  for (unsigned attempt = 0; attempt < 3u; ++attempt) {
    std::uint8_t qualifier[8];
    make_setup(qualifier, 0x80, 0x06, 0x0600, 0, 10);
    USB_REQUIRE(rig.hw.setup(qualifier) == Reply::Ack);
    std::uint32_t len = 0;
    bool data1 = false;
    CHECK(rig.hw.in(0, buf, sizeof buf, len, data1) == Reply::Stall);
    CHECK(rig.hw.in(0, buf, sizeof buf, len, data1) == Reply::Stall);
  }
  Host::Result r = rig.get_descriptor(2, 0, 0, 9, buf, sizeof buf);
  USB_REQUIRE((r.ok && r.length == 9u));
  r = rig.get_descriptor(2, 0, 0, static_cast<std::uint16_t>(buf[2] | (buf[3] << 8)), buf, sizeof buf);
  USB_REQUIRE((r.ok && r.length == rig.cfg_len));
  CHECK(std::memcmp(buf, rig.cfg_desc, rig.cfg_len) == 0);
  USB_REQUIRE(rig.set_configuration(1).ok);
}

}  // namespace

TEST_CASE("usb device: STANDARD, AUDIO and the boot-disk enumerations are unchanged by the lasting stall (the qualifier read and its retried IN), whatever a SETUP does while EPSTALL is set") {
  // The controller's reading of "a SETUP while EPSTALL is set" could not be confirmed: the driver must work if the SETUP is taken (switch off) and if it is refused (switch on).
  const Mode modes[] = {Mode::Standard, Mode::Audio, Mode::Disk};
  for (unsigned pessimistic = 0; pessimistic < 2u; ++pessimistic) {
    for (const Mode mode : modes) {
      CAPTURE(pessimistic);
      Config cfg;
      cfg.mode = mode;
      cfg.speed = Speed::Full;
      Rig& rig = fresh(cfg);
      rig.hw.model_epstall_blocks_setup(pessimistic != 0u);
      Ep0Trace trace;
      rig.dev.bind_trace(&trace);
      trace.arm();
      enumerate_like_linux(rig, 9);
      CHECK(trace.stall_retries() == 3u);  // the retried IN of each of the three qualifier tries
      CHECK(rig.pipe.configured());
      CHECK(rig.hw.address() == 9u);
      CHECK(rig.hw.endpt(0) == kEndpt0);  // the last SETUP ended the last stall
      CHECK(rig.dev.counters().ep0_stalls == 3u);
      CHECK(rig.pipe.counters().stalls == 3u);
      CHECK(rig.dev.counters().tokens_stray == 0u);
      CHECK(rig.dev.counters().pingpong_resyncs == 0u);
      CHECK(rig.host.toggle_errors() == 0u);
      CHECK(rig.hw.toggle_mismatches() == 0u);
      CHECK(rig.dev.counters().errors[4] == 0u);
      // The same device serves the next control transfer and a repeat of the whole sequence.
      std::uint8_t buf[64];
      CHECK(rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf).ok);
      enumerate_like_linux(rig, 11);
      CHECK(rig.pipe.configured());
      CHECK(rig.hw.address() == 11u);
    }
  }
}

TEST_CASE("usb device: OPEN HARDWARE QUESTION (int.15 section 2): if a BD-level stall advances the transmit pointer, the IN data stage after a stall and a retried IN is NAKed") {
  // The driver ends a stall by zeroing both transmit descriptors at the next SETUP and arms the data stage at the pointer it kept (PingPong::flush). It assumes a stalled descriptor is not consumed, so the
  // controller's pointer stays. If the real controller advances it on a BD-level STALL (the host's retried IN, the EPSTALL handshake before it does not), the data stage armed next sits behind a pointer that
  // looks at the other, empty descriptor: NAK for ever (the hang of int.14 again, by another door). Nobody knows which it is; the unit decides (`usb ep0` stall retries, a share after a retried IN). This test
  // only documents what the driver does in that case: it does not resynchronise (that would need CTL ODDRST at a SETUP, which nothing justifies yet).
  std::uint8_t data[64];
  std::uint32_t len = 0;
  bool data1 = false;
  std::uint8_t good[8];
  make_setup(good, 0x80, 0x06, 0x0100, 0, 18);
  std::uint8_t vendor[8];
  make_setup(vendor, 0x40, 0x01, 0, 0, 0);
  {
    // The first handshake only (EPSTALL, no retry): the pointer is untouched whatever the model says, the next transfer is served.
    Rig& rig = fresh();
    USB_REQUIRE(rig.enumerate());
    rig.hw.model_bd_stall_toggles_pointer(true);
    CHECK(rig.hw.setup(vendor) == Reply::Ack);
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    CHECK(rig.host.control_in(good, data, sizeof data).ok);
  }
  {
    // The switch off (the driver's assumption): a stall, a retried IN and the next transfer is served.
    Rig& rig = fresh();
    USB_REQUIRE(rig.enumerate());
    CHECK(rig.hw.setup(vendor) == Reply::Ack);
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    CHECK(rig.host.control_in(good, data, sizeof data).ok);
  }
  {
    // The switch on: a stall, a retried IN (answered by the BD stall, which advances the pointer), then the next IN data stage.
    Rig& rig = fresh();
    USB_REQUIRE(rig.enumerate());
    rig.hw.model_bd_stall_toggles_pointer(true);
    CHECK(rig.hw.setup(vendor) == Reply::Ack);
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    const Host::Result r = rig.host.control_in(good, data, sizeof data);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.stalled);  // not a STALL: the NAK of a descriptor nobody owns
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Nak);
  }
}

TEST_CASE("usb device: a stall whose IN token met the token freeze leaves the controller able to take the next SETUP") {
  // OS-USBDISK int.13: every failed `disk share` ended its EP0 trace with `stall set` and `stall sent` 0 us later (the host's data-stage IN reached the controller while the SETUP still froze it, the
  // interrupt being late), and the unit never took another SETUP. The model (usb_fake_controller.h, a hypothesis consistent with the trace, not a measurement): that IN is answered when the freeze ends
  // and the controller freezes again; a stalled data stage raises no TOKDNE, so only the stall handler can release it.
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  rig.hw.model_late_stall_freeze(true);
  rig.hw.set_auto_isr(false);
  std::uint8_t qualifier[8];
  make_setup(qualifier, 0x80, 0x06, 0x0600, 0, 10);
  CHECK(rig.hw.setup(qualifier) == Reply::Ack);  // acknowledged; the interrupt has not run yet
  std::uint8_t data[64];
  std::uint32_t len = 0;
  bool data1 = false;
  CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Ignored);  // the data stage's IN token: no answer while frozen
  rig.hw.interrupt();                                                     // the late interrupt: the SETUP, the stall, the release, the stall flag
  CHECK(rig.hw.endpt(0) == kEndpt0);                                      // the first handshake cleared the one-shot
  CHECK_FALSE(rig.hw.frozen());
  // The host retries the IN (it took the silence for a lost token): stalled again by the transmit descriptors, counted, and the controller stays released.
  for (unsigned retry = 0; retry < 3u; ++retry) {
    CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Stall);
    rig.hw.interrupt();
    CHECK_FALSE(rig.hw.frozen());
  }
  CHECK(trace.stall_retries() == 3u);
  rig.hw.set_auto_isr(true);
  std::uint8_t good[8];
  make_setup(good, 0x80, 0x06, 0x0100, 0, 18);
  CHECK(rig.host.control_in(good, data, sizeof data).ok);  // the next SETUP is taken and served
  // The trace says when the setup buffers were looked at: after the stall flag, both owned, the freeze released.
  bool found = false;
  for (unsigned i = 0; i < trace.count(); ++i) {
    const Ep0Trace::Entry& e = trace.entry(i);
    if (e.tag == static_cast<std::uint8_t>(Ep0Trace::Tag::SetupArmed)) {
      found = true;
      CHECK(e.w0 == 3u);
      CHECK(e.extra == 1u);
      CHECK(i > 0u);
      CHECK(trace.entry(i - 1u).tag == static_cast<std::uint8_t>(Ep0Trace::Tag::StallSeen));
    }
  }
  CHECK(found);
}

namespace {

// The SetupArmed entries of a trace and the last one's extra.
unsigned setup_armed_entries(const Ep0Trace& trace, std::uint16_t* last_extra = nullptr) {
  unsigned n = 0;
  for (unsigned i = 0; i < trace.count(); ++i) {
    if (trace.entry(i).tag == static_cast<std::uint8_t>(Ep0Trace::Tag::SetupArmed)) {
      ++n;
      if (last_extra != nullptr) {
        *last_extra = trace.entry(i).extra;
      }
    }
  }
  return n;
}

}  // namespace

TEST_CASE("usb device: the stall handler leaves the freeze alone while a TOKDNE is pending, and the trace reads TXSUSPENDTOKENBUSY back (extra bit 1)") {
  // The STALL branch is only entered with TOKDNE clear, but a token can finish while the handler runs; that token's own handling ends the freeze after its work, so the handler must not write CTL.
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  rig.hw.model_late_stall_freeze(true);
  rig.hw.model_token_at_stall_seen();
  rig.hw.set_auto_isr(false);
  std::uint8_t qualifier[8];
  make_setup(qualifier, 0x80, 0x06, 0x0600, 0, 10);
  CHECK(rig.hw.setup(qualifier) == Reply::Ack);
  std::uint8_t data[64];
  std::uint32_t len = 0;
  bool data1 = false;
  CHECK(rig.hw.in(0, data, sizeof data, len, data1) == Reply::Ignored);
  const unsigned writes = rig.hw.ctl_writes();
  rig.hw.interrupt();
  CHECK(rig.hw.ctl_writes() == writes + 1u);  // on_token's own write after the SETUP; none from the stall handler
  CHECK(rig.dev.counters().tokens_stray == 1u);  // the token that finished meanwhile
  std::uint16_t extra = 0;
  CHECK(setup_armed_entries(trace, &extra) == 1u);
  CHECK(extra == 2u);  // freeze kept (bit 0 clear), TXSUSPENDTOKENBUSY still set on the readback (bit 1)
  CHECK(rig.hw.endpt(0) == kEndpt0);  // the first handshake cleared the one-shot all the same
}

TEST_CASE("usb device: the STALL of a halted bulk endpoint does not write CTL or touch endpoint 0 (no stall of endpoint 0 pending)") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  USB_REQUIRE(rig.set_feature_halt(true, 0x82).ok);
  trace.arm();  // from here on only the bulk handshake
  const unsigned writes = rig.hw.ctl_writes();
  std::uint8_t buf[64];
  std::uint32_t len = 0;
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Stall);
  CHECK(rig.hw.ctl_writes() == writes);
  CHECK(setup_armed_entries(trace) == 0u);
  CHECK((rig.hw.endpt(2) & kEndptStall) != 0u);  // the halt stays
}

TEST_CASE("usb device: a bulk halt's STALL flag still up when endpoint 0 stalls does not end the endpoint 0 stall early") {
  // Review fold: stall_ep0 drops the stale flag first, or the interrupt would take it for the handshake of the endpoint 0 stall: the trace would show `stall sent` before the host was answered, and the
  // handler would write CTL. (EPSTALL itself is no longer the handler's to clear, so that is what the test looks at.)
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  trace.arm();
  USB_REQUIRE(rig.set_feature_halt(true, 0x82).ok);
  rig.hw.set_auto_isr(false);
  std::uint8_t buf[64];
  std::uint32_t len = 0;
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Stall);  // the flag is up, the interrupt has not run
  std::uint8_t vendor[8];
  make_setup(vendor, 0x40, 0x01, 0, 0, 0);  // unsupported: endpoint 0 stalls
  CHECK(rig.hw.setup(vendor) == Reply::Ack);
  rig.hw.interrupt();
  CHECK((rig.hw.endpt(0) & kEndptStall) != 0u);  // still stalled: the host has not been answered yet
  CHECK(rig.dev.counters().ep0_stalls == 1u);
  for (unsigned i = 0; i < trace.count(); ++i) {
    CHECK(trace.entry(i).tag != static_cast<std::uint8_t>(Ep0Trace::Tag::StallSeen));  // the bulk halt's flag was dropped, not taken for this stall's handshake
  }
  CHECK(setup_armed_entries(trace) == 0u);
}

TEST_CASE("usb device: a SETUP or a bus reset ends a pending endpoint 0 stall, so a later STALL flag writes no CTL") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  std::uint8_t vendor[8];
  make_setup(vendor, 0x40, 0x01, 0, 0, 0);
  std::uint8_t good[8];
  make_setup(good, 0x80, 0x06, 0x0100, 0, 18);
  std::uint8_t buf[64];
  // 1. A SETUP.
  CHECK(rig.hw.setup(vendor) == Reply::Ack);  // stalls; the host does not follow up
  CHECK((rig.hw.endpt(0) & kEndptStall) != 0u);
  CHECK(rig.host.control_in(good, buf, sizeof buf).ok);
  trace.arm();
  unsigned writes = rig.hw.ctl_writes();
  rig.hw.raise_stall_flag();
  CHECK(rig.hw.ctl_writes() == writes);
  CHECK(setup_armed_entries(trace) == 0u);
  CHECK(trace.stall_retries() == 0u);
  // 2. A bus reset.
  CHECK(rig.hw.setup(vendor) == Reply::Ack);
  rig.hw.bus_reset();
  trace.arm();
  writes = rig.hw.ctl_writes();
  rig.hw.raise_stall_flag();
  CHECK(rig.hw.ctl_writes() == writes);
  CHECK(setup_armed_entries(trace) == 0u);
  // Control: with the stall pending, the same flag does write CTL and the trace says so.
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.hw.setup(vendor) == Reply::Ack);
  trace.arm();
  writes = rig.hw.ctl_writes();
  rig.hw.raise_stall_flag();
  CHECK(rig.hw.ctl_writes() == writes + 1u);
  CHECK(setup_armed_entries(trace) == 1u);
}

TEST_CASE("usb device: CDC bytes to the host, one packet each, toggles alternate across ping-pong") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  write_line(rig, "hello\n");
  CHECK(rig.tx_ring.size() == 1u);
  std::uint8_t buf[64];
  std::uint32_t len = 0;
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Nak);  // nothing is armed until the next SOF
  rig.hw.sof();
  Collected c = drain(rig, 2);
  CHECK((c.length == 6u && c.packets == 1u && std::memcmp(c.bytes, "hello\n", 6) == 0));
  // Three lines written at once: the SOF arms two, the completion of the first arms the third.
  write_line(rig, "one\n");
  write_line(rig, "two\n");
  write_line(rig, "three\n");
  rig.hw.sof();
  c = drain(rig, 2);
  CHECK((c.packets == 3u && c.length == 4u + 4u + 6u));
  CHECK(std::memcmp(c.bytes, "one\ntwo\nthree\n", 14) == 0);
  // A long run: every packet DATA0, DATA1, DATA0, ... with no toggle error on the host side.
  for (int i = 0; i < 25; ++i) {
    char line[16] = "line xx\n";
    line[5] = static_cast<char>('0' + i / 10);
    line[6] = static_cast<char>('0' + i % 10);
    write_line(rig, line);
    rig.hw.sof();
    c = drain(rig, 2);
    USB_REQUIRE(c.length == 8u);
    CHECK(std::memcmp(c.bytes, line, 8) == 0);
  }
  CHECK(rig.host.toggle_errors() == 0u);
  CHECK(rig.dev.counters().cdc_tx_packets == 1u + 3u + 25u);
  CHECK(rig.dev.counters().tokens_cdc_in == 29u);
  CHECK(rig.dev.counters().pingpong_resyncs == 0u);
}

TEST_CASE("usb device: long output is cut into packets of at most 63 bytes, so none is full") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  char text[200];
  for (unsigned i = 0; i < sizeof text; ++i) {
    text[i] = static_cast<char>('a' + i % 26u);
  }
  rig.writer.write(text, sizeof text);
  rig.writer.flush();
  rig.hw.sof();
  std::uint8_t bytes[256];
  std::uint32_t total = 0;
  std::uint32_t packets = 0;
  for (unsigned guard = 0; guard < 8u; ++guard) {
    std::uint32_t len = 0;
    if (rig.host.bulk_in(2, bytes + total, sizeof bytes - total, len) != Reply::Ack) {
      break;
    }
    CHECK(len <= 63u);  // never 64: every packet is short, so a host that waits for a short packet never waits
    total += len;
    ++packets;
  }
  CHECK(total == 200u);
  CHECK(packets == 4u);  // 63 + 63 + 63 + 11
  CHECK(std::memcmp(bytes, text, 200) == 0);
}

TEST_CASE("usb device: a host that never reads loses the oldest bytes, counted, and the firmware never waits") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  constexpr unsigned kCap = CdcTxRing::capacity();
  constexpr unsigned kExtra = 25;
  char line[16];
  const auto name = [&](unsigned i) {
    std::strcpy(line, "msg 000\n");
    line[4] = static_cast<char>('0' + i / 100);
    line[5] = static_cast<char>('0' + (i / 10) % 10);
    line[6] = static_cast<char>('0' + i % 10);
  };
  for (unsigned i = 0; i < kCap + kExtra; ++i) {  // more chunks than the ring keeps
    name(i);
    write_line(rig, line);
  }
  CHECK(rig.writer.dropped_chunks() == kExtra);
  CHECK(rig.writer.bytes_written() == (kCap + kExtra) * 8u);
  rig.hw.sof();
  const Collected c = drain(rig, 2);
  CHECK(c.packets == kCap);
  name(kExtra);
  CHECK(std::memcmp(c.bytes, line, 8) == 0);                       // the oldest are gone
  name(kCap + kExtra - 1);
  CHECK(std::memcmp(c.bytes + (kCap - 1u) * 8u, line, 8) == 0);    // the newest is there
  CHECK(rig.tx_ring.dropped() == kExtra);                          // and the reader noticed the same number
  // Not configured: nothing is armed, the ring just cycles.
  const std::uint32_t packets_before = rig.dev.counters().cdc_tx_packets;
  const std::uint32_t dropped_before = rig.writer.dropped_chunks();
  rig.hw.bus_reset();
  for (unsigned i = 0; i < kCap + 10u; ++i) {
    write_line(rig, "x\n");
  }
  rig.hw.sof();
  CHECK(rig.dev.counters().cdc_tx_packets == packets_before);
  CHECK(rig.writer.dropped_chunks() == dropped_before + 10u);
  CHECK(rig.pipe.state() == DeviceState::Default);
}

TEST_CASE("usb device: CDC bytes from the host land in the receive ring") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.host.bulk_out(2, reinterpret_cast<const std::uint8_t*>("ab"), 2) == Reply::Ack);
  CHECK(rig.host.bulk_out(2, reinterpret_cast<const std::uint8_t*>("cde"), 3) == Reply::Ack);
  CHECK(rig.host.bulk_out(2, reinterpret_cast<const std::uint8_t*>("f"), 1) == Reply::Ack);
  CHECK(rig.hw.toggle_mismatches() == 0u);
  CHECK(rig.dev.counters().cdc_rx_bytes == 6u);
  ByteChunk chunk;
  USB_REQUIRE(rig.rx_ring.read(&chunk, 1) == 1u);
  CHECK((chunk.len == 2u && std::memcmp(chunk.data, "ab", 2) == 0));
  USB_REQUIRE(rig.rx_ring.read(&chunk, 1) == 1u);
  CHECK((chunk.len == 3u && std::memcmp(chunk.data, "cde", 3) == 0));
  USB_REQUIRE(rig.rx_ring.read(&chunk, 1) == 1u);
  CHECK((chunk.len == 1u && chunk.data[0] == 'f'));
  CHECK(rig.rx_ring.read(&chunk, 1) == 0u);
}

TEST_CASE("usb device: a main loop that falls behind NAKs the host instead of losing bytes") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  // The main loop reads nothing. Numbered one-byte packets: the device accepts them until the ring holds kCdcRxHoldChunks (and the two
  // descriptors that were armed), then it stops re-arming and the host is NAKed.
  std::uint32_t accepted = 0;
  std::uint32_t naks = 0;
  for (std::uint32_t i = 0; i < 40; ++i) {
    const std::uint8_t v = static_cast<std::uint8_t>(i);
    if (rig.host.bulk_out(2, &v, 1) == Reply::Ack) {
      ++accepted;
    } else {
      ++naks;
    }
  }
  CHECK(accepted >= kCdcRxHoldChunks);
  CHECK(accepted <= kCdcRxHoldChunks + 2u);  // the two descriptors armed when the limit was reached
  CHECK(naks == 40u - accepted);
  CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);  // nothing was overwritten
  CHECK(rig.dev.counters().cdc_rx_holds == 1u);           // one hold, counted once, not once per NAK
  CHECK(rig.rx_ring.size() == accepted);
  // The host keeps trying: still NAKed. Reading two chunks is not enough to lift the hold (ring still above the limit) ...
  ByteChunk chunk;
  USB_REQUIRE(rig.rx_ring.read(&chunk, 1) == 1u);
  rig.hw.sof();
  const std::uint8_t v = 100;
  if (accepted > kCdcRxHoldChunks) {
    CHECK(rig.host.bulk_out(2, &v, 1) == Reply::Nak);
  }
  // ... reading the ring down below it is, and the next start of frame re-arms the descriptors within a millisecond.
  while (rig.rx_ring.size() > 4u) {
    USB_REQUIRE(rig.rx_ring.read(&chunk, 1) == 1u);
  }
  CHECK(rig.host.bulk_out(2, &v, 1) == Reply::Nak);  // not before the SOF: the interrupt did not run
  rig.hw.sof();
  CHECK(rig.host.bulk_out(2, &v, 1) == Reply::Ack);
  CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);
}

TEST_CASE("usb device: a host that retries on NAK delivers every packet, in order, to a slow reader") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  constexpr std::uint32_t kPackets = 300;
  std::uint32_t sent = 0;
  std::uint32_t next_read = 0;
  std::uint32_t step = 0;
  bool in_order = true;
  while ((sent < kPackets || rig.rx_ring.size() != 0u) && step < 20000u) {
    ++step;
    if (sent < kPackets) {
      const std::uint8_t v[2] = {static_cast<std::uint8_t>(sent & 0xFFu), static_cast<std::uint8_t>(sent >> 8)};
      if (rig.host.bulk_out(2, v, 2) == Reply::Ack) {
        ++sent;
      }
    }
    if (step % 5u == 0u) {  // the main loop reads one chunk every fifth millisecond: slower than the host sends
      ByteChunk chunk;
      if (rig.rx_ring.read(&chunk, 1) == 1u) {
        in_order = in_order && chunk.len == 2u && (chunk.data[0] | (chunk.data[1] << 8)) == static_cast<int>(next_read);
        ++next_read;
      }
    }
    rig.hw.sof();
  }
  CHECK(sent == kPackets);
  CHECK(next_read == kPackets);  // every packet arrived exactly once
  CHECK(in_order);
  CHECK(rig.dev.counters().cdc_rx_dropped_chunks == 0u);
  CHECK(rig.dev.counters().cdc_rx_holds > 0u);
  CHECK(rig.hw.toggle_mismatches() == 0u);
}

TEST_CASE("usb device: a host that retransmits with the wrong toggle is acknowledged and ignored") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  const std::uint8_t a = 'a';
  CHECK(rig.hw.out(2, &a, 1, false) == Reply::Ack);  // DATA0: expected by the even descriptor
  CHECK(rig.hw.out(2, &a, 1, false) == Reply::Ack);  // DATA0 again where DATA1 is due: a duplicate, discarded
  CHECK(rig.hw.toggle_mismatches() == 1u);
  CHECK(rig.dev.counters().cdc_rx_bytes == 1u);
  const std::uint8_t b = 'b';
  CHECK(rig.hw.out(2, &b, 1, true) == Reply::Ack);   // DATA1: the next real packet
  CHECK(rig.dev.counters().cdc_rx_bytes == 2u);
}

TEST_CASE("usb device: USB-MIDI event packets both ways, reserved events and ragged bytes counted") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  // Host to device: note on, note off, a padding word, a reserved cable event, and 2 stray bytes.
  const std::uint8_t packet[18] = {0x09, 0x90, 0x3C, 0x7F, 0x08, 0x80, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x01, 0xAA, 0xBB, 0xCC, 0x55, 0x66};
  CHECK(rig.host.bulk_out(3, packet, sizeof packet) == Reply::Ack);
  CHECK(rig.dev.counters().midi_out_packets == 1u);
  CHECK(rig.dev.counters().midi_out_events == 2u);
  CHECK(rig.dev.counters().midi_out_skipped == 2u);
  CHECK(rig.dev.counters().midi_out_bad_bytes == 2u);
  std::uint32_t ev[4];
  USB_REQUIRE(rig.midi_out.read(ev, 4) == 2u);
  CHECK(ev[0] == 0x7F3C9009u);  // memory order is wire order: 09 90 3C 7F
  CHECK(ev[1] == 0x003C8008u);
  // Device to host: 20 events become two packets of 16 and 4, DATA0 then DATA1.
  std::uint32_t out[20];
  for (std::uint32_t i = 0; i < 20u; ++i) {
    out[i] = 0x00000009u | (i << 8);
  }
  rig.midi_in.write(out, 16);
  rig.midi_in.write(out + 16, 4);
  rig.hw.sof();
  const Collected c = drain(rig, 3);
  CHECK(c.packets == 2u);
  CHECK(c.length == 80u);
  CHECK(std::memcmp(c.bytes, out, 80) == 0);
  CHECK(rig.host.toggle_errors() == 0u);
  // A bulk packet that is full of events (16) is fine: the largest an event packet gets is 64 bytes.
  rig.midi_in.write(out, 16);
  rig.hw.sof();
  CHECK(drain(rig, 3).length == 64u);
}

TEST_CASE("usb device: a bus reset returns to the Default state and a re-enumeration brings everything back") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  write_line(rig, "before\n");
  rig.hw.sof();
  CHECK(drain(rig, 2).length == 7u);
  rig.hw.bus_reset();
  CHECK(rig.pipe.state() == DeviceState::Default);
  CHECK(rig.hw.address() == 0u);
  CHECK((rig.hw.endpt(1) == 0 && rig.hw.endpt(2) == 0 && rig.hw.endpt(3) == 0));  // the endpoints are gone until configured
  CHECK(rig.hw.endpt(0) == kEndpt0);
  CHECK(bd::owned(rig.bdt.entry[bdt_index(0, false, false)].desc));
  USB_REQUIRE(rig.enumerate(11));
  write_line(rig, "after\n");
  rig.hw.sof();
  const Collected c = drain(rig, 2);
  CHECK((c.length == 6u && std::memcmp(c.bytes, "after\n", 6) == 0));
  CHECK(rig.host.toggle_errors() == 0u);
}

TEST_CASE("usb device: halt stalls the endpoint, clear-halt restarts at DATA0 whatever the ping-pong position") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  // Three packets: the controller's pointer ends on the odd descriptor, the host's next expected toggle is DATA1.
  for (int i = 0; i < 3; ++i) {
    write_line(rig, "p\n");
    rig.hw.sof();
    USB_REQUIRE(drain(rig, 2).length == 2u);
  }
  CHECK(rig.hw.tx_next_odd(2));
  USB_REQUIRE(rig.set_feature_halt(true, 0x82).ok);
  CHECK((rig.hw.endpt(2) & kEndptStall) != 0u);
  std::uint8_t buf[64];
  std::uint32_t len = 0;
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Stall);
  CHECK(rig.host.bulk_out(2, buf, 1) == Reply::Stall);  // both directions of the number
  CHECK((rig.hw.endpt(2) & kEndptStall) != 0u);          // the STALL interrupt must not clear a halted bulk endpoint
  CHECK(rig.pipe.halted(0x82));
  // Data written meanwhile waits in the ring.
  write_line(rig, "kept\n");
  USB_REQUIRE(rig.set_feature_halt(false, 0x82).ok);
  CHECK((rig.hw.endpt(2) & kEndptStall) == 0u);
  CHECK_FALSE(rig.pipe.halted(0x82));
  rig.hw.sof();
  const Collected c = drain(rig, 2);
  CHECK((c.length == 5u && std::memcmp(c.bytes, "kept\n", 5) == 0));
  CHECK(rig.host.toggle_errors() == 0u);  // DATA0 after the clear, on the descriptor the controller serves next
  CHECK(rig.dev.counters().pingpong_resyncs == 0u);
  // The OUT side restarts from DATA0 as well.
  const std::uint8_t x = 'x';
  CHECK(rig.host.bulk_out(2, &x, 1) == Reply::Ack);
  CHECK(rig.hw.toggle_mismatches() == 0u);
  ByteChunk chunk;
  CHECK(rig.rx_ring.read(&chunk, 1) == 1u);
  // The interrupt endpoint can be halted too, and a number that does not exist stalls the request.
  CHECK(rig.set_feature_halt(true, 0x81).ok);
  CHECK(rig.set_feature_halt(false, 0x81).ok);
  CHECK(rig.set_feature_halt(true, 0x05).stalled);
}

TEST_CASE("usb device: suspend, resume and bus errors") {
  Rig& rig = fresh();
  Recorder rec;
  rig.dev.set_event_hook(&Recorder::hook, &rec);
  USB_REQUIRE(rig.enumerate());
  rec.count = 0;
  rig.hw.bus_idle();
  CHECK(rig.pipe.suspended());
  CHECK(rig.pipe.state() == DeviceState::Configured);  // the configuration survives a suspend
  rig.hw.bus_resume();
  CHECK_FALSE(rig.pipe.suspended());
  rig.hw.bus_idle();
  rig.hw.sof();  // frames flowing again: awake, whether or not the resume flag was seen
  CHECK_FALSE(rig.pipe.suspended());
  USB_REQUIRE(rec.count == 4u);
  CHECK((rec.events[0] == UsbEvent::Suspended && rec.events[1] == UsbEvent::Resumed && rec.events[2] == UsbEvent::Suspended &&
         rec.events[3] == UsbEvent::Resumed));
  // After a resume the device still works.
  write_line(rig, "awake\n");
  rig.hw.sof();
  CHECK(drain(rig, 2).length == 6u);
  // Error flags are cleared and counted by bit.
  rig.hw.bus_error(0x80u | 0x04u);
  rig.hw.bus_error(0x04u);
  CHECK(rig.dev.counters().errors[7] == 1u);
  CHECK(rig.dev.counters().errors[2] == 2u);
  CHECK(rig.hw.errstat() == 0u);
  CHECK(rig.audio.sof == 2u);  // every SOF is counted for `diag usb`
}

TEST_CASE("usb device: the interrupt's own cost is measured") {
  Rig& rig = fresh();
  CHECK(rig.dev.counters().isr_count == 0u);  // the construction-time reset is prepare(), not an interrupt
  const std::uint32_t before = rig.dev.counters().isr_count;
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.dev.counters().isr_count > before);
  CHECK(rig.dev.counters().isr_cycles_last > 0u);
  CHECK(rig.dev.counters().isr_cycles_max >= rig.dev.counters().isr_cycles_last);
  // OS-USBDISK U1d: the sum of every entry's cycles (the bench's window takes the difference of two reads); the window's peak reset leaves it and the count alone.
  CHECK(rig.dev.counters().isr_cycles_sum >= rig.dev.counters().isr_cycles_max);
  const std::uint32_t sum = rig.dev.counters().isr_cycles_sum;
  const std::uint32_t entries = rig.dev.counters().isr_count;
  rig.dev.reset_isr_peak();
  CHECK(rig.dev.counters().isr_cycles_max == 0u);
  CHECK(rig.dev.counters().isr_cycles_sum == sum);
  CHECK(rig.dev.counters().isr_count == entries);
  rig.dev.isr();
  CHECK(rig.dev.counters().isr_cycles_sum >= sum + rig.dev.counters().isr_cycles_last);
  rig.dev.reset_counters();
  CHECK(rig.dev.counters().isr_cycles_max == 0u);
  CHECK(rig.dev.counters().isr_cycles_sum == 0u);
}

// ---- the bound on one interrupt's work ---------------------------------------------------------------------------------

namespace {

// A broken controller whose TOKDNE never clears: the interrupt must give up after kMaxIsrLoops passes, not spin.
struct StuckHw {
  std::uint8_t istat() { return sie::kIstatTokDne; }
  void istat_clear(std::uint8_t) {}
  std::uint8_t stat() { return 0; }  // endpoint 0, receive, even
  std::uint8_t errstat() { return 0; }
  void errstat_clear(std::uint8_t) {}
  void ctl(std::uint8_t) { ++ctl_writes; }
  std::uint8_t ctl() { return 0; }
  void set_address(std::uint8_t) {}
  void set_inten(std::uint8_t) {}
  void set_erren(std::uint8_t) {}
  std::uint8_t endpt(unsigned) { return 0; }
  void endpt(unsigned, std::uint8_t) {}
  void dmb() {}
  std::uint32_t cycles() { return 0; }
  unsigned ctl_writes = 0;
};

struct alignas(kBdtAlign) StuckRig {
  StuckRig()
      : dev_len(build_device_descriptor(cfg, dev_desc, sizeof dev_desc)),
        cfg_len(build_config_descriptor(cfg, cfg_desc, sizeof cfg_desc)),
        pipe(cfg, dev_desc, dev_len, cfg_desc, cfg_len),
        dev(hw, bdt, pipe, audio, tx, rx, mi, mo) {}
  BdtTable bdt{};
  Config cfg;
  std::uint8_t dev_desc[kDeviceDescriptorSize] = {};
  std::uint8_t cfg_desc[kMaxConfigDescriptorSize] = {};
  std::size_t dev_len;
  std::size_t cfg_len;
  ControlPipe pipe;
  Counters audio;
  CdcTxRing tx;
  CdcRxRing rx;
  MidiRing mi;
  MidiRing mo;
  StuckHw hw;
  UsbDevice<StuckHw> dev;
};

}  // namespace

TEST_CASE("usb device: one interrupt does bounded work even when the controller never clears a flag") {
  alignas(kBdtAlign) static unsigned char storage[sizeof(StuckRig)];
  StuckRig* rig = new (storage) StuckRig();
  rig->dev.isr();
  CHECK(rig->dev.counters().isr_loop_limit == 1u);
  CHECK(rig->dev.counters().tokens_ep0 == kMaxIsrLoops);  // exactly the cap, then it returned
  rig->~StuckRig();
}

TEST_CASE("usb device: a full STAT FIFO is drained in one interrupt") {
  Rig& rig = fresh();
  USB_REQUIRE(rig.enumerate());
  rig.hw.set_auto_isr(false);
  // Queue four completed tokens (the FIFO is four deep): two IN packets, two OUT packets.
  write_line(rig, "a\n");
  write_line(rig, "b\n");
  rig.hw.sof();
  rig.hw.interrupt();  // arms the two IN descriptors
  std::uint8_t buf[64];
  std::uint32_t len = 0;
  const std::uint8_t x = 'x';
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Ack);
  CHECK(rig.host.bulk_in(2, buf, sizeof buf, len) == Reply::Ack);
  CHECK(rig.host.bulk_out(2, &x, 1) == Reply::Ack);
  CHECK(rig.host.bulk_out(2, &x, 1) == Reply::Ack);
  CHECK(rig.hw.fifo_depth() == 4u);
  const std::uint32_t calls = rig.hw.isr_calls();
  rig.hw.interrupt();
  CHECK(rig.hw.fifo_depth() == 0u);
  CHECK(rig.hw.isr_calls() == calls + 1u);  // one entry handled all four
  CHECK(rig.dev.counters().tokens_cdc_in >= 2u);
  CHECK(rig.dev.counters().tokens_cdc_out >= 2u);
}
