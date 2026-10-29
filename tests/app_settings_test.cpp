// SPDX-License-Identifier: GPL-3.0-or-later
// The kit's settings file (app/settings.h, docs/formats/o3sdk-settings.md): the bytes it writes, the values it reads back, and what it does with a file that is not exactly
// its own: an unknown key, a newer minor version, a newer major version, a value out of range, a chunk it does not know, a damaged container. After any refusal the
// values are untouched.
#include <doctest/doctest.h>

#include <o3sdk/app/settings.h>
#include <o3sdk/formats/chunk_file.h>

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
  CHECK(st::kKeyCount == 14);
  CHECK(st::kSetsVersion == 0x0100);
}

TEST_CASE("settings: the file of the defaults is the documented bytes") {
  const std::vector<std::uint8_t> f = file_of(KitValues{});
  // FORM, length, O3SK, SETS 4 + 14 * 6 = 88 bytes (even: no pad), CRC.
  REQUIRE(f.size() == 12 + 8 + 88 + 14);
  CHECK(std::memcmp(f.data(), "FORM", 4) == 0);
  CHECK(std::memcmp(f.data() + 8, "O3SK", 4) == 0);
  CHECK(std::memcmp(f.data() + 12, "SETS", 4) == 0);
  CHECK(f[16] == 0);
  CHECK(f[19] == 88);
  CHECK(f[20] == 0x01);  // version 1.0
  CHECK(f[21] == 0x00);
  CHECK(f[22] == 0x00);  // fourteen entries
  CHECK(f[23] == 0x0E);
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
    CHECK(rep.applied == 14);
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
  const std::vector<std::uint8_t> f = sets_file(0x0100, {{st::kMidiChannel, 5.0f}, {99, 1.0f}, {st::kTheme, 2.0f}, {15, 0.0f}, {0, 1.0f}});
  st::Report rep;
  REQUIRE(st::read(f.data(), f.size(), v, rep) == st::Result::Ok);
  CHECK(v.midi.channel == 5);
  CHECK(v.theme == ui::ThemeId::White);
  CHECK(rep.applied == 2);
  CHECK(rep.skipped_keys == 3);  // 99, 15 (the next key of a newer firmware) and 0
}

TEST_CASE("settings: a newer minor version is read as far as this version knows it") {
  KitValues v;
  // Version 1.3 with entries a later firmware appended (key 15, 16) and two extra bytes at the end of the chunk.
  const std::vector<std::uint8_t> f = sets_file(0x0103, {{st::kTheme, 1.0f}, {st::kMidiOutChannel, 9.0f}, {15, 4.0f}, {16, 1.0f}}, 2, [](chunk::Writer&) {});
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
  std::uint8_t exact[12 + 8 + 88 + 14];
  CHECK(st::write(non_default(), exact, sizeof exact) == sizeof exact);
  CHECK(st::write(non_default(), nullptr, 0) == 0);
}
