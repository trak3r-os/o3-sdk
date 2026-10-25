// SPDX-License-Identifier: GPL-3.0-or-later
//
// The USB audio capture tap (usb/usb_audio.h; docs/M2B-PLAN.md, slice D): the block the main output plays goes to the stem ring, the master on the LAST TWO channels of the stream and
// silence on the rest (7 and 8 of the boot set P8, as track3r-os's P8), the ring's lifetime contract holds under an overwriting ring, the return is pulled and dropped while the OUT
// stream runs, wrap() leaves the block the main output plays untouched, and the whole chain (the tap, usb::IsoAudio, the device logic, the fake controller, a host reading packets) delivers the
// samples that went in. Pure logic and host fakes: the interrupt's real timing and the unit's cost are for the unit.
#include <doctest/doctest.h>

#include "usb_rig.h"
#include "usb_test_util.h"

#include <o3sdk/hal/audio.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/iso_audio.h>
#include <o3sdk/usb/stream.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace ua = o3sdk::platform::mk66::usb_audio;
namespace haudio = o3sdk::hal::audio;
using namespace o3sdk::usb;
using namespace usb_rig;

namespace {

constexpr std::uint32_t kBlock = haudio::kBlock;

// The master, a known function of the global frame number: never 0, so silence is recognisable, and left and right differ.
std::int16_t master_l(std::uint32_t frame) { return static_cast<std::int16_t>(1 + ((frame * 13u) & 0x3FFFu)); }
std::int16_t master_r(std::uint32_t frame) { return static_cast<std::int16_t>(-1 - static_cast<std::int32_t>((frame * 7u + 5u) & 0x3FFFu)); }

// One render block of the master, interleaved, starting at global frame `first`.
void fill_block(std::int16_t* lr, std::uint32_t first) {
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    lr[2u * i] = master_l(first + i);
    lr[2u * i + 1u] = master_r(first + i);
  }
}

// `blocks` render blocks pushed from global frame `first`.
void push_blocks(ua::UsbAudioTap& tap, std::uint32_t first, std::uint32_t blocks) {
  alignas(4) std::int16_t lr[2u * kBlock];
  for (std::uint32_t b = 0; b < blocks; ++b) {
    fill_block(lr, first + b * kBlock);
    tap.push(lr, kBlock);
  }
}

}  // namespace

TEST_CASE("usb audio tap: the profiles it can feed are track3r-os's (every one but P4S), the pool is the lifetime contract's, and it is small") {
  CHECK(ua::supported(Profile::P2));
  CHECK(ua::supported(Profile::P8));
  CHECK(ua::supported(Profile::P10));
  CHECK(ua::supported(Profile::P11));
  CHECK_FALSE(ua::supported(Profile::P4S));
  static_assert(ua::UsbAudioTap::kPool == StemRing::capacity() + 2u, "capacity() + 2 blocks a plane: ring content, the reader's block, the one being produced (usb/stream.h)");
  CHECK(ua::UsbAudioTap::kPool == 5u);
  CHECK(sizeof(ua::UsbAudioTap) < 4096u);  // SRAM_U: the 32 KiB free rule has room (about 3 KiB)
  CHECK(ua::UsbAudioTap::kDefaultChannels == profile_channels(Profile::P8));  // the boot set
}

TEST_CASE("usb audio tap: disabled it publishes nothing; a null block or a stopped stream changes nothing") {
  static ua::UsbAudioTap tap;
  tap.reset();
  CHECK_FALSE(tap.enabled());
  push_blocks(tap, 0, 6);
  CHECK(tap.published() == 0u);
  CHECK(tap.ring().size() == 0u);
  tap.set_enabled(true);
  tap.push(nullptr, kBlock);  // a missing block is skipped
  CHECK(tap.published() == 0u);
  push_blocks(tap, 0, 2);
  CHECK(tap.published() == 1u);
  CHECK(tap.ring().size() == 1u);
}

TEST_CASE("usb audio tap: the master is on the last two channels of the stream and every other channel is silence, for every profile") {
  static ua::UsbAudioTap tap;
  static std::int16_t out[3u * kStemBlockFrames * kMaxChannels];
  struct Case {
    Profile profile;
    std::uint32_t master;  // 0-based index of the left channel
  };
  const Case cases[] = {{Profile::P2, 0}, {Profile::P8, 6}, {Profile::P10, 8}, {Profile::P11, 9}};
  for (const Case& c : cases) {
    const std::uint32_t channels = profile_channels(c.profile);
    INFO("profile with " << channels << " channels");
    tap.reset();
    tap.set_channels(channels);
    tap.set_enabled(true);
    push_blocks(tap, 0, 6);  // three stem blocks: the ring holds all three
    CHECK(tap.published() == 3u);
    StemReader reader(tap.ring());
    const std::uint32_t frames = 3u * kStemBlockFrames;
    USB_REQUIRE(reader.read_interleaved(out, frames, channels) == frames);
    CHECK(reader.dropped_blocks() == 0u);
    bool exact = true;
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t ch = 0; ch < channels; ++ch) {
        const std::int16_t got = out[f * channels + ch];
        std::int16_t want = 0;
        if (ch == c.master) {
          want = master_l(f);
        } else if (ch == c.master + 1u) {
          want = master_r(f);
        }
        exact = exact && got == want;
      }
    }
    CHECK(exact);
    CHECK(c.master + 2u == channels);  // (the master is the last pair: 7 and 8 of P8)
  }
}

TEST_CASE("usb audio tap: a tap built over zeroed memory is all zeros and already the boot set (the image's tap is `.bss`, not `.data`)") {
  alignas(ua::UsbAudioTap) static unsigned char storage[sizeof(ua::UsbAudioTap)];
  std::memset(storage, 0, sizeof storage);
  const ua::UsbAudioTap* tap = new (storage) ua::UsbAudioTap();
  bool zero = true;
  for (unsigned char b : storage) {
    zero = zero && b == 0u;
  }
  CHECK(zero);  // a non-zero initialiser anywhere in the tap would move the object to `.data`: 3 KB of flash and a copy at boot
  CHECK(tap->channels() == 8u);
  CHECK(tap->channels() == ua::UsbAudioTap::kDefaultChannels);
  tap->~UsbAudioTap();
}

TEST_CASE("usb audio tap: the boot set P8 has the master on channels 7 and 8 and channels 1 to 6 silent, with the default channel count") {
  static ua::UsbAudioTap tap;
  tap.reset();
  CHECK(tap.channels() == 8u);  // no set_channels() yet: the default is the boot profile
  tap.set_enabled(true);
  push_blocks(tap, 0, 2);
  StemReader reader(tap.ring());
  alignas(4) std::int16_t out[kStemBlockFrames * 8u];
  USB_REQUIRE(reader.read_interleaved(out, kStemBlockFrames, 8) == kStemBlockFrames);
  bool ok = true;
  for (std::uint32_t f = 0; f < kStemBlockFrames; ++f) {
    for (std::uint32_t ch = 0; ch < 6u; ++ch) {
      ok = ok && out[f * 8u + ch] == 0;
    }
    ok = ok && out[f * 8u + 6u] == master_l(f) && out[f * 8u + 7u] == master_r(f);
  }
  CHECK(ok);
  // A count the stream cannot have is clamped, not trusted: 0 and 1 put the master on 0 and 1, more than 11 on the last pair of 11.
  tap.set_channels(0);
  CHECK(tap.channels() == 0u);
  tap.set_channels(99);
  CHECK(tap.channels() == kMaxChannels);
}

TEST_CASE("usb audio tap: a stream starts on a stem-block boundary - a half block pushed before a stop is forgotten") {
  static ua::UsbAudioTap tap;
  tap.reset();
  tap.set_channels(2);
  tap.set_enabled(true);
  push_blocks(tap, 1000, 1);  // the first half of a stem block...
  tap.set_enabled(false);
  push_blocks(tap, 2000, 1);  // ... a stopped tap sees a block and forgets the phase ...
  tap.set_enabled(true);
  push_blocks(tap, 3000, 2);  // ... and the next stream's first block is the first half of a new stem block
  CHECK(tap.published() == 1u);
  StemReader reader(tap.ring());
  alignas(4) std::int16_t out[kStemBlockFrames * 2u];
  USB_REQUIRE(reader.read_interleaved(out, kStemBlockFrames, 2) == kStemBlockFrames);
  bool ok = true;
  for (std::uint32_t f = 0; f < kStemBlockFrames; ++f) {
    ok = ok && out[2u * f] == master_l(3000u + f) && out[2u * f + 1u] == master_r(3000u + f);
  }
  CHECK(ok);
}

TEST_CASE("usb audio tap: any frame count is accumulated into whole stem blocks, and a call is bounded to one stem block") {
  static ua::UsbAudioTap tap;
  tap.reset();
  tap.set_channels(2);
  tap.set_enabled(true);
  alignas(4) std::int16_t lr[2u * kStemBlockFrames];
  // 50 + 78 frames: one stem block, the second call crossing nothing; then 100 + 28 as the next.
  const std::uint32_t splits[] = {50, 78, 100, 28};
  std::uint32_t frame = 0;
  for (std::uint32_t n : splits) {
    for (std::uint32_t i = 0; i < n; ++i) {
      lr[2u * i] = master_l(frame + i);
      lr[2u * i + 1u] = master_r(frame + i);
    }
    tap.push(lr, n);
    frame += n;
  }
  CHECK(tap.published() == 2u);
  // A call with more than a stem block takes one stem block's worth (the loop is bounded by the call, not the caller).
  for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
    lr[2u * i] = 1;
    lr[2u * i + 1u] = 1;
  }
  tap.push(lr, 100000);
  CHECK(tap.published() == 3u);
  CHECK(tap.ring().size() == 3u);
}

TEST_CASE("usb audio tap: the lifetime contract - a block the reader holds survives capacity() + 1 further blocks, and is overwritten by the next (the pool is exactly that size)") {
  static ua::UsbAudioTap tap;
  tap.reset();
  tap.set_channels(2);
  tap.set_enabled(true);
  // Block 0 in the ring; the reader takes 10 frames of it and so holds it.
  push_blocks(tap, 0, 2);
  StemReader reader(tap.ring());
  alignas(4) std::int16_t head[10u * 2u];
  USB_REQUIRE(reader.read_interleaved(head, 10, 2) == 10u);
  // capacity() + 1 = 4 further blocks: the ring (3 usable) overwrites the oldest of them, and the reader's block is not touched.
  const std::uint32_t further = StemRing::capacity() + 1u;
  push_blocks(tap, kStemBlockFrames, 2u * further);
  USB_REQUIRE(tap.published() == 1u + further);
  alignas(4) std::int16_t rest[(kStemBlockFrames - 10u) * 2u];
  USB_REQUIRE(reader.read_interleaved(rest, kStemBlockFrames - 10u, 2) == kStemBlockFrames - 10u);
  bool intact = true;
  for (std::uint32_t f = 10; f < kStemBlockFrames; ++f) {
    intact = intact && rest[2u * (f - 10u)] == master_l(f) && rest[2u * (f - 10u) + 1u] == master_r(f);
  }
  CHECK(intact);
  // The next block read is a newer one, not the stale one: the ring skipped what it overwrote, and counted it.
  alignas(4) std::int16_t next[2u];
  USB_REQUIRE(reader.read_interleaved(next, 1, 2) == 1u);
  CHECK(reader.dropped_blocks() >= 1u);
  CHECK(next[0] != master_l(kStemBlockFrames));  // block 1 was overwritten in the ring

  // The bound is tight, and the test sees it: with one more block the slot of the block held is reused.
  tap.reset();
  tap.set_channels(2);
  tap.set_enabled(true);
  push_blocks(tap, 0, 2);
  StemReader reader2(tap.ring());
  USB_REQUIRE(reader2.read_interleaved(head, 10, 2) == 10u);
  push_blocks(tap, kStemBlockFrames, 2u * (further + 1u));  // capacity() + 2 further blocks
  USB_REQUIRE(reader2.read_interleaved(rest, kStemBlockFrames - 10u, 2) == kStemBlockFrames - 10u);
  bool overwritten = false;
  for (std::uint32_t f = 10; f < kStemBlockFrames; ++f) {
    overwritten = overwritten || rest[2u * (f - 10u)] != master_l(f);
  }
  CHECK(overwritten);
}

TEST_CASE("usb audio tap: the return is pulled and dropped while the OUT stream runs - the ring stays at the proven fill, nothing is counted as lost") {
  static ua::UsbAudioTap tap;
  static ReturnRing ring;
  static Counters counters;
  tap.reset();
  ring.reset();
  counters.reset();
  ReturnReader reader(ring, counters);
  OutStream out(ring, counters);
  std::uint32_t pkt[kReturnMaxChunk] = {};

  // Not attached and not running: the host's frames pile up to the ring's capacity, which is what the drain prevents.
  tap.drain_return();
  CHECK(tap.drained() == 0u);
  for (unsigned i = 0; i < 10; ++i) {
    out.on_packet(pkt, 44u * 4u);
  }
  CHECK(ring.size_from_producer() == 10u * 44u);
  tap.attach_return(&reader);
  tap.drain_return();  // attached, but the OUT stream is not running
  CHECK(tap.drained() == 0u);
  CHECK(ring.size_from_producer() == 10u * 44u);

  // The stream starts (the hook): every render block pulls 64 frames; the host sends 64 frames a block in two packets.
  ring.reset();
  counters.reset();
  reader.request_restart();
  tap.set_return_enabled(true);
  CHECK(tap.return_enabled());
  std::uint32_t fill_min = 0xFFFFFFFFu;
  std::uint32_t fill_max = 0;
  for (unsigned block = 0; block < 400; ++block) {
    out.on_packet(pkt, 32u * 4u);
    out.on_packet(pkt, 32u * 4u);
    tap.drain_return();
    if (block >= 8) {  // after the start-up
      const std::uint32_t fill = ring.size_from_producer();
      fill_min = fill < fill_min ? fill : fill_min;
      fill_max = fill > fill_max ? fill : fill_max;
    }
  }
  CHECK(tap.drained() == 400u);
  CHECK(counters.out_dropped_frames == 0u);
  CHECK(counters.out_underruns == 0u);
  CHECK(fill_min >= kReturnPrimeFrames - 64u);  // the pull trims the ring to the prime fill at its start and holds it there
  CHECK(fill_max <= kReturnPrimeFrames + 64u);

  // The stream stops: no more pulls.
  tap.set_return_enabled(false);
  const std::uint32_t drained = tap.drained();
  tap.drain_return();
  CHECK(tap.drained() == drained);
}

TEST_CASE("usb audio tap: wrap() runs the app's callback first, leaves the block the main output plays untouched, and feeds the tap with it") {
  struct Inner {
    unsigned calls = 0;
    std::uint32_t first_frame = 0;
  } inner;
  const haudio::BlockCallback app{[](void* user, const haudio::BlockIo& io) noexcept {
                                    Inner& in = *static_cast<Inner*>(user);
                                    fill_block(io.out, in.first_frame);
                                    in.first_frame += kBlock;
                                    ++in.calls;
                                  },
                                  &inner};
  ua::UsbAudioTap& tap = ua::tap();
  tap.reset();
  tap.set_channels(2);
  const haudio::BlockCallback wrapped = ua::wrap(app);
  USB_REQUIRE(wrapped.fn != nullptr);
  CHECK(wrapped.fn != app.fn);

  alignas(4) std::int16_t in_block[2u * kBlock] = {};
  alignas(4) std::int16_t out_block[2u * kBlock] = {};
  haudio::BlockReport report{};
  const haudio::BlockIo io{in_block, out_block, 0u, &report};

  // Disabled tap: the app runs, its block is the output, nothing is published.
  wrapped.fn(wrapped.user, io);
  CHECK(inner.calls == 1u);
  CHECK(tap.published() == 0u);
  alignas(4) std::int16_t expect[2u * kBlock];
  fill_block(expect, 0);
  CHECK(std::memcmp(out_block, expect, sizeof expect) == 0);

  // Enabled: two blocks make a stem block of exactly what the app wrote, and io.out is as the app left it.
  tap.set_enabled(true);
  wrapped.fn(wrapped.user, io);
  fill_block(expect, kBlock);
  CHECK(std::memcmp(out_block, expect, sizeof expect) == 0);
  wrapped.fn(wrapped.user, io);
  CHECK(inner.calls == 3u);
  CHECK(tap.published() == 1u);
  StemReader reader(tap.ring());
  alignas(4) std::int16_t got[2u * kStemBlockFrames];
  USB_REQUIRE(reader.read_interleaved(got, kStemBlockFrames, 2) == kStemBlockFrames);
  bool exact = true;
  for (std::uint32_t f = 0; f < kStemBlockFrames; ++f) {
    exact = exact && got[2u * f] == master_l(kBlock + f) && got[2u * f + 1u] == master_r(kBlock + f);
  }
  CHECK(exact);

  // No app callback (a null function): nothing is called, and the block is still offered to the tap.
  const haudio::BlockCallback bare = ua::wrap(haudio::BlockCallback{nullptr, nullptr});
  USB_REQUIRE(bare.fn != nullptr);
  bare.fn(bare.user, io);
  CHECK(inner.calls == 3u);
  tap.set_enabled(false);
  tap.reset();
}

namespace {

// One AUDIO-mode rig in the boot set (Audio, P8, USB RETURN on) with the real tap behind IsoAudio, and the stream hook usb0.cpp installs (on_streams), line for line in effect.
struct BootFixture {
  static Config boot_config() {
    Config c;
    c.mode = Mode::Audio;
    c.profile = Profile::P8;
    c.usb_return = true;
    return c;
  }
  static void on_streams(void* self, bool in_active, std::uint32_t channels, bool out_active) {
    BootFixture& f = *static_cast<BootFixture*>(self);
    if (in_active) {
      f.tap.set_channels(channels);
    }
    f.tap.set_enabled(in_active);
    f.tap.set_return_enabled(out_active);
    ++f.hook_calls;
  }
  BootFixture()
      : rig(fresh(boot_config())), reader(returns, rig.audio), iso(tap.ring(), returns, reader, rig.audio) {
    tap.reset();
    rig.hw.set_sof_period(nominal_sof_cycles(Speed::Full));
    iso.set_hook(&BootFixture::on_streams, this);
    rig.dev.bind_audio(&iso);
    tap.attach_return(&reader);
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
  ua::UsbAudioTap tap;
  ReturnRing returns;
  ReturnReader reader;
  IsoAudio iso;
  unsigned hook_calls = 0;
};

}  // namespace

TEST_CASE("usb audio tap: the boot set end to end - the host reads 8-channel packets with the master on 7 and 8 and silence on 1 to 6, the return is drained, nothing is lost") {
  BootFixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(f.tap.enabled());
  CHECK(f.tap.return_enabled());
  CHECK(f.tap.channels() == 8u);  // the stream's own count: what the descriptors of P8 carry
  CHECK(f.iso.in_channels() == 8u);

  // The render: 64 master frames every 1.451 ms of bus time (a hair slower than the host's 44.1 kHz, as the existing return test), keeping the stem ring topped up at three blocks;
  // the drain pulls the return in the same breath. The host: one frame's IN packet and one OUT packet a millisecond.
  std::uint32_t produced = 0;  // master frames given to the tap
  std::uint32_t seen = 0;      // master frames the host has read
  std::uint32_t sent = 0;
  std::uint32_t acc = 0;
  std::uint32_t real_packets = 0;
  std::uint32_t silent_packets = 0;
  std::uint8_t buf[1024];
  std::uint8_t fb[4];
  std::uint32_t fb_min = 0xFFFFFFFFu, fb_max = 0;
  for (unsigned frame = 0; frame < 1500u; ++frame) {
    // The host's frame: its IN poll (the packet the start of frame armed), then its OUT packet (44 frames, 45 on the 44.1176 cadence) and the feedback endpoint.
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    USB_REQUIRE(len % 16 == 0);  // 8 channels x 2 bytes
    const std::uint32_t n = static_cast<std::uint32_t>(len) / 16u;
    CHECK((n == 44u || n == 45u));
    const std::int16_t* s = reinterpret_cast<const std::int16_t*>(buf);
    bool exact = true;
    if (s[6] == 0) {  // the priming packet: silence of the right size (the master is never 0)
      CHECK(real_packets == 0u);
      ++silent_packets;
      for (std::uint32_t k = 0; k < n * 8u; ++k) {
        exact = exact && s[k] == 0;
      }
    } else {
      for (std::uint32_t k = 0; k < n; ++k) {
        for (std::uint32_t c = 0; c < 6u; ++c) {
          exact = exact && s[k * 8u + c] == 0;  // channels 1 to 6: silence
        }
        exact = exact && s[k * 8u + 6u] == master_l(seen + k);  // channel 7: master left
        exact = exact && s[k * 8u + 7u] == master_r(seen + k);  // channel 8: master right
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
    // The render, at its own rate.
    acc += 1000u;
    while (acc >= 1451u) {
      acc -= 1451u;
      push_blocks(f.tap, produced, 1);
      produced += kBlock;
      f.tap.drain_return();
    }
  }
  CHECK(silent_packets >= 1u);
  CHECK(real_packets > 1000u);
  CHECK(f.tap.drained() > 900u);
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

  // Alt 0 on both streams: the hook switches the tap and the drain off.
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 0));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 0));
  CHECK_FALSE(f.tap.enabled());
  CHECK_FALSE(f.tap.return_enabled());
}
