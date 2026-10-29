// SPDX-License-Identifier: GPL-3.0-or-later
// The kit's settings file (app/settings.h, docs/formats/o3sdk-settings.md): the bytes it writes, the values it reads back, and what it does with a file that is not exactly
// its own: an unknown key, a newer minor version, a newer major version, a value out of range, a chunk it does not know, a damaged container. After any refusal the
// values are untouched.
#include <doctest/doctest.h>

#include <o3sdk/app/settings.h>
#include <o3sdk/formats/chunk_file.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace o3sdk;
namespace chunk = formats::chunk;
using app::KitValues;
namespace st = app::settings;

namespace {

KitValues non_default() {
  KitValues v;
  v.theme = ui::ThemeId::White;
  v.midi.channel = 7;
  v.midi.out_channel = 12;
  v.midi.out_ports = core::kMidiUsb;
  v.midi.clock_out = false;
  v.midi.follow = true;
  v.midi.follow_port = hal::midi::Port::Usb;
  v.midi.transport_in = false;
  v.midi.transport_out = false;
  v.in_source = 4;  // M2c slice AI: the audio in and the radio (mic high, 40 %, monitor 70, Japan, 82.10 MHz)
  v.in_gain = 40;
  v.in_monitor = 70;
  v.radio_region = 3;
  v.radio_freq = 8210;
  v.usb_audio = false;  // M2c slice U: USB audio (STANDARD, P10, no return, level 35, the first slots edited)
  v.usb_profile = 2;
  v.usb_duplex = false;
  v.usb_level = 35;
  v.usb_map_set = true;
  v.usb_stem[0] = 0x0003;
  v.usb_stem[10] = 0xF000;
  return v;
}

std::vector<std::uint8_t> file_of(const KitValues& v) {
  std::vector<std::uint8_t> buf(st::kMaxBytes);
  const std::size_t n = st::write(v, buf.data(), buf.size());
  REQUIRE(n != 0);
  buf.resize(n);
  return buf;
}

// A file with a SETS chunk of the given version and entries (key, value), then the given extra chunks written by `more`.
template <typename More>
std::vector<std::uint8_t> sets_file(std::uint16_t version, const std::vector<std::pair<std::uint16_t, float>>& entries, std::size_t extra_per_entry, More more) {
  std::vector<std::uint8_t> buf(512);
  chunk::Writer w(buf.data(), buf.size(), "O3SK");
  REQUIRE(w.begin("SETS"));
  REQUIRE(w.u16(version));
  REQUIRE(w.u16(static_cast<std::uint16_t>(entries.size())));
  for (const auto& e : entries) {
    REQUIRE(w.u16(e.first));
    REQUIRE(w.f32(e.second));
  }
  const std::uint8_t pad[8] = {};
  REQUIRE(w.bytes(pad, extra_per_entry));
  REQUIRE(w.end());
  more(w);
  const std::size_t n = w.finish();
  REQUIRE(n != 0);
  buf.resize(n);
  return buf;
}

std::vector<std::uint8_t> sets_file(std::uint16_t version, const std::vector<std::pair<std::uint16_t, float>>& entries) {
  return sets_file(version, entries, 0, [](chunk::Writer&) {});
}

}  // namespace

TEST_CASE("settings: the keys are the documented ones, in order, and never move") {
  // Appended, never inserted or reused: these numbers are on every card that ever held a system.o3s.
  CHECK(st::kTheme == 1);
  CHECK(st::kMidiChannel == 2);
  CHECK(st::kMidiOutChannel == 3);
  CHECK(st::kMidiOutPorts == 4);
  CHECK(st::kMidiClockOut == 5);
  CHECK(st::kMidiFollow == 6);
  CHECK(st::kMidiFollowPort == 7);
  CHECK(st::kMidiTransportIn == 8);
  CHECK(st::kMidiTransportOut == 9);
  CHECK(st::kInputSource == 10);  // slice AI
  CHECK(st::kInputGain == 11);
  CHECK(st::kInputMonitor == 12);
  CHECK(st::kRadioRegion == 13);
  CHECK(st::kRadioFreq == 14);
  CHECK(st::kUsbAudio == 15);  // slice U
  CHECK(st::kUsbProfile == 16);
  CHECK(st::kUsbDuplex == 17);
  CHECK(st::kUsbReturnLevel == 18);
  CHECK(st::kUsbMapSet == 19);
  CHECK(st::kUsbStem1 == 20);
  CHECK(st::kUsbStem11 == 30);
  CHECK(st::kBrightness == 31);  // slice CP
  CHECK(st::kPadLights == 32);
  CHECK(st::kKeyCount == 32);
  CHECK(st::kSetsVersion == 0x0100);
}

TEST_CASE("settings: the file of the defaults is the documented bytes") {
  const std::vector<std::uint8_t> f = file_of(KitValues{});
  // FORM, length, O3SK, SETS 4 + 32 * 6 = 196 bytes (even: no pad), CRC. No APPV or LRNB: nothing to say.
  REQUIRE(f.size() == 12 + 8 + 196 + 14);
  CHECK(std::memcmp(f.data(), "FORM", 4) == 0);
  CHECK(std::memcmp(f.data() + 8, "O3SK", 4) == 0);
  CHECK(std::memcmp(f.data() + 12, "SETS", 4) == 0);
  CHECK(f[16] == 0);
  CHECK(f[19] == 196);
  CHECK(f[20] == 0x01);  // version 1.0
  CHECK(f[21] == 0x00);
  CHECK(f[22] == 0x00);  // thirty-two entries
  CHECK(f[23] == 0x20);
  // Entry 1: key 1 (theme), the value 0.0f; entry 2: key 2 (channel, omni 0); entry 3: key 3, out channel 1.0f = 0x3F800000.
  CHECK(f[24] == 0);
  CHECK(f[25] == 1);
  CHECK(f[30] == 0);
  CHECK(f[31] == 2);
  CHECK(f[36] == 0);
  CHECK(f[37] == 3);
  CHECK(f[38] == 0x3F);
  CHECK(f[39] == 0x80);
  CHECK(f.size() <= st::kMaxBytes);
}

TEST_CASE("settings: every setting round trips, the defaults and a file of changed values") {
  for (const KitValues& v : {KitValues{}, non_default()}) {
    const std::vector<std::uint8_t> f = file_of(v);
    KitValues back = v.theme == ui::ThemeId::Amiga ? non_default() : KitValues{};  // (the other set: every field must be written by the file)
    st::Report rep;
    REQUIRE(st::read(f.data(), f.size(), back, rep) == st::Result::Ok);
    CHECK(back == v);
    CHECK(rep.applied == 32);
    CHECK(rep.skipped_keys == 0);
    CHECK(rep.skipped_values == 0);
    CHECK(rep.unknown_chunks == 0);
    CHECK_FALSE(rep.newer_minor);
  }
  CHECK(KitValues{} != non_default());
}

TEST_CASE("settings: a key the file lacks keeps the value the app set, one it has wins") {
  KitValues v = non_default();
  v.midi.channel = 3;
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kTheme, 1.0f}});  // an older file: the theme only
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(v.theme == ui::ThemeId::Black);
  CHECK(v.midi.channel == 3);
  CHECK(v.midi.out_channel == 12);
  CHECK(rep.applied == 1);
}

TEST_CASE("settings: a key this version does not know is skipped and counted, the rest is read") {
  KitValues v;
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kMidiChannel, 5.0f}, {99, 1.0f}, {st::kTheme, 2.0f}, {33, 0.0f}, {0, 1.0f}});
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(v.midi.channel == 5);
  CHECK(v.theme == ui::ThemeId::White);
  CHECK(rep.applied == 2);
  CHECK(rep.skipped_keys == 3);  // 99, 33 (the next key of a newer firmware) and 0
}

TEST_CASE("settings: a newer minor version is read as far as this version knows it") {
  KitValues v;
  // Version 1.3 with entries a later firmware appended (key 33, 34) and two extra bytes at the end of the chunk.
  const std::vector<std::uint8_t> f = sets_file(0x0103, {{st::kTheme, 1.0f}, {st::kMidiOutChannel, 9.0f}, {33, 4.0f}, {34, 1.0f}}, 2, [](chunk::Writer&) {});
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(rep.newer_minor);
  CHECK(v.theme == ui::ThemeId::Black);
  CHECK(v.midi.out_channel == 9);
  CHECK(rep.applied == 2);
  CHECK(rep.skipped_keys == 2);
  // The same chunk of this version's minor with extra bytes is malformed: nothing says what they are.
  const std::vector<std::uint8_t> g = sets_file(0x0100, {{st::kTheme, 1.0f}}, 2, [](chunk::Writer&) {});
  KitValues w;
  CHECK(st::read(g.data(), g.size(), w, rep) == st::Result::Malformed);
}

TEST_CASE("settings: a newer major version is refused whole and the values are untouched") {
  KitValues v = non_default();
  const KitValues before = v;
  const std::vector<std::uint8_t> f = sets_file(0x0200, {{st::kTheme, 0.0f}, {st::kMidiChannel, 0.0f}});
  st::Report rep;
  CHECK(st::read(f.data(), f.size(), v, rep) == st::Result::NewerMajor);
  CHECK(v == before);
  const std::vector<std::uint8_t> zero = sets_file(0x0000, {{st::kTheme, 0.0f}});  // major 0 never existed
  CHECK(st::read(zero.data(), zero.size(), v, rep) == st::Result::Malformed);
  CHECK(v == before);
}

TEST_CASE("settings: a value outside its setting's range is skipped and counted, the setting keeps its value") {
  KitValues v = non_default();
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kTheme, 3.0f},              // there are three themes: 0..2
                                                          {st::kMidiChannel, 17.0f},       // 0..16
                                                          {st::kMidiOutChannel, 0.0f},     // 1..16
                                                          {st::kMidiOutPorts, 4.0f},       // a mask of two ports
                                                          {st::kMidiClockOut, 2.0f},       // 0 or 1
                                                          {st::kMidiFollowPort, 2.0f},     // two ports
                                                          {st::kMidiFollow, 0.5f},         // not a whole number
                                                          {st::kMidiTransportIn, std::numeric_limits<float>::quiet_NaN()},
                                                          {st::kMidiTransportOut, -1.0f}});
  st::Report rep;
  const KitValues before = v;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(v == before);
  CHECK(rep.skipped_values == 9);
  CHECK(rep.applied == 0);
}

TEST_CASE("settings: a chunk this version does not know is skipped and counted; a second SETS is malformed") {
  KitValues v;
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kTheme, 1.0f}}, 0, [](chunk::Writer& w) {
    REQUIRE(w.begin("APPV"));  // reserved: the app's page values
    REQUIRE(w.u16(0x0100));
    REQUIRE(w.end());
    REQUIRE(w.begin("LRNB"));  // reserved: the learn bindings
    REQUIRE(w.u8(1));
    REQUIRE(w.end());
  });
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(rep.unknown_chunks == 2);
  CHECK(v.theme == ui::ThemeId::Black);

  const std::vector<std::uint8_t> twice = sets_file(0x0100, {{st::kTheme, 1.0f}}, 0, [](chunk::Writer& w) {
    REQUIRE(w.begin("SETS"));
    REQUIRE(w.u16(0x0100));
    REQUIRE(w.u16(0));
    REQUIRE(w.end());
  });
  KitValues w;
  CHECK(st::read(twice.data(), twice.size(), w, rep) == st::Result::Malformed);
  CHECK(w == KitValues{});
  // A count that does not fit the length.
  std::vector<std::uint8_t> lie(256);
  chunk::Writer lw(lie.data(), lie.size(), "O3SK");
  REQUIRE(lw.begin("SETS"));
  REQUIRE(lw.u16(0x0100));
  REQUIRE(lw.u16(5));  // five entries claimed
  REQUIRE(lw.u16(st::kTheme));
  REQUIRE(lw.f32(1.0f));  // one given
  REQUIRE(lw.end());
  lie.resize(lw.finish());
  CHECK(st::read(lie.data(), lie.size(), w, rep) == st::Result::Malformed);
  // A file with no SETS at all is a file of defaults: Ok, nothing applied.
  std::vector<std::uint8_t> none(128);
  chunk::Writer nw(none.data(), none.size(), "O3SK");
  none.resize(nw.finish());
  KitValues x = non_default();
  REQUIRE(st::read(none.data(), none.size(), x, rep) == st::Result::Ok);
  CHECK(x == non_default());
  CHECK(rep.applied == 0);
}

TEST_CASE("settings: a damaged file is refused at every byte and every cut, and the values stay") {
  const KitValues v = non_default();
  const std::vector<std::uint8_t> f = file_of(v);
  const std::size_t minor = f.size() - 14 + 9;  // the trailer's minor version: nothing checks it, a newer one is read
  for (std::size_t i = 0; i < f.size(); ++i) {
    if (i == minor) {
      continue;
    }
    std::vector<std::uint8_t> g = f;
    g[i] ^= 0x10;
    KitValues out;
    st::Report rep;
    CAPTURE(i);
    CHECK(st::read(g.data(), g.size(), out, rep) != st::Result::Ok);
    CHECK(out == KitValues{});
  }
  for (std::size_t n = 0; n < f.size(); ++n) {
    KitValues out;
    st::Report rep;
    CAPTURE(n);
    CHECK(st::read(f.data(), n, out, rep) != st::Result::Ok);
    CHECK(out == KitValues{});
  }
  KitValues out;
  st::Report rep;
  CHECK(st::read(nullptr, 0, out, rep) == st::Result::Truncated);
  // Another type of file is not ours.
  std::vector<std::uint8_t> other(64);
  chunk::Writer ow(other.data(), other.size(), "HRIF");
  other.resize(ow.finish());
  CHECK(st::read(other.data(), other.size(), out, rep) == st::Result::BadMagic);
}

TEST_CASE("settings: a buffer that is too small writes nothing") {
  std::uint8_t tiny[40];
  CHECK(st::write(non_default(), tiny, sizeof tiny) == 0);
  std::uint8_t exact[12 + 8 + 196 + 14];
  CHECK(st::write(non_default(), exact, sizeof exact) == sizeof exact);
  CHECK(st::write(non_default(), nullptr, 0) == 0);
}

// ---- M2c slice CP: brightness and pad lights (keys 31 and 32), the app's page values (APPV) and the learn bindings (LRNB) -----------------------------------------------------------------

namespace {

core::LearnBinding binding(unsigned unit, unsigned field, unsigned cc, unsigned channel, core::LearnMode mode) {
  core::LearnBinding b;
  b.param = core::param_id(core::ParamKind::Unit, unit, field);
  b.cc = static_cast<std::uint8_t>(cc);
  b.channel = static_cast<std::uint8_t>(channel);
  b.mode = static_cast<std::uint8_t>(mode);
  b.flags = 0;
  return b;
}

KitValues with_extras() {
  KitValues v = non_default();
  v.brightness = 0;
  v.pad_lights = 1;
  v.app_count = 3;
  v.app_key[0] = 1;
  v.app_value[0] = 122.0f;
  v.app_key[1] = 40000;
  v.app_value[1] = -7.5f;
  v.app_key[2] = 3;
  v.app_value[2] = 0.0f;
  v.learn[0] = binding(0, 1, 74, 1, core::LearnMode::Pickup);
  v.learn[1] = binding(1, 300, 11, 0, core::LearnMode::Relative);  // (a read gives the bindings back in order, with no gaps)
  return v;
}

}  // namespace

TEST_CASE("settings: brightness and pad lights are keys 31 and 32, 0 to 2, and a value past that is skipped") {
  KitValues v;
  CHECK(v.brightness == 2);  // high, the hardware's own, is the default
  CHECK(v.pad_lights == 2);
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kBrightness, 0.0f}, {st::kPadLights, 1.0f}});
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(v.brightness == 0);
  CHECK(v.pad_lights == 1);
  const std::vector<std::uint8_t> bad = sets_file(0x0100, {{st::kBrightness, 3.0f}, {st::kPadLights, -1.0f}});
  KitValues w;
  REQUIRE(st::read(bad.data(), bad.size(), w, rep) == st::Result::Ok);
  CHECK(rep.skipped_values == 2);
  CHECK(w.brightness == 2);
  CHECK(w.pad_lights == 2);
}

TEST_CASE("settings: the page values and the learn bindings are written as APPV and LRNB and read back, and a file of the defaults has neither") {
  const std::vector<std::uint8_t> plain = file_of(KitValues{});
  CHECK(std::search(plain.begin(), plain.end(), "APPV", "APPV" + 4) == plain.end());
  CHECK(std::search(plain.begin(), plain.end(), "LRNB", "LRNB" + 4) == plain.end());
  const KitValues v = with_extras();
  const std::vector<std::uint8_t> f = file_of(v);
  CHECK(std::search(f.begin(), f.end(), "APPV", "APPV" + 4) != f.end());
  CHECK(std::search(f.begin(), f.end(), "LRNB", "LRNB" + 4) != f.end());
  CHECK(f.size() <= st::kMaxBytes);
  KitValues back;
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), back, rep) == st::Result::Ok);
  CHECK(back == v);
  CHECK(back.app_count == 3);
  CHECK(back.app_key[1] == 40000);
  CHECK(back.app_value[1] == -7.5f);
  CHECK(core::param_index(back.learn[1].param) == 1u);
  CHECK(core::param_field(back.learn[1].param) == 300u);
  CHECK(back.learn[1].mode == static_cast<std::uint8_t>(core::LearnMode::Relative));
  CHECK(rep.unknown_chunks == 0);
  CHECK(rep.skipped_values == 0);
  // The values are part of the comparison that decides whether the settings are due: a change of one is a change.
  KitValues other = v;
  other.app_value[2] = 1.0f;
  CHECK(other != v);
  other = v;
  other.learn[0].cc = 75;
  CHECK(other != v);
  other = v;
  other.brightness = 1;
  CHECK(other != v);
}

TEST_CASE("settings: a binding that is not valid for a unit's parameter, or listens to a message another has, is dropped; a chunk this version cannot read is counted") {
  KitValues v;
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kTheme, 1.0f}}, 0, [](chunk::Writer& w) {
    REQUIRE(w.begin("LRNB"));
    REQUIRE(w.u16(0x0100));
    REQUIRE(w.u16(5));
    const auto put = [&](core::ParamId id, unsigned cc, unsigned channel, unsigned mode, unsigned flags) {
      REQUIRE(w.u32(static_cast<std::uint32_t>(id)));
      REQUIRE(w.u8(static_cast<std::uint8_t>(cc)));
      REQUIRE(w.u8(static_cast<std::uint8_t>(channel)));
      REQUIRE(w.u8(static_cast<std::uint8_t>(mode)));
      REQUIRE(w.u8(static_cast<std::uint8_t>(flags)));
    };
    put(core::param_id(core::ParamKind::Unit, 0, 2), 74, 1, 0, 0);        // good
    put(core::param_id(core::ParamKind::Instrument, 0, 2), 80, 1, 0, 0);  // a kind that is not a unit's: dropped
    put(core::param_id(core::ParamKind::Unit, 0, 3), 200, 1, 0, 0);       // a controller past 127
    put(core::param_id(core::ParamKind::Unit, 0, 4), 74, 1, 1, 0);        // the same controller on the same channel: takes it from the first
    put(core::param_id(core::ParamKind::Unit, 0, 5), 12, 0, 9, 0);        // an unknown mode
    REQUIRE(w.end());
    REQUIRE(w.begin("APPV"));
    REQUIRE(w.u16(0x0200));  // a newer major version
    REQUIRE(w.u16(0));
    REQUIRE(w.end());
  });
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  unsigned bound = 0;
  for (const core::LearnBinding& b : v.learn) {
    bound += core::learn_empty(b) ? 0u : 1u;
  }
  CHECK(bound == 1u);  // only the fourth survives: it displaced the first
  CHECK(core::param_field(v.learn[0].param) == 4u);
  CHECK(rep.skipped_values == 3);
  CHECK(rep.unknown_chunks == 1);  // the APPV of major 2
  CHECK(v.app_count == 0);
  // A second APPV is skipped like any other it does not expect.
  const std::vector<std::uint8_t> twice = sets_file(0x0100, {{st::kTheme, 1.0f}}, 0, [](chunk::Writer& w) {
    for (unsigned i = 0; i < 2; ++i) {
      REQUIRE(w.begin("APPV"));
      REQUIRE(w.u16(0x0100));
      REQUIRE(w.u16(1));
      REQUIRE(w.u16(static_cast<std::uint16_t>(7 + i)));
      REQUIRE(w.f32(1.0f));
      REQUIRE(w.end());
    }
  });
  KitValues t;
  REQUIRE(st::read(twice.data(), twice.size(), t, rep) == st::Result::Ok);
  CHECK(t.app_count == 1);
  CHECK(t.app_key[0] == 7);
  CHECK(rep.unknown_chunks == 1);
  // A NaN or a key of 0 is no value of an item.
  const std::vector<std::uint8_t> nan = sets_file(0x0100, {{st::kTheme, 1.0f}}, 0, [](chunk::Writer& w) {
    REQUIRE(w.begin("APPV"));
    REQUIRE(w.u16(0x0100));
    REQUIRE(w.u16(3));
    REQUIRE(w.u16(5));
    REQUIRE(w.f32(std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(w.u16(0));
    REQUIRE(w.f32(1.0f));
    REQUIRE(w.u16(6));
    REQUIRE(w.f32(2.0f));
    REQUIRE(w.end());
  });
  KitValues n;
  REQUIRE(st::read(nan.data(), nan.size(), n, rep) == st::Result::Ok);
  CHECK(n.app_count == 1);
  CHECK(n.app_key[0] == 6);
}
