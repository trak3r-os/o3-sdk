// SPDX-License-Identifier: GPL-3.0-or-later
//
// The USB audio of the image (docs/M2C-PLAN.md section 7, slice U; docs/M2B-PLAN.md slice D had the master-only tap this file used to test): the pure service (platform/mk66/audio
// usb_audio_service.h) over a REAL engine, the real stem tap and map, the real return ring and reader, and, at the end, the real usb::IsoAudio, the device logic and a fake controller
// with a host reading packets. Proved here: the boot profile P8 carries the main output's own samples on channels 7 and 8 and silence on 1..6 (the proven layout does not regress); a
// mapped slot is the engine's stem; the engine stages nothing while no host listens (and an idle service does no work: a count); the return is pulled whatever the level, mixed into the
// output with saturation after the stems have taken their copy, and is the engine's input only while the audio in's source is Usb. Pure logic and host fakes: the interrupt's real timing
// and the unit's cost are for the unit.
#include <doctest/doctest.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/usb_audio.h>
#include <o3sdk/platform/mk66/audio/usb_audio_service.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/iso_audio.h>
#include <o3sdk/usb/stream.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace au = o3sdk::platform::mk66::audio;
namespace ua = o3sdk::platform::mk66::usb_audio;
namespace hu = o3sdk::hal::usb_audio;
namespace haudio = o3sdk::hal::audio;
namespace dsp = o3sdk::dsp;
using namespace o3sdk::usb;
using namespace usb_rig;

namespace {

constexpr std::uint32_t kBlock = haudio::kBlock;

// The instrument of the tests: a sine plus an offset on each side, so that no two samples of the master are alike and silence is recognisable.
class Gen final : public dsp::Unit {
 public:
  Gen() : info_{"GEN", dsp::UnitKind::Instrument, nullptr, 0, nullptr, 0} {}
  const dsp::UnitInfo& info() const noexcept override { return info_; }
  void prepare(float) noexcept override { n_ = 0; }
  void process(const dsp::ProcessArgs& a) noexcept override {
    for (std::uint32_t i = 0; i < a.frames; ++i, ++n_) {
      a.out_l[i] = 0.1f + 0.4f * std::sin(0.07f * static_cast<float>(n_));
      a.out_r[i] = -0.1f + 0.4f * std::cos(0.05f * static_cast<float>(n_));
    }
  }

 private:
  dsp::UnitInfo info_;
  std::uint32_t n_ = 0;
};

// A port that only says what layout it is in (the tests drive the layout themselves).
class TestLink final : public au::UsbAudioService::Link {
 public:
  bool ready() const noexcept override { return true; }
  bool audio() const noexcept override { return audio_; }
  hu::Profile profile() const noexcept override { return profile_; }
  bool duplex() const noexcept override { return duplex_; }
  bool changing() const noexcept override { return false; }
  bool request(bool audio, hu::Profile profile, bool duplex) noexcept override {
    audio_ = audio;
    profile_ = profile;
    duplex_ = duplex;
    return true;
  }
  hu::DiskStatus disk() const noexcept override { return hu::DiskStatus{}; }
  hu::DiskRefusal disk_share(bool) noexcept override { return hu::DiskRefusal::Unavailable; }
  bool disk_eject() noexcept override { return false; }
  bool audio_ = true;
  hu::Profile profile_ = hu::Profile::P8;
  bool duplex_ = true;
};

// The service on a real engine with one instrument on slot 0. The "app" (inner) renders the engine into the block the main output plays and keeps what it made, the master before the return
// is added and slot 0's stem, block by block; `in_seen` is the codec block the app was given.
struct Fx {
  Fx() : tap(pool), svc(tap, map, link), reader(returns, counters), out(returns, counters) {
    const dsp::UnitId id = engine.attach(gen);
    ok = engine.set_source(0, id);
    engine.start();
    au::factory_map(map, o3sdk::usb::Profile::P8);  // what usb0's start does for the boot profile
    map.apply(tap);
    svc.attach_return(&reader);
    cb = svc.wrap(haudio::BlockCallback{&Fx::inner, this}, engine);
  }
  static void inner(void* user, const haudio::BlockIo& io) noexcept {
    Fx& f = *static_cast<Fx*>(user);
    f.in_seen.assign(io.in, io.in + haudio::kBlockSamples);
    float l[kBlock];
    float r[kBlock];
    f.engine.render(nullptr, nullptr, l, r, kBlock);
    dsp::planes_to_s16(l, r, io.out, kBlock);
    for (std::uint32_t i = 0; i < kBlock; ++i) {
      f.master_l.push_back(io.out[2u * i]);
      f.master_r.push_back(io.out[2u * i + 1u]);
    }
    const std::int16_t* const* t = f.engine.stem_block();
    for (std::uint32_t i = 0; i < kBlock; ++i) {
      f.stem0.push_back(t != nullptr && t[0] != nullptr ? t[0][i] : std::int16_t{0});
    }
    ++f.inner_calls;
  }
  // One block: `codec` is what the codec delivers (null: silence), `flags` as the pipeline gives them. Returns the block the main output plays.
  const std::int16_t* step(const std::int16_t* codec = nullptr, std::uint32_t flags = 0) {
    alignas(4) static const std::int16_t kQuiet[haudio::kBlockSamples] = {};
    const haudio::BlockIo io{codec != nullptr ? codec : kQuiet, play, flags, &report};
    cb.fn(cb.user, io);
    return play;
  }
  // The computer plays `blocks` blocks of a constant return (the OUT packets, 44-ish frames a packet as the host sends them).
  void host_plays(std::int16_t l, std::int16_t r, unsigned blocks) {
    for (unsigned b = 0; b < blocks; ++b) {
      alignas(4) std::uint32_t frames[kBlock];
      for (std::uint32_t i = 0; i < kBlock; ++i) {
        frames[i] = static_cast<std::uint16_t>(l) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(r)) << 16);
      }
      out.on_packet(frames, 44u * 4u);
      out.on_packet(frames + 44u, 20u * 4u);
    }
  }

  au::StemPool pool;
  au::StemTap tap;
  au::StemMap map;
  TestLink link;
  au::UsbAudioService svc;
  Counters counters;
  ReturnRing returns;
  ReturnReader reader;
  OutStream out;
  Gen gen;
  dsp::Engine engine;
  haudio::BlockCallback cb{nullptr, nullptr};
  haudio::BlockReport report{};
  alignas(4) std::int16_t play[haudio::kBlockSamples] = {};
  std::vector<std::int16_t> in_seen;
  std::vector<std::int16_t> master_l;
  std::vector<std::int16_t> master_r;
  std::vector<std::int16_t> stem0;
  unsigned inner_calls = 0;
  bool ok = false;
};

}  // namespace

TEST_CASE("usb audio: the profiles the image can feed are track3r-os's (every one but P4S), the HAL's four map onto them, the pool is the lifetime contract's and fits SRAM_L") {
  CHECK(ua::supported(Profile::P2));
  CHECK(ua::supported(Profile::P8));
  CHECK(ua::supported(Profile::P10));
  CHECK(ua::supported(Profile::P11));
  CHECK_FALSE(ua::supported(Profile::P4S));
  for (unsigned i = 0; i < hu::kProfileCount; ++i) {
    const hu::Profile p = static_cast<hu::Profile>(i);
    CHECK(au::from_usb(au::to_usb(p)) == p);
    CHECK(hu::profile_channels(p) == profile_channels(au::to_usb(p)));
  }
  static_assert(au::StemPool::kPool == StemRing::capacity() + 2u, "capacity() + 2 blocks a channel: ring content, the reader's block, the one being produced (usb/stream.h)");
  CHECK(au::StemPool::kPool == 5u);
  CHECK(sizeof(au::StemPool) < 16u * 1024u);  // about 15.8 KB: the budget of mem_map.h kSramLDataBudget is 20 KiB
}

TEST_CASE("usb audio: o3-sdk's factory map is StemMap's for every profile but P8, whose slots are empty (the boot profile streams the master and silence)") {
  au::StemMap m;
  for (const Profile p : {Profile::P2, Profile::P10, Profile::P11}) {
    au::StemMap plain;
    REQUIRE(plain.reset(p));
    REQUIRE(au::factory_map(m, p));
    for (unsigned c = 0; c < profile_channels(p); ++c) {
      CHECK(m.slot(c) == plain.slot(c));
    }
  }
  REQUIRE(au::factory_map(m, Profile::P8));
  for (unsigned c = 0; c < 6u; ++c) {
    CHECK(m.slot(c) == 0u);
  }
  CHECK(m.slot(6) == (1u << au::stem_src::kMasterL));
  CHECK(m.slot(7) == (1u << au::stem_src::kMasterR));
  CHECK_FALSE(au::factory_map(m, Profile::P4S));
  CHECK(m.profile() == Profile::P8);  // (nothing changed)
  REQUIRE(au::factory_map(m, Profile::P10));
  CHECK(m.slot(0) == 1u);  // P10: T1..T8, then the master
  CHECK(m.slot(8) == (1u << au::stem_src::kMasterL));
}

TEST_CASE("usb audio: a service with no stream and no Usb source does no work and the engine stages nothing (a count)") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  for (unsigned b = 0; b < 40; ++b) {
    f.step();
  }
  CHECK(f.inner_calls == 40u);
  CHECK(f.svc.active_blocks() == 0u);
  CHECK(f.svc.pulled_blocks() == 0u);
  CHECK(f.tap.published() == 0u);
  CHECK(f.engine.stem_slots() == 0u);
  CHECK(f.engine.feature_work().stem_frames == 0u);
  CHECK(f.in_seen.size() == haudio::kBlockSamples);
  // A race that left the engine's mask set while no host listens is put back by the next block, and nothing else is touched.
  f.engine.set_stem_slots(1u);
  f.step();
  CHECK(f.engine.stem_slots() == 0u);
}

TEST_CASE("usb audio: the boot profile P8 carries the main output's own samples on channels 7 and 8, silence on 1 to 6, and the engine stages only the master pair") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  for (unsigned b = 0; b < 6; ++b) {
    f.step();
  }
  const std::size_t first = f.master_l.size();  // the host opens the stream: the stem phase starts with the next block
  f.svc.streams_changed(true, false);
  CHECK(f.svc.streaming());
  CHECK_FALSE(f.svc.return_streaming());
  CHECK(f.engine.stem_slots() == ((1u << dsp::kStemMasterL) | (1u << dsp::kStemMasterR)));
  alignas(4) std::int16_t played[8][haudio::kBlockSamples];
  for (unsigned b = 0; b < 8; ++b) {
    std::memcpy(played[b], f.step(), sizeof played[b]);
  }
  CHECK(f.svc.active_blocks() == 8u);
  CHECK(f.tap.published() == 4u);
  StemReader reader(f.tap.ring());
  alignas(4) static std::int16_t got[3u * kStemBlockFrames * 8u];
  const std::uint32_t frames = 3u * kStemBlockFrames;  // the ring holds three of the four blocks
  REQUIRE(reader.read_interleaved(got, frames, 8) == frames);
  const std::uint32_t skip = kStemBlockFrames;  // the oldest block was overwritten
  bool exact = true;
  bool silent = true;
  for (std::uint32_t k = 0; k < frames; ++k) {
    const std::size_t at = first + skip + k;
    exact = exact && got[k * 8u + 6u] == f.master_l[at] && got[k * 8u + 7u] == f.master_r[at];
    for (unsigned c = 0; c < 6u; ++c) {
      silent = silent && got[k * 8u + c] == 0;
    }
  }
  CHECK(exact);
  CHECK(silent);
  CHECK(f.master_l[first + skip] != 0);  // (the master is a signal: silence would pass the test above)
  // The block the main output plays is as the app left it: the stems are a copy, the output untouched.
  bool untouched = true;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    untouched = untouched && played[7][2u * i] == f.master_l[first + 7u * kBlock + i] && played[7][2u * i + 1u] == f.master_r[first + 7u * kBlock + i];
  }
  CHECK(untouched);
  CHECK(f.engine.feature_work().stem_frames == 8u * 2u * kBlock);  // two sources, eight blocks

  // The host closes the stream: nothing is staged any more and the count stops.
  f.svc.streams_changed(false, false);
  CHECK(f.engine.stem_slots() == 0u);
  const std::uint32_t before = f.engine.feature_work().stem_frames;
  for (unsigned b = 0; b < 4; ++b) {
    f.step();
  }
  CHECK(f.engine.feature_work().stem_frames == before);
  CHECK(f.tap.published() == 4u);
}

TEST_CASE("usb audio: a mapped slot is the engine's stem of that slot, the mask follows the map, an edit outside the profile is refused, and reset gives the factory slots back") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  const std::size_t first = f.stem0.size();
  f.svc.streams_changed(true, false);
  CHECK_FALSE(f.svc.set_stem(8, 1u));  // P8 has eight channels
  CHECK(f.svc.set_stem(0, hu::track_source(1)));
  CHECK(f.svc.stem(0) == 1u);
  CHECK((f.engine.stem_slots() & 1u) == 1u);
  CHECK(f.svc.set_stem(1, hu::track_source(1) | hu::kSourceMasterL));  // a sum of two sources
  for (unsigned b = 0; b < 12; ++b) {
    f.step();
  }
  StemReader reader(f.tap.ring());
  alignas(4) static std::int16_t got[3u * kStemBlockFrames * 8u];
  const std::uint32_t frames = 3u * kStemBlockFrames;
  REQUIRE(reader.read_interleaved(got, frames, 8) == frames);
  bool exact = true;
  bool any = false;
  bool summed = true;
  const std::size_t start = first + (f.tap.published() - 3u) * kStemBlockFrames;  // the ring holds the last three stem blocks
  for (std::uint32_t k = 0; k < frames; ++k) {
    exact = exact && got[k * 8u + 0u] == f.stem0[start + k];
    any = any || got[k * 8u + 0u] != 0;
    const std::int32_t want = static_cast<std::int32_t>(f.stem0[start + k]) + static_cast<std::int32_t>(f.master_l[start + k]);
    summed = summed && got[k * 8u + 1u] == static_cast<std::int16_t>(want > 32767 ? 32767 : (want < -32768 ? -32768 : want));
  }
  CHECK(exact);
  CHECK(any);
  CHECK(summed);
  f.svc.reset_stems();
  CHECK(f.svc.stem(0) == 0u);
  CHECK(f.svc.stem(6) == (1u << au::stem_src::kMasterL));
  CHECK(f.engine.stem_slots() == ((1u << dsp::kStemMasterL) | (1u << dsp::kStemMasterR)));
}

TEST_CASE("usb audio: with a stream open and nothing staged (every slot empty) silence blocks keep the ring's pace") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  for (unsigned c = 0; c < 8u; ++c) {
    REQUIRE(f.svc.set_stem(c, 0u));
  }
  f.svc.streams_changed(true, false);
  for (unsigned b = 0; b < 6; ++b) {
    f.step();
  }
  CHECK(f.tap.published() == 3u);
  CHECK(f.engine.stem_slots() == 0u);
  StemReader reader(f.tap.ring());
  alignas(4) static std::int16_t got[3u * kStemBlockFrames * 8u];
  REQUIRE(reader.read_interleaved(got, 3u * kStemBlockFrames, 8) == 3u * kStemBlockFrames);
  bool silent = true;
  for (const std::int16_t s : got) {
    silent = silent && s == 0;
  }
  CHECK(silent);
}

TEST_CASE("usb audio: the return is pulled whatever the level, added after the render with saturation, never seen by a stem, and a silenced block gets none") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  const std::size_t first = f.master_l.size();
  f.svc.streams_changed(true, true);
  CHECK(f.svc.return_streaming());
  f.host_plays(1000, -2000, 4);  // the reader primes (256 frames) before it delivers audio
  f.svc.set_return_level(0.5f);
  CHECK(f.svc.return_level() == doctest::Approx(0.5f));
  // The first blocks prime and ramp; look at a settled one.
  for (unsigned b = 0; b < 6; ++b) {
    f.step();
    f.host_plays(1000, -2000, 1);
  }
  const std::size_t last = f.master_l.size() - kBlock;
  const std::int16_t* out = f.step();
  f.host_plays(1000, -2000, 1);
  bool mixed = true;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    const std::size_t at = f.master_l.size() - kBlock + i;
    mixed = mixed && out[2u * i] == f.master_l[at] + 500 && out[2u * i + 1u] == f.master_r[at] - 1000;  // (1000 * 0.5, -2000 * 0.5)
  }
  CHECK(mixed);
  CHECK(f.master_l.size() > last);
  // The stem took its copy before the return was added: the master channels are the pre-mix master.
  StemReader reader(f.tap.ring());
  alignas(4) static std::int16_t got[3u * kStemBlockFrames * 8u];
  const std::uint32_t frames = 3u * kStemBlockFrames;
  REQUIRE(reader.read_interleaved(got, frames, 8) == frames);
  bool pre = true;
  const std::size_t start = first + (f.tap.published() - 3u) * kStemBlockFrames;  // the ring holds the last three stem blocks
  for (std::uint32_t k = 0; k < frames; ++k) {
    pre = pre && got[k * 8u + 6u] == f.master_l[start + k];
  }
  CHECK(pre);
  // Saturation: a full-scale return on a loud master clips, it does not wrap.
  f.svc.set_return_level(1.0f);
  for (unsigned b = 0; b < 4; ++b) {
    f.step();
    f.host_plays(32767, -32768, 1);
  }
  const std::int16_t* loud = f.step();
  f.host_plays(32767, -32768, 1);
  bool clipped = true;
  bool hit = false;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    const std::size_t at = f.master_l.size() - kBlock + i;
    const std::int32_t want_l = static_cast<std::int32_t>(f.master_l[at]) + 32767;
    clipped = clipped && loud[2u * i] == (want_l > 32767 ? 32767 : want_l);
    hit = hit || want_l > 32767;
  }
  CHECK(clipped);
  CHECK(hit);
  // Level 0: the return is pulled (the ring does not overrun) and nothing is added.
  f.svc.set_return_level(0.0f);
  for (unsigned b = 0; b < 3; ++b) {
    f.step();
    f.host_plays(1000, -2000, 1);
  }
  const std::uint32_t pulled = f.svc.pulled_blocks();
  const std::int16_t* none = f.step();
  CHECK(f.svc.pulled_blocks() == pulled + 1u);
  bool plain = true;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    plain = plain && none[2u * i] == f.master_l[f.master_l.size() - kBlock + i];
  }
  CHECK(plain);
  CHECK(f.counters.out_dropped_frames == 0u);
  // A silenced block (the pipeline's underrun mute): nothing is mixed.
  f.svc.set_return_level(1.0f);
  f.host_plays(1000, -2000, 1);
  const std::int16_t* muted = f.step(nullptr, haudio::kFlagSilence);
  bool same = true;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    same = same && muted[2u * i] == f.master_l[f.master_l.size() - kBlock + i];
  }
  CHECK(same);
  // The level is clamped and a NaN is zero.
  f.svc.set_return_level(7.0f);
  CHECK(f.svc.return_level() == doctest::Approx(1.0f));
  f.svc.set_return_level(std::nanf(""));
  CHECK(f.svc.return_level() == 0.0f);
}

TEST_CASE("usb audio: with the audio in's source Usb the app hears the return in place of the codec (silence while the computer plays nothing); otherwise the codec's block as it was") {
  auto fx = std::make_unique<Fx>();
  Fx& f = *fx;
  REQUIRE(f.ok);
  alignas(4) std::int16_t codec[haudio::kBlockSamples];
  for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
    codec[i] = static_cast<std::int16_t>(100 + i);
  }
  f.step(codec);
  CHECK(std::memcmp(f.in_seen.data(), codec, sizeof codec) == 0);
  CHECK(f.svc.active_blocks() == 0u);
  f.svc.route_return_to_input(true);
  f.step(codec);  // the host plays nothing yet: silence, not the codec
  bool quiet = true;
  for (const std::int16_t s : f.in_seen) {
    quiet = quiet && s == 0;
  }
  CHECK(quiet);
  CHECK(f.svc.active_blocks() == 1u);
  f.svc.streams_changed(false, true);
  f.host_plays(1234, -4321, 4);
  for (unsigned b = 0; b < 6; ++b) {
    f.step(codec);
    f.host_plays(1234, -4321, 1);
  }
  bool ret = true;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    ret = ret && f.in_seen[2u * i] == 1234 && f.in_seen[2u * i + 1u] == -4321;
  }
  CHECK(ret);
  f.svc.route_return_to_input(false);
  f.svc.streams_changed(false, false);
  f.step(codec);
  CHECK(std::memcmp(f.in_seen.data(), codec, sizeof codec) == 0);
}

namespace {

// One AUDIO-mode rig in the boot set (Audio, P8, USB RETURN on) with the real service behind IsoAudio, and the stream hook usb0.cpp installs (on_streams), line for line in effect.
struct BootFixture {
  static Config boot_config() {
    Config c;
    c.mode = Mode::Audio;
    c.profile = Profile::P8;
    c.usb_return = true;
    return c;
  }
  static void on_streams(void* self, bool in_active, std::uint32_t, bool out_active) {
    BootFixture& f = *static_cast<BootFixture*>(self);
    f.fx->svc.streams_changed(in_active, out_active);
    ++f.hook_calls;
  }
  BootFixture() : rig(fresh(boot_config())), fx(std::make_unique<Fx>()), iso(fx->tap.ring(), fx->returns, fx->reader, rig.audio) {
    rig.hw.set_sof_period(nominal_sof_cycles(Speed::Full));
    iso.set_hook(&BootFixture::on_streams, this);
    rig.dev.bind_audio(&iso);
  }
  bool set_alt(std::uint8_t interface, std::uint8_t alt) {
    std::uint8_t raw[8];
    usb_fake::make_setup(raw, 0x01, 0x0B, alt, interface, 0);
    return rig.host.control_none(raw).ok;
  }
  int frame_in(std::uint8_t* data, std::uint32_t cap) {
    rig.hw.sof();
    std::uint32_t len = 0;
    bool d1 = false;
    return rig.hw.in(4, data, cap, len, d1) == Reply::Ack ? static_cast<int>(len) : -1;
  }
  int read_feedback(std::uint8_t* data) {
    std::uint32_t len = 0;
    bool d1 = false;
    return rig.hw.in(5, data, 4, len, d1) == Reply::Ack ? static_cast<int>(len) : -1;
  }

  Rig& rig;
  std::unique_ptr<Fx> fx;
  IsoAudio iso;
  unsigned hook_calls = 0;
};

}  // namespace

TEST_CASE("usb audio: the boot set end to end - the host reads 8-channel packets with the main output's samples on 7 and 8 and silence on 1 to 6, the return is drained, nothing is lost") {
  BootFixture f;
  Fx& fx = *f.fx;
  REQUIRE(fx.ok);
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(fx.svc.streaming());
  CHECK(fx.svc.return_streaming());
  CHECK(f.iso.in_channels() == 8u);

  // The render: 64 frames every 1.451 ms of bus time (a hair slower than the host's 44.1 kHz), keeping the stem ring topped up at three blocks and pulling the return in the same breath. The
  // host: one frame's IN packet and one OUT packet a millisecond.
  std::uint32_t seen = 0;  // master frames the host has read
  std::uint32_t sent = 0;
  std::uint32_t acc = 0;
  std::uint32_t real_packets = 0;
  std::uint32_t silent_packets = 0;
  std::size_t origin = 0;  // where the stream's first frame is in the master the app made
  bool origin_set = false;
  std::uint8_t buf[1024];
  std::uint8_t fb[4];
  std::uint32_t fb_min = 0xFFFFFFFFu, fb_max = 0;
  for (unsigned frame = 0; frame < 1500u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    USB_REQUIRE(len % 16 == 0);  // 8 channels x 2 bytes
    const std::uint32_t n = static_cast<std::uint32_t>(len) / 16u;
    CHECK((n == 44u || n == 45u));
    const std::int16_t* s = reinterpret_cast<const std::int16_t*>(buf);
    bool exact = true;
    if (!origin_set && s[6] == 0 && s[7] == 0) {  // the priming packet: silence of the right size
      CHECK(real_packets == 0u);
      ++silent_packets;
      for (std::uint32_t k = 0; k < n * 8u; ++k) {
        exact = exact && s[k] == 0;
      }
    } else {
      if (!origin_set) {  // the stream's first frame is a frame of the master the app made: find it (the instrument never repeats a sample pair)
        origin_set = true;
        origin = fx.master_l.size();
        for (std::size_t i = 0; i < fx.master_l.size(); ++i) {
          if (fx.master_l[i] == s[6] && fx.master_r[i] == s[7]) {
            origin = i;
            break;
          }
        }
        USB_REQUIRE(origin < fx.master_l.size());
      }
      for (std::uint32_t k = 0; k < n; ++k) {
        for (std::uint32_t c = 0; c < 6u; ++c) {
          exact = exact && s[k * 8u + c] == 0;  // channels 1 to 6: silence
        }
        exact = exact && s[k * 8u + 6u] == fx.master_l[origin + seen + k];  // channel 7: master left, the main output's sample
        exact = exact && s[k * 8u + 7u] == fx.master_r[origin + seen + k];  // channel 8: master right
      }
      seen += n;
      ++real_packets;
    }
    USB_REQUIRE(exact);
    const std::uint32_t n_out = 44u + (((frame * 1176u) / 10000u) != (((frame + 1u) * 1176u) / 10000u) ? 1u : 0u);
    std::uint32_t pkt[48];
    for (std::uint32_t k = 0; k < n_out; ++k) {
      pkt[k] = ((sent + k) & 0xFFFFu) | (((~(sent + k)) & 0xFFFFu) << 16);
    }
    USB_REQUIRE(f.rig.hw.out(4, reinterpret_cast<const std::uint8_t*>(pkt), n_out * 4u, false) == Reply::Ack);
    sent += n_out;
    if (f.read_feedback(fb) == 3) {
      const std::uint32_t v = fb[0] | (fb[1] << 8) | (fb[2] << 16);
      fb_min = v < fb_min ? v : fb_min;
      fb_max = v > fb_max ? v : fb_max;
    }
    acc += 1000u;
    while (acc >= 1451u) {
      acc -= 1451u;
      fx.step();
    }
  }
  CHECK(silent_packets >= 1u);
  CHECK(real_packets > 1000u);
  CHECK(fx.svc.pulled_blocks() > 900u);
  CHECK(f.rig.audio.in_size_other == 0u);
  CHECK(f.rig.audio.in_underruns == 0u);
  CHECK(f.rig.audio.in_dropped_blocks == 0u);
  CHECK(f.rig.audio.out_underruns == 0u);
  CHECK(f.rig.audio.out_dropped_frames == 0u);
  CHECK(f.rig.audio.out_bad_packets == 0u);
  CHECK(f.rig.dev.counters().isr_loop_limit == 0u);
  const std::uint32_t nominal = feedback_value(Speed::Full, nominal_frames_q16(Speed::Full));
  CHECK(fb_min + 400u > nominal);
  CHECK(fb_max < nominal + 400u);

  // Alt 0 on both streams: the hook switches the stems, the return and the engine's staging off.
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 0));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 0));
  CHECK_FALSE(fx.svc.streaming());
  CHECK_FALSE(fx.svc.return_streaming());
  CHECK(fx.engine.stem_slots() == 0u);
}
