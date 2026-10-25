// SPDX-License-Identifier: GPL-3.0-or-later
//
// The injection commands of the diag console (OS-HIL, usb/hil_inject.h): the argument grammar of `key`, `pad` and `enc`, and the injector that gives a tap or a
// hold its release: what is pushed when, what is refused whole, and that nothing at all goes out while `allowed` is false. Runs on the host and on QEMU's
// Cortex-M4F (`qemu_mk66`).
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/usb/hil_inject.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace console = o3sdk::platform::mk66::console;
namespace core = o3sdk::core;

namespace {

struct Pushed {
  core::InputKind kind;
  std::uint8_t id;
  std::int16_t value;
  bool operator==(const Pushed& o) const { return kind == o.kind && id == o.id && value == o.value; }
};

struct Sink {
  std::vector<Pushed> events;
  unsigned room = 1000;  // pushes the queue takes before it says full
  static bool push(void* context, core::InputKind kind, std::uint8_t id, std::int16_t value) {
    Sink& s = *static_cast<Sink*>(context);
    if (s.room == 0) {
      return false;
    }
    --s.room;
    s.events.push_back(Pushed{kind, id, value});
    return true;
  }
};

int fake_names(const char* name) {
  if (std::strcmp(name, "play") == 0) {
    return 23;
  }
  if (std::strcmp(name, "f1") == 0) {
    return 0;
  }
  if (std::strcmp(name, "7") == 0) {
    return 7;
  }
  return -1;
}

console::InjectParse key(std::vector<const char*> argv, console::InjectRequest& r) {
  return console::parse_key(static_cast<unsigned>(argv.size()), argv.data(), &fake_names, r);
}
console::InjectParse pad(std::vector<const char*> argv, console::InjectRequest& r) {
  return console::parse_pad(static_cast<unsigned>(argv.size()), argv.data(), r);
}
console::InjectParse enc(std::vector<const char*> argv, console::InjectRequest& r) {
  return console::parse_enc(static_cast<unsigned>(argv.size()), argv.data(), r);
}

}  // namespace

TEST_CASE("inject: key grammar") {
  console::InjectRequest r;
  CHECK(key({"key", "play"}, r) == console::InjectParse::Ok);
  CHECK(r.kind == core::InputKind::Button);
  CHECK(r.id == 23);
  CHECK(r.gesture == console::InjectGesture::Tap);
  CHECK(key({"key", "f1", "down"}, r) == console::InjectParse::Ok);
  CHECK(r.gesture == console::InjectGesture::Down);
  CHECK(key({"key", "f1", "up"}, r) == console::InjectParse::Ok);
  CHECK(r.gesture == console::InjectGesture::Up);
  CHECK(key({"key", "7", "tap"}, r) == console::InjectParse::Ok);
  CHECK(r.id == 7);
  CHECK(key({"key", "play", "hold", "500"}, r) == console::InjectParse::Ok);
  CHECK(r.gesture == console::InjectGesture::Hold);
  CHECK(r.hold_ms == 500u);
  CHECK(key({"key", "play", "hold", "10000"}, r) == console::InjectParse::Ok);
  CHECK(r.hold_ms == 10000u);
  // what is not the grammar is a usage, what has the grammar but names nothing is Unknown
  CHECK(key({"key"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "tap", "x"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "hold"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "hold", "0"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "hold", "10001"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "hold", "abc"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "hold", "-5"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "long"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "play", "down", "5"}, r) == console::InjectParse::Usage);
  CHECK(key({"key", "bogus"}, r) == console::InjectParse::Unknown);
  CHECK(key({"key", "bogus", "hold", "10"}, r) == console::InjectParse::Unknown);
  const char* one[] = {"key", "play"};
  CHECK(console::parse_key(2, one, nullptr, r) == console::InjectParse::Usage);  // no name table
}

TEST_CASE("inject: pad grammar") {
  console::InjectRequest r;
  CHECK(pad({"pad", "0"}, r) == console::InjectParse::Ok);
  CHECK(r.kind == core::InputKind::Pad);
  CHECK(r.id == 0);
  CHECK(r.value == 127);
  CHECK(r.gesture == console::InjectGesture::Tap);
  CHECK(pad({"pad", "47", "down"}, r) == console::InjectParse::Ok);
  CHECK(r.id == 47);
  CHECK(r.gesture == console::InjectGesture::Down);
  CHECK(r.value == 127);
  CHECK(pad({"pad", "5", "up"}, r) == console::InjectParse::Ok);
  CHECK(r.gesture == console::InjectGesture::Up);
  CHECK(pad({"pad", "5", "tap", "90"}, r) == console::InjectParse::Ok);
  CHECK(r.value == 90);
  CHECK(pad({"pad", "5", "down", "1"}, r) == console::InjectParse::Ok);
  CHECK(r.value == 1);
  CHECK(pad({"pad", "5", "90"}, r) == console::InjectParse::Ok);  // a bare number is a tap's velocity
  CHECK(r.gesture == console::InjectGesture::Tap);
  CHECK(r.value == 90);
  CHECK(pad({"pad", "48"}, r) == console::InjectParse::Unknown);
  CHECK(pad({"pad", "999"}, r) == console::InjectParse::Unknown);
  CHECK(pad({"pad"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "x"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "-1"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "5", "tap", "0"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "5", "tap", "128"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "5", "up", "90"}, r) == console::InjectParse::Usage);  // a release has no velocity
  CHECK(pad({"pad", "5", "hold"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "5", "tap", "90", "1"}, r) == console::InjectParse::Usage);
  CHECK(pad({"pad", "5", "0"}, r) == console::InjectParse::Usage);
}

TEST_CASE("inject: enc grammar") {
  console::InjectRequest r;
  CHECK(enc({"enc", "3"}, r) == console::InjectParse::Ok);
  CHECK(r.kind == core::InputKind::Encoder);
  CHECK(r.id == 0);
  CHECK(r.value == 3);
  CHECK(enc({"enc", "+5"}, r) == console::InjectParse::Ok);
  CHECK(r.value == 5);
  CHECK(enc({"enc", "-1"}, r) == console::InjectParse::Ok);
  CHECK(r.value == -1);
  CHECK(enc({"enc", "-64"}, r) == console::InjectParse::Ok);
  CHECK(r.value == -64);
  CHECK(enc({"enc", "64"}, r) == console::InjectParse::Ok);
  CHECK(enc({"enc", "65"}, r) == console::InjectParse::Unknown);
  CHECK(enc({"enc", "-65"}, r) == console::InjectParse::Unknown);
  CHECK(enc({"enc", "0"}, r) == console::InjectParse::Unknown);
  CHECK(enc({"enc", "-0"}, r) == console::InjectParse::Unknown);
  CHECK(enc({"enc"}, r) == console::InjectParse::Usage);
  CHECK(enc({"enc", "1", "2"}, r) == console::InjectParse::Usage);
  CHECK(enc({"enc", "x"}, r) == console::InjectParse::Usage);
  CHECK(enc({"enc", "--1"}, r) == console::InjectParse::Usage);
  CHECK(enc({"enc", "+"}, r) == console::InjectParse::Usage);
  CHECK(enc({"enc", "99999999999"}, r) == console::InjectParse::Usage);
}

// OS-AUDIT-28: `enc accel [off | <t1> <t2> [gap_ms]]`. The ranges are app/enc_accel.h EncAccel::valid (mirrored here: the usb layer does not see the app).
TEST_CASE("enc accel: grammar and ranges") {
  auto accel = [](std::vector<const char*> argv, console::EncAccelRequest& q) {
    return console::parse_enc_accel(static_cast<unsigned>(argv.size()), argv.data(), q);
  };
  console::EncAccelRequest q;
  CHECK(accel({"enc", "accel"}, q) == console::InjectParse::Ok);
  CHECK_FALSE(q.set);  // a query
  CHECK(accel({"enc", "accel", "off"}, q) == console::InjectParse::Ok);
  CHECK(q.set);
  CHECK_FALSE(q.on);
  CHECK(accel({"enc", "accel", "6", "16"}, q) == console::InjectParse::Ok);
  CHECK(q.set);
  CHECK(q.on);
  CHECK(q.t1 == 6);
  CHECK(q.t2 == 16);
  CHECK(q.gap_ms == 60);  // the default gap
  CHECK(accel({"enc", "accel", "1", "255", "250"}, q) == console::InjectParse::Ok);
  CHECK(q.t1 == 1);
  CHECK(q.t2 == 255);
  CHECK(q.gap_ms == 250);
  CHECK(accel({"enc", "accel", "3", "3", "5"}, q) == console::InjectParse::Ok);
  CHECK(q.gap_ms == 5);
  CHECK(accel({"enc", "accel", "0", "5"}, q) == console::InjectParse::Unknown);
  CHECK(accel({"enc", "accel", "8", "4"}, q) == console::InjectParse::Unknown);
  CHECK(accel({"enc", "accel", "4", "256"}, q) == console::InjectParse::Unknown);
  CHECK(accel({"enc", "accel", "4", "8", "4"}, q) == console::InjectParse::Unknown);
  CHECK(accel({"enc", "accel", "4", "8", "251"}, q) == console::InjectParse::Unknown);
  CHECK(accel({"enc", "accel", "4"}, q) == console::InjectParse::Usage);
  CHECK(accel({"enc", "accel", "x", "8"}, q) == console::InjectParse::Usage);
  CHECK(accel({"enc", "accel", "off", "1"}, q) == console::InjectParse::Usage);
  CHECK(accel({"enc", "accel", "4", "8", "60", "1"}, q) == console::InjectParse::Usage);
  CHECK(accel({"enc", "5"}, q) == console::InjectParse::Usage);
}

TEST_CASE("injector: a tap is down now and up kTapMs later, a hold when it says, down and up are one event") {
  console::Injector inj;
  Sink s;
  console::InjectRequest tap;
  tap.id = 23;
  CHECK(inj.inject(tap, 1000, &Sink::push, &s) == console::Injector::Result::Ok);
  CHECK(s.events == std::vector<Pushed>{{core::InputKind::Button, 23, 1}});
  CHECK(inj.pending() == 1u);
  inj.service(1000 + console::kInjectTapMs - 1, true, &Sink::push, &s);
  CHECK(s.events.size() == 1u);
  inj.service(1000 + console::kInjectTapMs, true, &Sink::push, &s);
  CHECK(s.events.size() == 2u);
  CHECK(s.events[1] == Pushed{core::InputKind::Button, 23, 0});
  CHECK(inj.pending() == 0u);
  inj.service(100000, true, &Sink::push, &s);  // nothing left to say
  CHECK(s.events.size() == 2u);

  s.events.clear();
  console::InjectRequest hold;
  hold.id = 0;
  hold.gesture = console::InjectGesture::Hold;
  hold.hold_ms = 700;
  CHECK(inj.inject(hold, 5000, &Sink::push, &s) == console::Injector::Result::Ok);
  inj.service(5699, true, &Sink::push, &s);
  CHECK(s.events.size() == 1u);
  inj.service(5700, true, &Sink::push, &s);
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Button, 0, 1}, {core::InputKind::Button, 0, 0}}));

  s.events.clear();
  console::InjectRequest down;
  down.gesture = console::InjectGesture::Down;
  down.id = 29;
  console::InjectRequest up = down;
  up.gesture = console::InjectGesture::Up;
  CHECK(inj.inject(down, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  CHECK(inj.inject(up, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  CHECK(inj.pending() == 0u);  // nothing is owed
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Button, 29, 1}, {core::InputKind::Button, 29, 0}}));
}

TEST_CASE("injector: pads carry their velocity down and 0 up; an encoder turn is one event and owes nothing") {
  console::Injector inj;
  Sink s;
  console::InjectRequest p;
  p.kind = core::InputKind::Pad;
  p.id = 12;
  p.value = 90;
  CHECK(inj.inject(p, 10, &Sink::push, &s) == console::Injector::Result::Ok);
  inj.service(10 + console::kInjectTapMs, true, &Sink::push, &s);
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Pad, 12, 90}, {core::InputKind::Pad, 12, 0}}));
  s.events.clear();
  p.gesture = console::InjectGesture::Up;
  CHECK(inj.inject(p, 10, &Sink::push, &s) == console::Injector::Result::Ok);
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Pad, 12, 0}}));
  s.events.clear();
  console::InjectRequest e;
  e.kind = core::InputKind::Encoder;
  e.value = -7;
  e.gesture = console::InjectGesture::Hold;  // a gesture means nothing to a knob
  e.hold_ms = 50;
  CHECK(inj.inject(e, 10, &Sink::push, &s) == console::Injector::Result::Ok);
  CHECK(inj.pending() == 0u);
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Encoder, 0, -7}}));
}

TEST_CASE("injector: a full table or a full queue refuses the request whole") {
  console::Injector inj;
  Sink s;
  console::InjectRequest hold;
  hold.gesture = console::InjectGesture::Hold;
  hold.hold_ms = 5000;
  for (unsigned i = 0; i < console::Injector::kCapacity; ++i) {
    hold.id = static_cast<std::uint8_t>(i);
    CHECK(inj.inject(hold, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  }
  CHECK(inj.pending() == console::Injector::kCapacity);
  const std::size_t before = s.events.size();
  hold.id = 20;
  CHECK(inj.inject(hold, 0, &Sink::push, &s) == console::Injector::Result::NoRoom);
  CHECK(s.events.size() == before);  // not even the press went out: no key is left held without a release
  console::InjectRequest down;  // a request that owes no release still goes
  down.gesture = console::InjectGesture::Down;
  down.id = 21;
  CHECK(inj.inject(down, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  // the queue full: nothing is filed
  console::Injector inj2;
  Sink full;
  full.room = 0;
  hold.id = 1;
  CHECK(inj2.inject(hold, 0, &Sink::push, &full) == console::Injector::Result::QueueFull);
  CHECK(inj2.pending() == 0u);
  CHECK(full.events.empty());
}

TEST_CASE("injector: a release the queue refuses waits, nothing goes while it is not allowed, and a release that came due goes when it is") {
  console::Injector inj;
  Sink s;
  console::InjectRequest tap;
  tap.id = 3;
  CHECK(inj.inject(tap, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  console::InjectRequest tap2;
  tap2.id = 4;
  CHECK(inj.inject(tap2, 0, &Sink::push, &s) == console::Injector::Result::Ok);
  s.events.clear();
  // an fw update is armed: nothing is injected, however late
  inj.service(500, false, &Sink::push, &s);
  CHECK(s.events.empty());
  CHECK(inj.pending() == 2u);
  // allowed again, the queue takes one: the first release goes, the other waits for the next call
  s.room = 1;
  inj.service(501, true, &Sink::push, &s);
  CHECK(s.events == (std::vector<Pushed>{{core::InputKind::Button, 3, 0}}));
  CHECK(inj.pending() == 1u);
  s.room = 5;
  inj.service(502, true, &Sink::push, &s);
  CHECK(s.events.size() == 2u);
  CHECK(s.events[1] == Pushed{core::InputKind::Button, 4, 0});
  CHECK(inj.pending() == 0u);
}

TEST_CASE("injector: the millisecond clock may wrap while a release waits") {
  console::Injector inj;
  Sink s;
  console::InjectRequest hold;
  hold.id = 9;
  hold.gesture = console::InjectGesture::Hold;
  hold.hold_ms = 100;
  const std::uint32_t start = 0xFFFFFFF0u;
  CHECK(inj.inject(hold, start, &Sink::push, &s) == console::Injector::Result::Ok);  // due at 0x54
  inj.service(0xFFFFFFFFu, true, &Sink::push, &s);
  CHECK(s.events.size() == 1u);
  inj.service(0x53u, true, &Sink::push, &s);
  CHECK(s.events.size() == 1u);
  inj.service(0x54u, true, &Sink::push, &s);
  CHECK(s.events.size() == 2u);
}
