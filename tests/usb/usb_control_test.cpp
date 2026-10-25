// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: endpoint 0 and the device's standard state (usb/control.h): every standard request, the CDC and UAC2 class requests,
// the zero-length-packet rule, the stall paths, the DATA toggle sequence, SET_ADDRESS after the status stage, suspend and reset.
// The pipe is driven by hand-written SETUP packets: no controller and no bus. Runs on the host and on QEMU (qemu_usb_device).
#include <doctest/doctest.h>

#include "usb_test_util.h"

#include <o3sdk/usb/control.h>
#include <o3sdk/usb/descriptors.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <new>

using namespace o3sdk::usb;

namespace {

// A pipe over freshly built descriptors, in static storage (no heap on QEMU).
struct Fixture {
  explicit Fixture(Config c)
      : cfg(c),
        dev_len(build_device_descriptor(cfg, dev, sizeof dev)),
        cfg_len(build_config_descriptor(cfg, conf, sizeof conf)),
        pipe(cfg, dev, dev_len, conf, cfg_len) {}
  Config cfg;
  std::uint8_t dev[kDeviceDescriptorSize] = {};
  std::uint8_t conf[kMaxConfigDescriptorSize] = {};
  std::size_t dev_len;
  std::size_t cfg_len;
  ControlPipe pipe;
};

Fixture& fixture(Mode mode = Mode::Standard, bool usb_return = true) {
  static unsigned char storage[sizeof(Fixture)];
  static Fixture* current = nullptr;
  if (current != nullptr) {
    current->~Fixture();
  }
  Config c;
  c.mode = mode;
  c.usb_return = usb_return;
  current = new (storage) Fixture(c);
  return *current;
}

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

void raw(std::uint8_t out[8], std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint16_t length) {
  out[0] = type;
  out[1] = request;
  out[2] = static_cast<std::uint8_t>(value);
  out[3] = static_cast<std::uint8_t>(value >> 8);
  out[4] = static_cast<std::uint8_t>(index);
  out[5] = static_cast<std::uint8_t>(index >> 8);
  out[6] = static_cast<std::uint8_t>(length);
  out[7] = static_cast<std::uint8_t>(length >> 8);
}

ControlStep setup(ControlPipe& p, std::uint8_t type, std::uint8_t request, std::uint16_t value, std::uint16_t index,
                  std::uint16_t length) {
  std::uint8_t r[8];
  raw(r, type, request, value, index, length);
  return p.on_setup(r);
}

bool is_in(const ControlStep& s) { return s.kind == ControlStep::Kind::In; }
bool is_stall(const ControlStep& s) { return s.kind == ControlStep::Kind::Stall; }

// Finishes the status stage of a no-data request: the zero-length IN is acknowledged.
ControlStep finish_status(ControlPipe& p) { return p.on_in_done(); }

// Plays an IN data stage to the end the way a host does, collecting the packets: returns the bytes and the toggles seen.
struct Played {
  std::uint8_t bytes[512];
  std::uint32_t length = 0;
  std::uint32_t packets = 0;
  bool toggles_alternate = true;
  bool first_is_data1 = false;
  bool ended_with_zlp = false;
  std::uint32_t last_len = 0;
};

Played play_in(ControlPipe& p, ControlStep step) {
  Played r;
  bool expect = true;
  while (step.kind == ControlStep::Kind::In) {
    if (r.packets == 0u) {
      r.first_is_data1 = step.data1;
    }
    if (step.data1 != expect) {
      r.toggles_alternate = false;
    }
    expect = !expect;
    std::memcpy(r.bytes + r.length, step.data, step.len);
    r.length += step.len;
    ++r.packets;
    r.last_len = step.len;
    r.ended_with_zlp = step.len == 0u;
    step = p.on_in_done();
  }
  // The host's status stage: an OUT zero-length packet.
  const ControlStep s = p.on_out_done(nullptr, 0);
  CHECK(s.kind == ControlStep::Kind::None);
  return r;
}

void configure(ControlPipe& p) {
  CHECK(is_in(setup(p, 0x00, 0x05, 3, 0, 0)));
  (void)finish_status(p);
  CHECK(is_in(setup(p, 0x00, 0x09, 1, 0, 0)));
  (void)finish_status(p);
  CHECK(p.state() == DeviceState::Configured);
}

}  // namespace

TEST_CASE("usb control: GET_DESCRIPTOR device, wLength shorter, equal, longer") {
  Fixture& f = fixture();
  ControlPipe& p = f.pipe;
  // wLength 8: the first 8 bytes in one packet, as a host does before it knows the endpoint 0 packet size.
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0100, 0, 8));
  CHECK(r.length == 8u);
  CHECK(r.packets == 1u);
  CHECK(r.first_is_data1);
  CHECK(std::memcmp(r.bytes, f.dev, 8) == 0);
  // wLength 18 and 255: 18 bytes, one short packet, no zero-length packet.
  r = play_in(p, setup(p, 0x80, 0x06, 0x0100, 0, 18));
  CHECK((r.length == 18u && r.packets == 1u && !r.ended_with_zlp));
  CHECK(std::memcmp(r.bytes, f.dev, 18) == 0);
  r = play_in(p, setup(p, 0x80, 0x06, 0x0100, 0, 255));
  CHECK((r.length == 18u && r.packets == 1u && !r.ended_with_zlp));
  CHECK(rd16(r.bytes + 8) == 0x16D0);  // the tracker's VID
}

TEST_CASE("usb control: a configuration descriptor spans packets with alternating toggles") {
  Fixture& f = fixture();
  ControlPipe& p = f.pipe;
  CHECK(f.cfg_len == 175u);  // 64 + 64 + 47: ends on a short packet
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 255));
  CHECK(r.length == 175u);
  CHECK(r.packets == 3u);
  CHECK(r.first_is_data1);
  CHECK(r.toggles_alternate);
  CHECK(r.last_len == 47u);
  CHECK_FALSE(r.ended_with_zlp);
  CHECK(std::memcmp(r.bytes, f.conf, 175) == 0);
  // The host may stop early with wLength 9 (the header) or a value that is a packet multiple.
  r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 9));
  CHECK((r.length == 9u && r.packets == 1u));
  r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 64));
  CHECK((r.length == 64u && r.packets == 1u && !r.ended_with_zlp));  // asked for exactly one full packet: no ZLP
  r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 128));
  CHECK((r.length == 128u && r.packets == 2u && !r.ended_with_zlp));
  CHECK(r.toggles_alternate);
}

TEST_CASE("usb control: the AUDIO configuration is 6 x 64 bytes and needs a zero-length packet") {
  Fixture& f = fixture(Mode::Audio);
  ControlPipe& p = f.pipe;
  CHECK(f.cfg_len == 384u);
  // Host asks for more than there is (0xFFFF, or 512): the last packet is full, so a ZLP must end the stage.
  for (const std::uint16_t want : {std::uint16_t{385}, std::uint16_t{512}, std::uint16_t{0xFFFF}}) {
    Played r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, want));
    CHECK(r.length == 384u);
    CHECK(r.packets == 7u);
    CHECK(r.ended_with_zlp);
    CHECK(r.toggles_alternate);  // the ZLP continues the alternation: D1 D0 D1 D0 D1 D0 then D1
    CHECK(std::memcmp(r.bytes, f.conf, 384) == 0);
  }
  // Exactly what is there: six packets, no ZLP.
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 384));
  CHECK((r.length == 384u && r.packets == 6u && !r.ended_with_zlp));
  // A shorter request that is a multiple of 64 is satisfied without a ZLP too (reply_len == wLength).
  r = play_in(p, setup(p, 0x80, 0x06, 0x0200, 0, 256));
  CHECK((r.length == 256u && r.packets == 4u && !r.ended_with_zlp));
}

TEST_CASE("usb control: string descriptors") {
  Fixture& f = fixture();
  ControlPipe& p = f.pipe;
  for (const std::uint16_t lang : {std::uint16_t{0x0409}, std::uint16_t{0}}) {
    Played r = play_in(p, setup(p, 0x80, 0x06, 0x0300, lang, 255));  // index 0: the language list
    CHECK((r.length == 4u && r.bytes[0] == 4 && r.bytes[1] == 3 && rd16(r.bytes + 2) == 0x0409));
  }
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0302, 0x0409, 255));  // iProduct
  CHECK(r.length == 2u + 2u * 7u);
  CHECK(r.bytes[2] == 'T');
  CHECK(r.bytes[3] == 0);
  CHECK(r.bytes[14] == 'r');
  r = play_in(p, setup(p, 0x80, 0x06, 0x0303, 0x0409, 255));  // iSerialNumber: the placeholder until the platform passes the real one
  CHECK(r.length == 2u + 2u * 12u);
  // Strings the mode does not have, and a language we did not list, stall.
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0306, 0x0409, 255)));  // the audio function's name in STANDARD
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x03FF, 0x0409, 255)));
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0302, 0x0407, 255)));  // German
}

TEST_CASE("usb control: descriptors this device does not have stall") {
  ControlPipe& p = fixture().pipe;
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0600, 0, 10)));  // device qualifier: a full-speed-only device answers with a request error
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0700, 0, 255)));  // other-speed configuration
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0F00, 0, 255)));  // BOS (bcdUSB is 2.00)
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0201, 0, 255)));  // configuration index 1
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x0101, 0, 18)));   // device descriptor index 1
  CHECK(is_stall(setup(p, 0x80, 0x06, 0x2200, 0, 255)));  // a HID report descriptor
  CHECK(is_stall(setup(p, 0x00, 0x07, 0x0100, 0, 18)));   // SET_DESCRIPTOR
  CHECK(is_stall(setup(p, 0x00, 0x06, 0x0100, 0, 18)));   // GET_DESCRIPTOR with the direction bit wrong
  CHECK(is_stall(setup(p, 0x81, 0x06, 0x0100, 0, 18)));   // addressed to an interface
  CHECK(p.counters().stalls == 9u);
  CHECK(p.counters().last_stall.request_type == 0x81);
}

TEST_CASE("usb control: SET_ADDRESS takes effect after its status stage") {
  ControlPipe& p = fixture().pipe;
  CHECK(p.state() == DeviceState::Default);
  ControlStep s = setup(p, 0x00, 0x05, 9, 0, 0);
  CHECK(is_in(s));
  CHECK((s.len == 0u && s.data1));              // the status stage is a zero-length DATA1 packet
  CHECK(s.effect == Effect::None);              // not yet
  CHECK(p.state() == DeviceState::Default);
  CHECK(p.address() == 0u);
  s = p.on_in_done();                           // the host acknowledged it
  CHECK(s.kind == ControlStep::Kind::None);
  CHECK(s.effect == Effect::SetAddress);
  CHECK(s.arg == 9u);
  CHECK(p.state() == DeviceState::Address);
  CHECK(p.address() == 9u);
  // Address 0 from the Address state is the Default state again.
  CHECK(is_in(setup(p, 0x00, 0x05, 0, 0, 0)));
  s = p.on_in_done();
  CHECK((s.effect == Effect::SetAddress && s.arg == 0u));
  CHECK(p.state() == DeviceState::Default);
}

TEST_CASE("usb control: SET_ADDRESS refusals, and a status stage that never completes") {
  ControlPipe& p = fixture().pipe;
  CHECK(is_stall(setup(p, 0x00, 0x05, 128, 0, 0)));  // 7 bits
  CHECK(is_stall(setup(p, 0x00, 0x05, 5, 1, 0)));    // wIndex must be 0
  CHECK(is_stall(setup(p, 0x00, 0x05, 5, 0, 2)));    // wLength must be 0
  CHECK(is_stall(setup(p, 0x80, 0x05, 5, 0, 0)));    // direction
  CHECK(is_stall(setup(p, 0x01, 0x05, 5, 0, 0)));    // recipient
  // A new SETUP in place of the completion abandons the change: the address stays.
  CHECK(is_in(setup(p, 0x00, 0x05, 6, 0, 0)));
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0100, 0, 18));
  CHECK(r.length == 18u);
  CHECK(p.address() == 0u);
  CHECK(p.on_in_done().effect == Effect::None);
  // In the Configured state SET_ADDRESS is not allowed.
  configure(p);
  CHECK(is_stall(setup(p, 0x00, 0x05, 4, 0, 0)));
  CHECK(p.address() == 3u);
}

TEST_CASE("usb control: SET_CONFIGURATION and GET_CONFIGURATION by state") {
  ControlPipe& p = fixture().pipe;
  // Default state: neither is defined, both stall.
  CHECK(is_stall(setup(p, 0x00, 0x09, 1, 0, 0)));
  CHECK(is_stall(setup(p, 0x80, 0x08, 0, 0, 1)));
  CHECK(is_in(setup(p, 0x00, 0x05, 2, 0, 0)));
  (void)p.on_in_done();
  // Address state: configuration 0, and a refusal of everything but 0 and 1.
  Played r = play_in(p, setup(p, 0x80, 0x08, 0, 0, 1));
  CHECK((r.length == 1u && r.bytes[0] == 0));
  CHECK(is_stall(setup(p, 0x00, 0x09, 2, 0, 0)));
  CHECK(is_stall(setup(p, 0x00, 0x09, 0x0101, 0, 0)));  // high byte must be 0
  CHECK(is_stall(setup(p, 0x00, 0x09, 1, 1, 0)));
  CHECK(is_stall(setup(p, 0x00, 0x09, 1, 0, 1)));
  CHECK(is_stall(setup(p, 0x80, 0x08, 0, 0, 2)));       // GET_CONFIGURATION wLength must be 1
  ControlStep s = setup(p, 0x00, 0x09, 1, 0, 0);
  CHECK(is_in(s));
  CHECK((s.effect == Effect::Configure && s.arg == 1u));  // the driver configures before the status packet goes out
  CHECK((s.len == 0u && s.data1));
  CHECK(p.state() == DeviceState::Configured);
  CHECK(p.configuration() == 1u);
  (void)p.on_in_done();
  r = play_in(p, setup(p, 0x80, 0x08, 0, 0, 1));
  CHECK((r.length == 1u && r.bytes[0] == 1));
  // Configuring again is a configuration event, and configuration 0 un-configures.
  s = setup(p, 0x00, 0x09, 1, 0, 0);
  CHECK((s.effect == Effect::Configure && s.arg == 1u));
  (void)p.on_in_done();
  s = setup(p, 0x00, 0x09, 0, 0, 0);
  CHECK((s.effect == Effect::Configure && s.arg == 0u));
  CHECK(p.state() == DeviceState::Address);
  CHECK(p.counters().configurations == 3u);
}

TEST_CASE("usb control: GET_STATUS for the device, interfaces and endpoints") {
  ControlPipe& p = fixture().pipe;
  Played r = play_in(p, setup(p, 0x80, 0x00, 0, 0, 2));  // device: bus powered, no remote wakeup
  CHECK((r.length == 2u && r.bytes[0] == 0 && r.bytes[1] == 0));
  // Endpoint 0 is reachable in the Address state; the others and the interfaces only once configured.
  CHECK(is_stall(setup(p, 0x82, 0x00, 0, 0x00, 2)));  // Default: not even endpoint 0
  CHECK(is_in(setup(p, 0x00, 0x05, 3, 0, 0)));
  (void)p.on_in_done();
  r = play_in(p, setup(p, 0x82, 0x00, 0, 0x80, 2));
  CHECK((r.length == 2u && r.bytes[0] == 0));
  CHECK(is_stall(setup(p, 0x82, 0x00, 0, 0x82, 2)));
  CHECK(is_stall(setup(p, 0x81, 0x00, 0, 0, 2)));
  configure(p);
  for (unsigned interface = 0; interface < 4u; ++interface) {
    r = play_in(p, setup(p, 0x81, 0x00, 0, static_cast<std::uint16_t>(interface), 2));
    CHECK((r.length == 2u && r.bytes[0] == 0 && r.bytes[1] == 0));
  }
  CHECK(is_stall(setup(p, 0x81, 0x00, 0, 4, 2)));      // STANDARD has four interfaces
  CHECK(is_stall(setup(p, 0x81, 0x00, 0, 0x0100, 2)));  // high byte of wIndex must be 0
  for (const std::uint8_t ep : {std::uint8_t{0x81}, std::uint8_t{0x02}, std::uint8_t{0x82}, std::uint8_t{0x03}, std::uint8_t{0x83}}) {
    r = play_in(p, setup(p, 0x82, 0x00, 0, ep, 2));
    CHECK((r.length == 2u && r.bytes[0] == 0));
  }
  CHECK(is_stall(setup(p, 0x82, 0x00, 0, 0x04, 2)));  // endpoint 4 is AUDIO's
  CHECK(is_stall(setup(p, 0x82, 0x00, 0, 0x8F, 2)));
  CHECK(is_stall(setup(p, 0x82, 0x00, 1, 0x82, 2)));  // wValue must be 0
  CHECK(is_stall(setup(p, 0x02, 0x00, 0, 0x82, 2)));  // direction
  CHECK(is_stall(setup(p, 0x83, 0x00, 0, 0x82, 2)));  // recipient "other"
  // wLength 1 gets one byte.
  r = play_in(p, setup(p, 0x80, 0x00, 0, 0, 1));
  CHECK(r.length == 1u);
}

// OS-USBRET: GET_STATUS and ENDPOINT_HALT follow the endpoints the descriptors declare. With USB RETURN off the return's OUT endpoint (0x04) and the feedback endpoint (0x85)
// do not exist, so a host that asks about them gets a stall; with it on they answer (and, being isochronous, cannot be halted).
TEST_CASE("usb control: endpoints a USB RETURN off set does not declare stall, with and without the return") {
  for (const bool usb_return : {false, true}) {
    ControlPipe& p = fixture(Mode::Audio, usb_return).pipe;
    configure(p);
    INFO("return " << usb_return);
    for (const std::uint8_t ep : {std::uint8_t{0x81}, std::uint8_t{0x02}, std::uint8_t{0x82}, std::uint8_t{0x03}, std::uint8_t{0x83}, layout::kEpAudioIn}) {
      const Played r = play_in(p, setup(p, 0x82, 0x00, 0, ep, 2));
      CHECK((r.length == 2u && r.bytes[0] == 0));  // CDC, MIDI and the stems IN endpoint exist in both
    }
    for (const std::uint8_t ep : {layout::kEpAudioOut, layout::kEpAudioFeedback}) {
      if (usb_return) {
        const Played r = play_in(p, setup(p, 0x82, 0x00, 0, ep, 2));
        CHECK((r.length == 2u && r.bytes[0] == 0));
      } else {
        CHECK(is_stall(setup(p, 0x82, 0x00, 0, ep, 2)));  // not declared: no GET_STATUS answer
      }
      // Isochronous endpoints take no ENDPOINT_HALT in either case; the missing ones are refused for being missing.
      CHECK(is_stall(setup(p, 0x02, 0x03, 0, ep, 0)));
      CHECK(is_stall(setup(p, 0x02, 0x01, 0, ep, 0)));
    }
  }
}

TEST_CASE("usb control: ENDPOINT_HALT on the bulk and interrupt endpoints") {
  ControlPipe& p = fixture().pipe;
  CHECK(is_stall(setup(p, 0x02, 0x03, 0, 0x82, 0)));  // not configured yet
  configure(p);
  ControlStep s = setup(p, 0x02, 0x03, 0, 0x82, 0);   // SET_FEATURE(ENDPOINT_HALT), CDC bulk IN
  CHECK(is_in(s));
  CHECK((s.effect == Effect::Halt && s.arg == 0x82u));
  CHECK((s.len == 0u && s.data1));
  CHECK(p.halted(0x82));
  CHECK(p.halted(0x02));  // one stall bit per endpoint number on this chip: both directions
  CHECK_FALSE(p.halted(0x83));
  (void)p.on_in_done();
  Played r = play_in(p, setup(p, 0x82, 0x00, 0, 0x82, 2));
  CHECK((r.length == 2u && r.bytes[0] == 1 && r.bytes[1] == 0));  // GET_STATUS shows the halt
  s = setup(p, 0x02, 0x01, 0, 0x02, 0);  // CLEAR_FEATURE from the other direction's address clears the number
  CHECK((s.effect == Effect::ClearHalt && s.arg == 0x02u));
  CHECK_FALSE(p.halted(0x82));
  (void)p.on_in_done();
  // Clearing a halt that was not set still resets the toggle: it is an effect.
  s = setup(p, 0x02, 0x01, 0, 0x83, 0);
  CHECK(s.effect == Effect::ClearHalt);
  (void)p.on_in_done();
  // The interrupt endpoint is allowed too.
  s = setup(p, 0x02, 0x03, 0, 0x81, 0);
  CHECK((s.effect == Effect::Halt && s.arg == 0x81u));
  (void)p.on_in_done();
  // Not allowed: endpoint 0, an endpoint that does not exist, the device and interface recipients (DEVICE_REMOTE_WAKEUP, TEST_MODE),
  // a value other than ENDPOINT_HALT, a data stage, the wrong direction.
  CHECK(is_stall(setup(p, 0x02, 0x03, 0, 0x00, 0)));
  CHECK(is_stall(setup(p, 0x02, 0x03, 0, 0x80, 0)));
  CHECK(is_stall(setup(p, 0x02, 0x03, 0, 0x05, 0)));
  CHECK(is_stall(setup(p, 0x00, 0x03, 1, 0, 0)));    // DEVICE_REMOTE_WAKEUP: not advertised
  CHECK(is_stall(setup(p, 0x00, 0x03, 2, 0x0100, 0)));  // TEST_MODE: high speed only
  CHECK(is_stall(setup(p, 0x00, 0x01, 1, 0, 0)));
  CHECK(is_stall(setup(p, 0x01, 0x03, 0, 0, 0)));    // an interface has no features
  CHECK(is_stall(setup(p, 0x02, 0x03, 1, 0x82, 0)));
  CHECK(is_stall(setup(p, 0x02, 0x03, 0, 0x82, 1)));
  CHECK(is_stall(setup(p, 0x82, 0x03, 0, 0x82, 0)));
  // A configuration event clears every halt.
  CHECK(setup(p, 0x02, 0x03, 0, 0x83, 0).effect == Effect::Halt);
  (void)p.on_in_done();
  CHECK(p.halted(0x83));
  CHECK(setup(p, 0x00, 0x09, 1, 0, 0).effect == Effect::Configure);
  CHECK_FALSE(p.halted(0x83));
}

TEST_CASE("usb control: GET_INTERFACE and SET_INTERFACE in STANDARD mode") {
  ControlPipe& p = fixture().pipe;
  CHECK(is_stall(setup(p, 0x81, 0x0A, 0, 0, 1)));  // not configured
  configure(p);
  for (unsigned interface = 0; interface < 4u; ++interface) {
    Played r = play_in(p, setup(p, 0x81, 0x0A, 0, static_cast<std::uint16_t>(interface), 1));
    CHECK((r.length == 1u && r.bytes[0] == 0));
    ControlStep s = setup(p, 0x01, 0x0B, 0, static_cast<std::uint16_t>(interface), 0);  // alternate setting 0: always valid
    CHECK(is_in(s));
    CHECK(s.effect == Effect::None);
    (void)p.on_in_done();
  }
  CHECK(is_stall(setup(p, 0x01, 0x0B, 1, 1, 0)));   // no interface here has an alternate setting 1
  CHECK(is_stall(setup(p, 0x01, 0x0B, 0, 4, 0)));   // no interface 4
  CHECK(is_stall(setup(p, 0x81, 0x0A, 0, 4, 1)));
  CHECK(is_stall(setup(p, 0x81, 0x0A, 1, 0, 1)));   // wValue must be 0
  CHECK(is_stall(setup(p, 0x81, 0x0B, 0, 0, 0)));   // SET_INTERFACE with the device-to-host direction
  CHECK(is_stall(setup(p, 0x01, 0x0A, 0, 0, 1)));   // GET_INTERFACE with the host-to-device direction
  CHECK(is_stall(setup(p, 0x01, 0x0B, 0, 0, 1)));   // SET_INTERFACE with a data stage
  CHECK(is_stall(setup(p, 0x00, 0x0C, 0, 0, 2)));   // SYNCH_FRAME
}

TEST_CASE("usb control: AUDIO mode alternate settings and the UAC2 clock requests") {
  ControlPipe& p = fixture(Mode::Audio).pipe;
  configure(p);
  // GET_INTERFACE / SET_INTERFACE on the two AudioStreaming interfaces.
  Played r = play_in(p, setup(p, 0x81, 0x0A, 0, layout::kIfAudioIn, 1));
  CHECK(r.bytes[0] == 0);
  ControlStep s = setup(p, 0x01, 0x0B, 1, layout::kIfAudioIn, 0);
  CHECK(is_in(s));
  CHECK((s.effect == Effect::SetInterface && s.arg == layout::kIfAudioIn));
  CHECK(p.audio().in_active());
  CHECK_FALSE(p.audio().out_active());
  (void)p.on_in_done();
  r = play_in(p, setup(p, 0x81, 0x0A, 0, layout::kIfAudioIn, 1));
  CHECK(r.bytes[0] == 1);
  CHECK(is_stall(setup(p, 0x01, 0x0B, 2, layout::kIfAudioOut, 0)));  // alternate 2 does not exist
  CHECK(is_in(setup(p, 0x01, 0x0B, 1, layout::kIfAudioOut, 0)));
  (void)p.on_in_done();
  CHECK(p.audio().out_active());
  // Seven interfaces in AUDIO mode: 0,1 CDC, 2 UAC2 AC, 3,4 AS, 5,6 MIDI. The MIDI interfaces have the one alternate setting.
  CHECK(is_in(setup(p, 0x01, 0x0B, 0, layout::if_midi_control(Mode::Audio), 0)));
  (void)p.on_in_done();
  CHECK(is_stall(setup(p, 0x01, 0x0B, 1, layout::if_midi_stream(Mode::Audio), 0)));
  CHECK(is_in(setup(p, 0x01, 0x0B, 0, 6, 0)));
  (void)p.on_in_done();
  CHECK(is_stall(setup(p, 0x01, 0x0B, 0, 7, 0)));
  // The clock source: GET CUR is 44,100 Hz, GET RANGE one discrete rate.
  const std::uint16_t clock_index = static_cast<std::uint16_t>((layout::kEntClock << 8) | layout::kIfAudioControl);
  r = play_in(p, setup(p, 0xA1, 0x01, 0x0100, clock_index, 4));
  CHECK((r.length == 4u && rd16(r.bytes) == 44100u && rd16(r.bytes + 2) == 0u));
  r = play_in(p, setup(p, 0xA1, 0x02, 0x0100, clock_index, 14));
  CHECK((r.length == 14u && r.bytes[0] == 1));
  CHECK(is_stall(setup(p, 0x21, 0x01, 0x0100, clock_index, 4)));  // SET CUR: read-only control
  // The clock request is routed by the interface in wIndex: the MIDI function (5/6 in AUDIO mode, 2/3 in STANDARD) and the
  // AudioStreaming interfaces answer none of it.
  for (std::uint8_t other : {layout::if_midi_control(Mode::Audio), layout::if_midi_stream(Mode::Audio), layout::kIfAudioIn,
                             layout::kIfAudioOut, layout::kIfCdcData}) {
    CHECK(is_stall(setup(p, 0xA1, 0x01, 0x0100, static_cast<std::uint16_t>((layout::kEntClock << 8) | other), 4)));
  }
  CHECK(is_stall(setup(p, 0xA1, 0x01, 0x0200, clock_index, 4)));  // the clock validity selector is not implemented
  // A configuration event and a bus reset put the alternate settings back to 0.
  CHECK(setup(p, 0x00, 0x09, 1, 0, 0).effect == Effect::Configure);
  CHECK_FALSE(p.audio().in_active());
  CHECK(is_in(setup(p, 0x01, 0x0B, 1, layout::kIfAudioIn, 0)));
  p.bus_reset();
  CHECK_FALSE(p.audio().in_active());
  // STANDARD mode answers none of it.
  ControlPipe& q = fixture().pipe;
  configure(q);
  CHECK(is_stall(setup(q, 0xA1, 0x01, 0x0100, clock_index, 4)));
}

// OS-USBRET: with USB RETURN off interface 4 is the MIDI AudioControl and 5 the MIDIStreaming one, and there is no 6: a host that still asks for the return's alternate setting
// gets a stall, and the stems stream is unaffected.
TEST_CASE("usb control: AUDIO mode without USB RETURN - six interfaces, the return's alternate setting does not exist") {
  ControlPipe& p = fixture(Mode::Audio, false).pipe;
  configure(p);
  Played r = play_in(p, setup(p, 0x81, 0x0A, 0, layout::kIfAudioIn, 1));
  CHECK(r.bytes[0] == 0);
  ControlStep s = setup(p, 0x01, 0x0B, 1, layout::kIfAudioIn, 0);
  CHECK(is_in(s));
  CHECK((s.effect == Effect::SetInterface && s.arg == layout::kIfAudioIn));
  CHECK(p.audio().in_active());
  (void)p.on_in_done();
  // Interface 4 is the MIDI AudioControl: alternate 0 only. It is never an AudioStreaming interface, so it never becomes the return stream.
  CHECK(layout::if_midi_control(Mode::Audio, false) == layout::kIfAudioOut);
  CHECK(is_stall(setup(p, 0x01, 0x0B, 1, layout::kIfAudioOut, 0)));
  CHECK_FALSE(p.audio().out_active());
  s = setup(p, 0x01, 0x0B, 0, layout::kIfAudioOut, 0);
  CHECK(is_in(s));
  CHECK(s.effect == Effect::None);  // (no stream to stop)
  (void)p.on_in_done();
  r = play_in(p, setup(p, 0x81, 0x0A, 0, layout::kIfAudioOut, 1));
  CHECK(r.bytes[0] == 0);
  CHECK(is_stall(setup(p, 0x01, 0x0B, 1, layout::if_midi_stream(Mode::Audio, false), 0)));
  CHECK(is_in(setup(p, 0x01, 0x0B, 0, 5, 0)));
  (void)p.on_in_done();
  CHECK(is_stall(setup(p, 0x01, 0x0B, 0, 6, 0)));  // seven interfaces with the return, six without
  CHECK(is_stall(setup(p, 0x81, 0x0A, 0, 6, 1)));
  // The clock requests are routed by the interface as before; the MIDI function (4/5) answers none of it.
  const std::uint16_t clock_index = static_cast<std::uint16_t>((layout::kEntClock << 8) | layout::kIfAudioControl);
  r = play_in(p, setup(p, 0xA1, 0x01, 0x0100, clock_index, 4));
  CHECK((r.length == 4u && rd16(r.bytes) == 44100u));
  for (std::uint8_t other : {layout::if_midi_control(Mode::Audio, false), layout::if_midi_stream(Mode::Audio, false), layout::kIfAudioIn}) {
    CHECK(is_stall(setup(p, 0xA1, 0x01, 0x0100, static_cast<std::uint16_t>((layout::kEntClock << 8) | other), 4)));
  }
}

TEST_CASE("usb control: CDC line coding and control line state") {
  ControlPipe& p = fixture().pipe;
  CHECK(is_stall(setup(p, 0xA1, 0x21, 0, 0, 7)));  // class requests need a configured device
  configure(p);
  // Before any SET: 115200 8N1.
  Played r = play_in(p, setup(p, 0xA1, 0x21, 0, 0, 7));
  CHECK(r.length == 7u);
  const std::uint8_t def[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};
  CHECK(std::memcmp(r.bytes, def, 7) == 0);
  // SET_LINE_CODING: SETUP, one OUT packet of 7 bytes, then a zero-length IN status.
  ControlStep s = setup(p, 0x21, 0x20, 0, 0, 7);
  CHECK(s.kind == ControlStep::Kind::None);  // the receive descriptors are armed already
  const std::uint8_t coding[7] = {0x80, 0x25, 0x00, 0x00, 0x02, 0x01, 0x07};  // 9600, two stop bits, odd parity, 7 data bits
  s = p.on_out_done(coding, 7);
  CHECK(is_in(s));
  CHECK((s.len == 0u && s.data1));
  CHECK(p.cdc().coding.dte_rate == 9600u);
  CHECK((p.cdc().coding.char_format == 2 && p.cdc().coding.parity_type == 1 && p.cdc().coding.data_bits == 7));
  CHECK(p.cdc().coding_sets == 1u);
  (void)p.on_in_done();
  r = play_in(p, setup(p, 0xA1, 0x21, 0, 0, 7));
  CHECK(std::memcmp(r.bytes, coding, 7) == 0);  // GET returns what was set
  // A shorter GET gets a shorter reply.
  r = play_in(p, setup(p, 0xA1, 0x21, 0, 0, 4));
  CHECK(r.length == 4u);
  // SET_CONTROL_LINE_STATE: DTR and RTS.
  CHECK_FALSE(p.cdc().dtr());
  s = setup(p, 0x21, 0x22, 0x0003, 0, 0);
  CHECK(is_in(s));
  (void)p.on_in_done();
  CHECK((p.cdc().dtr() && p.cdc().rts()));
  CHECK(is_in(setup(p, 0x21, 0x22, 0x0001, 0, 0)));
  (void)p.on_in_done();
  CHECK((p.cdc().dtr() && !p.cdc().rts()));
  CHECK(is_in(setup(p, 0x21, 0x22, 0xFFF0, 0, 0)));  // the bits above 1 are not ours
  (void)p.on_in_done();
  CHECK_FALSE(p.cdc().dtr());
  CHECK(p.cdc().control_sets == 3u);
  // Refusals: a data stage of the wrong length, SEND_BREAK, the wrong interface, the wrong direction.
  CHECK(is_stall(setup(p, 0x21, 0x20, 0, 0, 6)));
  CHECK(is_stall(setup(p, 0x21, 0x20, 0, 0, 8)));
  CHECK(is_stall(setup(p, 0xA1, 0x20, 0, 0, 7)));
  CHECK(is_stall(setup(p, 0x21, 0x23, 0, 0, 0)));  // SEND_BREAK is not advertised
  CHECK(is_stall(setup(p, 0xA1, 0x21, 0, 1, 7)));  // the data interface has no class requests
  CHECK(is_stall(setup(p, 0xA1, 0x21, 0, 2, 7)));  // MIDI streaming neither
  CHECK(is_stall(setup(p, 0xA1, 0x21, 0, 0x0100, 7)));
  CHECK(is_stall(setup(p, 0x21, 0x22, 3, 0, 1)));
  CHECK(is_stall(setup(p, 0x22, 0x22, 3, 0, 0)));  // addressed to an endpoint
  CHECK(is_stall(setup(p, 0x41, 0x22, 3, 0, 0)));  // a vendor request
  CHECK(is_stall(setup(p, 0x21, 0x99, 0, 0, 0)));
  // An OUT data stage of the wrong size (the host sends 5 bytes where 7 were announced) stalls and is counted.
  (void)setup(p, 0x21, 0x20, 0, 0, 7);
  CHECK(is_stall(p.on_out_done(coding, 5)));
  CHECK(p.counters().out_stage_errors == 1u);
  CHECK(p.cdc().coding_sets == 1u);  // nothing was stored
  // A bus reset hangs the terminal up.
  CHECK(is_in(setup(p, 0x21, 0x22, 0x0003, 0, 0)));
  p.bus_reset();
  CHECK_FALSE(p.cdc().dtr());
}

TEST_CASE("usb control: a new SETUP ends the transfer in progress, an early status ends a data stage") {
  ControlPipe& p = fixture().pipe;
  ControlStep s = setup(p, 0x80, 0x06, 0x0200, 0, 255);
  CHECK((is_in(s) && s.len == 64u));
  // The host abandons the read and starts a new request.
  Played r = play_in(p, setup(p, 0x80, 0x06, 0x0100, 0, 18));
  CHECK((r.length == 18u && r.first_is_data1));
  // The host ends a data stage early with its status OUT: the rest is dropped and nothing more is sent.
  s = setup(p, 0x80, 0x06, 0x0200, 0, 255);
  CHECK(is_in(s));
  s = p.on_out_done(nullptr, 0);
  CHECK(s.kind == ControlStep::Kind::None);
  CHECK(p.on_in_done().kind == ControlStep::Kind::None);  // a stale completion
  // Completions nobody is waiting for are ignored.
  CHECK(p.on_in_done().kind == ControlStep::Kind::None);
  CHECK(p.on_out_done(nullptr, 0).kind == ControlStep::Kind::None);
}

TEST_CASE("usb control: stall paths are counted and remembered") {
  ControlPipe& p = fixture().pipe;
  CHECK(p.counters().stalls == 0u);
  CHECK(is_stall(setup(p, 0x40, 0x01, 0x1234, 0x5678, 9)));  // a vendor request
  CHECK(p.counters().stalls == 1u);
  const SetupPacket last = p.counters().last_stall;
  CHECK((last.request_type == 0x40 && last.request == 1 && last.value == 0x1234 && last.index == 0x5678 && last.length == 9));
  CHECK(is_stall(setup(p, 0x60, 0x00, 0, 0, 0)));  // reserved type
  CHECK(is_stall(setup(p, 0x00, 0xFF, 0, 0, 0)));  // unknown standard request
  CHECK(p.counters().stalls == 3u);
  CHECK(p.counters().setups == 3u);
}

TEST_CASE("usb control: bus reset, suspend and resume") {
  ControlPipe& p = fixture().pipe;
  configure(p);
  CHECK(is_in(setup(p, 0x21, 0x22, 3, 0, 0)));
  (void)p.on_in_done();
  CHECK(is_in(setup(p, 0x02, 0x03, 0, 0x82, 0)));
  (void)p.on_in_done();
  CHECK_FALSE(p.suspended());
  p.on_suspend();
  CHECK(p.suspended());
  CHECK(p.state() == DeviceState::Configured);  // suspend is not a state change: configuration survives
  CHECK(p.address() == 3u);
  p.on_resume();
  CHECK_FALSE(p.suspended());
  p.on_suspend();
  p.bus_reset();  // a reset ends suspend too and returns to Default
  CHECK_FALSE(p.suspended());
  CHECK(p.state() == DeviceState::Default);
  CHECK(p.address() == 0u);
  CHECK(p.configuration() == 0u);
  CHECK_FALSE(p.halted(0x82));
  CHECK_FALSE(p.cdc().dtr());
  CHECK(p.counters().suspends == 2u);
  CHECK(p.counters().resumes == 1u);
  CHECK(p.counters().bus_resets == 1u);
  // A transfer in progress at the reset is gone.
  (void)setup(p, 0x80, 0x06, 0x0200, 0, 255);
  p.bus_reset();
  CHECK(p.on_in_done().kind == ControlStep::Kind::None);
}
