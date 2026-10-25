// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the buffer descriptor word, the ping-pong and data-toggle bookkeeping, and the endpoint table of each mode against the
// descriptors the builder writes. Runs on the host and on QEMU (qemu_usb_device).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/endpoint.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace o3sdk::usb;

TEST_CASE("usb endpoint: buffer descriptor words") {
  // OWN | DTS, byte count in bits 25..16, DATA1 in bit 6.
  CHECK(bd::arm(64, false) == 0x00400088u);
  CHECK(bd::arm(64, true) == 0x004000C8u);
  CHECK(bd::arm(0, true) == 0x000000C8u);
  CHECK(bd::arm(8, false) == 0x00080088u);
  CHECK(bd::owned(bd::arm(1, false)));
  CHECK_FALSE(bd::owned(0x00080034u));
  // What the controller writes back: count and the token PID in bits 5..2, OWN clear.
  const std::uint32_t after_setup = (8u << 16) | (bd::kPidSetup << 2);
  CHECK(bd::pid(after_setup) == 0xDu);
  CHECK(bd::bytes(after_setup) == 8u);
  CHECK(bd::pid((3u << 16) | (bd::kPidOut << 2)) == 0x1u);
  CHECK(bd::pid((0u << 16) | (bd::kPidIn << 2)) == 0x9u);
  CHECK(bd::bytes(0xFFFF0000u) == 0x3FFu);  // the count field is ten bits
}

TEST_CASE("usb endpoint: table geometry") {
  static_assert(alignof(BdtTable) == 512, "BDT alignment");
  CHECK(sizeof(BdtTable) % 512u == 0u);
  CHECK(kBdtEntries == kBdtEndpoints * 4u);
  if (sizeof(void*) == 4u) {
    CHECK(sizeof(BdtEntry) == 8u);  // the hardware layout: this is the check that matters, and it runs on the QEMU M4
  }
  // The entry the STAT register names: (endpoint << 2) | (tx << 1) | odd, which is STAT >> 2.
  CHECK(bdt_index(0, false, false) == 0u);
  CHECK(bdt_index(0, false, true) == 1u);
  CHECK(bdt_index(0, true, false) == 2u);
  CHECK(bdt_index(0, true, true) == 3u);
  CHECK(bdt_index(2, true, true) == 11u);
  for (unsigned ep = 0; ep < kBdtEndpoints; ++ep) {
    for (unsigned tx = 0; tx < 2u; ++tx) {
      for (unsigned odd = 0; odd < 2u; ++odd) {
        const unsigned stat = (ep << 4) | (tx << 3) | (odd << 2);
        CHECK((stat >> 2) == bdt_index(ep, tx != 0u, odd != 0u));
      }
    }
  }
}

TEST_CASE("usb endpoint: ping-pong alternates parity and the DATA toggle") {
  PingPong pp;
  CHECK(pp.can_arm());
  CHECK(pp.armed() == 0u);
  const PingPong::Slot a = pp.arm();
  const PingPong::Slot b = pp.arm();
  CHECK_FALSE(a.odd);
  CHECK_FALSE(a.data1);
  CHECK(b.odd);
  CHECK(b.data1);
  CHECK_FALSE(pp.can_arm());  // two descriptors, both with the controller
  CHECK(pp.armed() == 2u);
  // In order: even finishes, then odd.
  CHECK(pp.complete(false));
  CHECK(pp.can_arm());
  const PingPong::Slot c = pp.arm();
  CHECK_FALSE(c.odd);  // the even descriptor again
  CHECK_FALSE(c.data1);  // DATA0, D1, D0: the toggle alternates per packet, not per descriptor
  CHECK(pp.complete(true));
  CHECK(pp.complete(false));
  CHECK(pp.armed() == 0u);
  // Idle again after three packets (even, odd, even): the controller serves the odd descriptor next, and the host expects the
  // fourth packet to carry DATA1.
  const PingPong::Slot d = pp.arm();
  CHECK(d.odd);
  CHECK(d.data1);
}

TEST_CASE("usb endpoint: a long run keeps parity and toggle in step, one at a time and two ahead") {
  PingPong one;
  bool odd_expected = false;
  bool data1_expected = false;
  for (int i = 0; i < 1001; ++i) {  // an odd count, so the pair has wrapped an uneven number of times
    const PingPong::Slot s = one.arm();
    CHECK(s.odd == odd_expected);
    CHECK(s.data1 == data1_expected);
    CHECK(one.complete(s.odd));
    odd_expected = !odd_expected;
    data1_expected = !data1_expected;
  }
  PingPong two;
  PingPong::Slot queue[2];
  queue[0] = two.arm();
  queue[1] = two.arm();
  unsigned head = 0;
  bool want_odd = false;
  bool want_data1 = false;
  for (int i = 0; i < 1000; ++i) {
    CHECK(queue[head].odd == want_odd);
    CHECK(queue[head].data1 == want_data1);
    CHECK(two.complete(queue[head].odd));
    queue[head] = two.arm();  // refill the one that finished, as the receive path does
    head ^= 1u;
    want_odd = !want_odd;
    want_data1 = !want_data1;
  }
}

TEST_CASE("usb endpoint: reset, flush and resynchronisation") {
  PingPong pp;
  (void)pp.arm();
  CHECK(pp.complete(false));
  CHECK(pp.next_done_odd());  // the controller now serves the odd descriptor
  // flush: everything taken back, the controller's position kept, DATA0 next (ClearFeature(HALT), configuration).
  (void)pp.arm();
  pp.flush();
  CHECK(pp.armed() == 0u);
  const PingPong::Slot s = pp.arm();
  CHECK(s.odd);
  CHECK_FALSE(s.data1);
  // reset: the controller's pointers went back to even (CTL[ODDRST] at a bus reset).
  pp.reset();
  const PingPong::Slot t = pp.arm();
  CHECK_FALSE(t.odd);
  CHECK_FALSE(t.data1);
  // A completion that arrives for the wrong descriptor is reported and the bookkeeping follows the controller.
  PingPong q;
  (void)q.arm();
  CHECK_FALSE(q.complete(true));        // even was expected
  CHECK(q.armed() == 0u);
  CHECK_FALSE(q.next_done_odd());       // the descriptor after the odd one that finished is even
  // A completion with nothing armed (a stale token after a flush) does not underflow.
  PingPong r;
  CHECK_FALSE(r.complete(false));
  CHECK(r.armed() == 0u);
  CHECK(r.can_arm());
}

// ---- the endpoint table against the descriptors ---------------------------------------------------------------------

namespace {

struct EpDesc {
  std::uint8_t address;
  std::uint8_t attributes;
  std::uint16_t max_packet;
};

unsigned collect(const Config& cfg, EpDesc* out, unsigned cap) {
  std::uint8_t d[kMaxConfigDescriptorSize];
  const std::size_t n = build_config_descriptor(cfg, d, sizeof d);
  unsigned count = 0;
  for (std::size_t i = 0; i + 1 < n; i += d[i]) {
    if (d[i + 1] == 5 && d[i] >= 7 && count < cap) {  // an endpoint descriptor (the class-specific ones have type 0x25)
      out[count++] = EpDesc{d[i + 2], d[i + 3], static_cast<std::uint16_t>(d[i + 4] | (d[i + 5] << 8))};
    }
  }
  return count;
}

}  // namespace

TEST_CASE("usb endpoint: the endpoint table agrees with every descriptor of every mode") {
  for (const Mode mode : {Mode::Standard, Mode::Audio}) {
    for (const bool usb_return : {true, false}) {
      if (mode == Mode::Standard && !usb_return) {
        continue;  // STANDARD has no return stream to leave out
      }
      Config cfg;
      cfg.mode = mode;
      cfg.usb_return = usb_return;
      EpDesc eps[16];
      const unsigned n = collect(cfg, eps, 16);
      CHECK(n == (mode == Mode::Audio ? (usb_return ? 8u : 6u) : 5u));  // the validator's per-mode counts
      bool seen_number[16] = {};
      for (unsigned i = 0; i < n; ++i) {
        const unsigned type = eps[i].attributes & 3u;
        const EndpointKind kind = endpoint_kind(mode, eps[i].address, usb_return);
        INFO("endpoint " << static_cast<unsigned>(eps[i].address) << " return " << usb_return);
        CHECK(kind == (type == 3u ? EndpointKind::Interrupt : type == 2u ? EndpointKind::Bulk : EndpointKind::Isochronous));
        seen_number[eps[i].address & 0x0Fu] = true;
        // The direction the endpoint has is the direction the ENDPTn value enables.
        const std::uint8_t cfg_bits = endpoint_config(mode, static_cast<std::uint8_t>(eps[i].address & 0x0Fu), usb_return);
        const bool in = (eps[i].address & 0x80u) != 0u;
        CHECK(((cfg_bits & (in ? kEndptTxEn : kEndptRxEn)) != 0u));
        CHECK(((cfg_bits & kEndptCtlDisable) != 0u));
        // Handshake on bulk and interrupt, none on isochronous.
        CHECK(((cfg_bits & kEndptHandshake) != 0u) == (type != 1u));
        CHECK(kind != EndpointKind::None);
        CHECK(eps[i].address <= (0x80u | last_endpoint(mode, usb_return)));
      }
      // A number that no descriptor mentions has no register value; the numbers the descriptor uses all have one.
      for (std::uint8_t number = 1; number < 16; ++number) {
        CHECK((endpoint_config(mode, number, usb_return) != 0u) == seen_number[number]);
      }
      CHECK(endpoint_kind(mode, 0x00, usb_return) == EndpointKind::Control);
      CHECK(endpoint_kind(mode, 0x80, usb_return) == EndpointKind::Control);
      CHECK(endpoint_kind(mode, 0x06, usb_return) == EndpointKind::None);
      CHECK(endpoint_kind(mode, 0x8F, usb_return) == EndpointKind::None);
    }
  }
  // The values themselves: 0x15 interrupt IN, 0x1D bulk IN and OUT.
  CHECK(endpoint_config(Mode::Standard, 1, true) == 0x15);
  CHECK(endpoint_config(Mode::Standard, 2, true) == 0x1D);
  CHECK(endpoint_config(Mode::Standard, 3, true) == 0x1D);
  CHECK(endpoint_config(Mode::Standard, 4, true) == 0x00);
  CHECK(endpoint_config(Mode::Audio, 4, true) == 0x1C);  // isochronous both ways: no handshake
  CHECK(endpoint_config(Mode::Audio, 5, true) == 0x14);  // the feedback endpoint: IN only
  CHECK(kEndpt0 == 0x0D);
  // USB RETURN off (OS-USBRET): endpoint 4 is the stems IN alone, endpoint 5 does not exist.
  CHECK(endpoint_config(Mode::Audio, 4, false) == 0x14);
  CHECK(endpoint_config(Mode::Audio, 5, false) == 0x00);
  CHECK(endpoint_kind(Mode::Audio, layout::kEpAudioOut, false) == EndpointKind::None);
  CHECK(endpoint_kind(Mode::Audio, layout::kEpAudioFeedback, false) == EndpointKind::None);
  CHECK(endpoint_kind(Mode::Audio, layout::kEpAudioIn, false) == EndpointKind::Isochronous);
  CHECK(last_endpoint(Mode::Audio, true) == 5);
  CHECK(last_endpoint(Mode::Audio, false) == 4);
  CHECK(last_endpoint(Mode::Standard, false) == 3);
  CHECK(endpoint_config(Mode::Standard, 1, false) == 0x15);  // STANDARD ignores the flag
}
