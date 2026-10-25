// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006d: AUDIO mode's isochronous endpoints in the USB interrupt (usb/device.h + usb/iso_audio.h) against the fake controller: the endpoint registers of
// endpoints 4 and 5, SET_INTERFACE starting and stopping the stems (IN) and return (OUT, with feedback) streams, the packets the host collects and sends,
// the stem and return ring contracts end to end (the samples that go in are the samples that come out), the stream hook, bus reset / configuration / alt 0
// stopping everything, a mode switch through ControlPipe::rebind, and the bound on the interrupt's work while streaming. Host only (heap-free, but long).
#include <doctest/doctest.h>

#include <initializer_list>

#include "usb_test_util.h"

#include <o3sdk/usb/device.h>
#include <o3sdk/usb/iso_audio.h>
#include <o3sdk/usb/stream.h>

#include "usb_rig.h"

#include <cstdint>
#include <cstring>

using namespace o3sdk::usb;
using namespace usb_rig;

namespace {

constexpr std::uint32_t kSofCycles = nominal_sof_cycles(Speed::Full);  // the host's 1 ms frame in CPU cycles: the nominal rate

// The engine's side, simulated: stem blocks of 11 channels in a pool of 5 (the lifetime contract of usb/stream.h), each sample a known function of its
// global frame number and channel, written into the ring a block at a time.
struct Engine {
  StemRing& ring;
  alignas(4) std::int16_t pool[kMaxChannels][5][kStemBlockFrames] = {};
  std::uint32_t blocks = 0;
  explicit Engine(StemRing& r) : ring(r) {}

  static std::int16_t sample(std::uint32_t frame, std::uint32_t channel) {
    return static_cast<std::int16_t>(1 + ((frame * 7u + channel * 1000u) & 0x3FFFu));  // never 0, so silence is recognisable
  }
  void produce_block() {
    const unsigned slot = blocks % 5u;
    StemBlock b{};
    for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
      for (std::uint32_t i = 0; i < kStemBlockFrames; ++i) {
        pool[c][slot][i] = sample(blocks * kStemBlockFrames + i, c);
      }
      b.ch[c] = pool[c][slot];
    }
    ring.write(&b, 1);
    ++blocks;
  }
};

struct Hook {
  bool in_active = false;
  bool out_active = false;
  std::uint32_t channels = 0;
  unsigned calls = 0;
  static void fn(void* self, bool in, std::uint32_t ch, bool out) {
    Hook* h = static_cast<Hook*>(self);
    h->in_active = in;
    h->out_active = out;
    h->channels = ch;
    ++h->calls;
  }
};

// One AUDIO-mode rig with its streams bound and a started device configuration.
struct Fixture {
  explicit Fixture(Profile profile = Profile::P10, bool usb_return = true)
      : rig(fresh(config(profile, usb_return))), reader(returns, rig.audio), iso(stems, returns, reader, rig.audio), engine(stems) {
    rig.hw.set_sof_period(kSofCycles);
    iso.set_hook(&Hook::fn, &hook);
    rig.dev.bind_audio(&iso);
  }
  static Config config(Profile profile, bool usb_return) {
    Config c;
    c.mode = Mode::Audio;
    c.profile = profile;
    c.usb_return = usb_return;
    return c;
  }
  bool set_alt(std::uint8_t interface, std::uint8_t alt) {
    std::uint8_t raw[8];
    usb_fake::make_setup(raw, 0x01, 0x0B, alt, interface, 0);
    return rig.host.control_none(raw).ok;
  }
  // One bus frame as the host sees it: the SOF, then the IN token of the stems endpoint. Returns the packet length in bytes, or -1 for a NAK.
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
  StemRing stems;
  ReturnRing returns;
  ReturnReader reader;
  IsoAudio iso;
  Engine engine;
  Hook hook;
};

}  // namespace

TEST_CASE("usb iso: AUDIO mode enables endpoints 4 and 5 as isochronous (no handshake), and bus reset or alt 0 leave them with no descriptors") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  // ENDPT4: both directions, isochronous (EPHSHK clear), no SETUP; ENDPT5: IN only. ENDPT2 (CDC bulk) keeps its handshake bit.
  CHECK(f.rig.hw.endpt(4) == (kEndptRxEn | kEndptTxEn | kEndptCtlDisable));
  CHECK(f.rig.hw.endpt(5) == (kEndptTxEn | kEndptCtlDisable));
  CHECK((f.rig.hw.endpt(2) & kEndptHandshake) != 0u);
  // Alt 0 everywhere after the configuration: nothing is armed, a poll is NAKed (an isochronous device sends nothing), no stream runs.
  std::uint8_t buf[1024];
  CHECK(f.frame_in(buf, sizeof buf) == -1);
  CHECK_FALSE(f.hook.in_active);
  CHECK(f.hook.calls == 0u);
  CHECK(f.rig.dev.counters().iso_in_packets == 0u);
}

TEST_CASE("usb iso: alt 1 on the stems interface starts the stream, the packets carry the engine's samples in order, alt 0 stops it") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  CHECK(f.hook.in_active);
  CHECK(f.hook.channels == 10u);
  CHECK_FALSE(f.hook.out_active);
  CHECK(f.rig.dev.counters().iso_in_starts == 1u);
  CHECK(f.rig.dev.counters().iso_in_packets == 1u);  // the first token already finds a (silent, priming) packet
  CHECK(f.rig.hw.endpt(4) == (kEndptRxEn | kEndptTxEn | kEndptCtlDisable));  // streaming does not touch the enables

  for (unsigned i = 0; i < 3u; ++i) {
    f.engine.produce_block();
  }
  std::uint8_t buf[1024];
  std::uint32_t frames_seen = 0;
  std::uint32_t sizes[2] = {0, 0};
  std::uint32_t real_packets = 0;
  std::uint32_t silent_packets = 0;
  for (unsigned frame = 0; frame < 400u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    USB_REQUIRE(len % 20 == 0);  // 10 channels x 2 bytes
    const std::uint32_t n = static_cast<std::uint32_t>(len) / 20u;
    CHECK((n == 44u || n == 45u));
    ++sizes[n == 45u ? 1 : 0];
    const std::int16_t* s = reinterpret_cast<const std::int16_t*>(buf);
    if (s[0] == 0) {  // the priming packet: silence of the right size (the samples of the engine are never 0)
      CHECK(real_packets == 0u);
      ++silent_packets;
      for (std::uint32_t k = 0; k < n * 10u; ++k) {
        CHECK(s[k] == 0);
      }
    } else {
      // From here on the stream is the engine's samples, frame after frame, ten channels interleaved, starting at frame 0 of the first block.
      for (std::uint32_t k = 0; k < n; ++k) {
        for (std::uint32_t c = 0; c < 10u; ++c) {
          USB_REQUIRE(s[k * 10u + c] == Engine::sample(frames_seen + k, c));
        }
      }
      frames_seen += n;
      ++real_packets;
    }
    while (f.engine.blocks * kStemBlockFrames < frames_seen + 3u * kStemBlockFrames) {  // the engine keeps the ring topped up at three blocks
      f.engine.produce_block();
    }
  }
  CHECK(silent_packets == 1u);
  CHECK(real_packets == 399u);
  CHECK(sizes[0] > 0u);
  CHECK(sizes[1] > 0u);
  CHECK(f.rig.audio.in_size_other == 0u);
  CHECK(f.rig.audio.in_underruns == 0u);
  CHECK(f.rig.audio.in_dropped_blocks == 0u);
  CHECK(f.rig.dev.counters().iso_in_missed == 0u);
  CHECK(f.rig.dev.counters().pingpong_resyncs == 0u);
  CHECK(f.rig.dev.counters().isr_loop_limit == 0u);
  // Alt 0: the descriptors are taken back and the stream stops; the host's polls are NAKed again.
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 0));
  CHECK_FALSE(f.hook.in_active);
  CHECK(f.frame_in(buf, sizeof buf) == -1);
  CHECK(f.frame_in(buf, sizeof buf) == -1);
  // And it can start again (alt 1 twice in a row restarts: the second one flushes).
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  CHECK(f.hook.in_active);
  CHECK(f.rig.dev.counters().iso_in_starts == 3u);
  CHECK(f.frame_in(buf, sizeof buf) > 0);
}

TEST_CASE("usb iso: an engine that stops producing is an underrun (silence of the right size, counted), and one that outruns the host drops the oldest block") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  std::uint8_t buf[1024];
  for (unsigned i = 0; i < 3u; ++i) {
    f.engine.produce_block();
  }
  std::uint32_t consumed = 0;
  for (unsigned frame = 0; frame < 20u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    consumed += static_cast<std::uint32_t>(len) / 20u;
    while (f.engine.blocks * kStemBlockFrames < consumed + 3u * kStemBlockFrames) {
      f.engine.produce_block();
    }
  }
  CHECK(f.rig.audio.in_underruns == 0u);
  // No more blocks: the ring runs dry after about 8 more packets; every packet is still a legal size.
  for (unsigned frame = 0; frame < 30u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    CHECK((len == 880 || len == 900));
  }
  CHECK(f.rig.audio.in_underruns >= 1u);
  CHECK(f.rig.audio.in_size_other == 0u);
  // The engine comes back with six blocks at once: the ring keeps the newest, counts the rest.
  for (unsigned i = 0; i < 6u; ++i) {
    f.engine.produce_block();
  }
  for (unsigned frame = 0; frame < 4u; ++frame) {
    USB_REQUIRE(f.frame_in(buf, sizeof buf) > 0);
  }
  CHECK(f.rig.audio.in_dropped_blocks >= 3u);
}

TEST_CASE("usb iso: alt 1 on the return interface starts OUT and feedback; both OUT descriptors are armed and the first feedback is the nominal value") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(f.hook.out_active);
  CHECK_FALSE(f.hook.in_active);
  CHECK(f.rig.dev.counters().iso_out_starts == 1u);
  // The first feedback packet is there at once, three bytes (Q10.14, little endian).
  std::uint8_t fb[4];
  USB_REQUIRE(f.read_feedback(fb) == 3);
  const std::uint32_t first = fb[0] | (fb[1] << 8) | (fb[2] << 16);
  CHECK(first == feedback_value(Speed::Full, nominal_frames_q16(Speed::Full)));
  CHECK(f.read_feedback(fb) == -1);  // one packet pending at a time: the next is armed at the next start of frame
  f.rig.hw.sof();
  CHECK(f.read_feedback(fb) == 3);
  // OUT: ten packets of 44 frames never meet a NAK (the descriptors are re-armed as the tokens complete); a zero-length packet is counted, not stored.
  std::uint32_t pkt[48] = {};
  for (unsigned i = 0; i < 10u; ++i) {
    f.rig.hw.sof();
    USB_REQUIRE(f.rig.hw.out(4, reinterpret_cast<const std::uint8_t*>(pkt), 176u, false) == Reply::Ack);
  }
  CHECK(f.rig.audio.out_packets == 10u);
  CHECK(f.iso.return_fill_frames() == 440u);
  f.rig.hw.sof();
  USB_REQUIRE(f.rig.hw.out(4, reinterpret_cast<const std::uint8_t*>(pkt), 0u, false) == Reply::Ack);
  CHECK(f.rig.dev.counters().iso_out_empty == 1u);
  CHECK(f.rig.audio.out_packets == 10u);
  // Alt 0: the return endpoint and its feedback stop answering.
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 0));
  CHECK_FALSE(f.hook.out_active);
  f.rig.hw.sof();
  CHECK(f.read_feedback(fb) == -1);
}

TEST_CASE("usb iso: the return stream end to end - frames in order, the ring primed once, no underrun at the nominal rate, feedback steady") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  std::uint32_t sent = 0;
  std::uint32_t expect = 0;       // the next sample the engine must see once it is playing (frame index of the stream)
  bool playing = false;
  std::uint32_t acc = 0;
  std::uint32_t real_pulls = 0;
  alignas(4) std::int16_t l[64], r[64];
  std::uint8_t fb[4];
  std::uint32_t fb_min = 0xFFFFFFFFu, fb_max = 0;
  for (unsigned frame = 0; frame < 3000u; ++frame) {
    const std::uint32_t n = 44u + (((frame * 1176u) / 10000u) != (((frame + 1u) * 1176u) / 10000u) ? 1u : 0u);  // 44.1176 frames per frame
    std::uint32_t pkt[48];
    for (std::uint32_t k = 0; k < n; ++k) {
      const std::uint32_t idx = sent + k;
      pkt[k] = (idx & 0xFFFFu) | (((~idx) & 0xFFFFu) << 16);
    }
    f.rig.hw.sof();
    USB_REQUIRE(f.rig.hw.out(4, reinterpret_cast<const std::uint8_t*>(pkt), n * 4u, false) == Reply::Ack);
    sent += n;
    if (f.read_feedback(fb) == 3) {
      const std::uint32_t v = fb[0] | (fb[1] << 8) | (fb[2] << 16);
      fb_min = v < fb_min ? v : fb_min;
      fb_max = v > fb_max ? v : fb_max;
    }
    acc += 1000u;
    while (acc >= 1451u) {  // 64 frames per 1.451 ms = 44,106 Hz: a hair slower than the host, so the ring creeps up and the trim has something to do
      acc -= 1451u;
      if (f.reader.pull(l, r, 64)) {
        if (!playing) {
          // The stream starts where the prime trim left it: the first real pull sets the expected sequence.
          expect = static_cast<std::uint32_t>(static_cast<std::uint16_t>(l[0]));
          playing = true;
        }
        for (std::uint32_t i = 0; i < 64u; ++i) {
          USB_REQUIRE(static_cast<std::uint16_t>(l[i]) == ((expect + i) & 0xFFFFu));
          CHECK(static_cast<std::uint16_t>(r[i]) == static_cast<std::uint16_t>(~(expect + i)));
        }
        expect += 64u;
        ++real_pulls;
      } else {
        CHECK_FALSE(playing);  // after priming the ring never runs dry at this rate
      }
    }
  }
  CHECK(playing);
  CHECK(real_pulls > 1000u);
  CHECK(f.rig.audio.out_underruns == 0u);
  CHECK(f.rig.audio.out_dropped_frames == 0u);
  CHECK(f.rig.audio.out_bad_packets == 0u);
  CHECK(f.rig.dev.counters().pingpong_resyncs == 0u);
  // Feedback: always 3 bytes, within +/-500 ppm of nominal (a few hundred Q10.14 LSB at most) at all times.
  const std::uint32_t nominal = feedback_value(Speed::Full, nominal_frames_q16(Speed::Full));
  CHECK(fb_min + 400u > nominal);
  CHECK(fb_max < nominal + 400u);
  CHECK(f.rig.audio.fb_last >= fb_min);
}

TEST_CASE("usb iso: bus reset, a new configuration and the end of a session stop every stream; the hook says so") {
  Fixture f;
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(f.hook.in_active);
  CHECK(f.hook.out_active);
  // SET_CONFIGURATION again: alternate settings go back to 0 and the streams stop.
  USB_REQUIRE(f.rig.set_configuration(1).ok);
  CHECK_FALSE(f.hook.in_active);
  CHECK_FALSE(f.hook.out_active);
  std::uint8_t buf[1024];
  CHECK(f.frame_in(buf, sizeof buf) == -1);
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  CHECK(f.hook.in_active);
  // A bus reset (the host went away or re-enumerates): everything off, endpoints disabled again.
  f.rig.hw.bus_reset();
  CHECK_FALSE(f.hook.in_active);
  CHECK(f.rig.hw.endpt(4) == 0u);
  CHECK(f.rig.hw.endpt(5) == 0u);
  USB_REQUIRE(f.rig.enumerate());
  CHECK(f.frame_in(buf, sizeof buf) == -1);  // alt 0 after the new enumeration
}

TEST_CASE("usb iso: both streams and CDC together - one interrupt's work stays bounded and the CDC console still runs") {
  Fixture f(Profile::P11);
  USB_REQUIRE(f.rig.enumerate());
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(f.hook.channels == 11u);
  for (unsigned i = 0; i < 3u; ++i) {
    f.engine.produce_block();
  }
  const char* text = "hello";
  f.rig.writer.write(text, 5);
  f.rig.writer.flush();
  std::uint8_t buf[1024];
  std::uint32_t pkt[48] = {};
  std::uint32_t cdc_seen = 0;
  for (unsigned frame = 0; frame < 200u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE(len > 0);
    USB_REQUIRE(len % 22 == 0);  // 11 channels x 2 bytes
    f.rig.hw.out(4, reinterpret_cast<const std::uint8_t*>(pkt), 176u, false);
    std::uint8_t fb[4];
    f.read_feedback(fb);
    if (frame % 3u == 0u) {
      f.engine.produce_block();
    }
    std::uint8_t cdc[64];
    std::uint32_t got = 0;
    if (f.rig.host.bulk_in(2, cdc, sizeof cdc, got) == Reply::Ack) {
      cdc_seen += got;
    }
  }
  CHECK(cdc_seen == 5u);  // "hello", once, through the CDC IN endpoint that shares the bus with the isochronous ones
  CHECK(f.rig.dev.counters().isr_loop_limit == 0u);
  CHECK(f.rig.dev.counters().pingpong_resyncs == 0u);
  CHECK(f.rig.dev.counters().tokens_stray == 0u);
}

TEST_CASE("usb iso: a mode switch through ControlPipe::rebind presents the other descriptor set (bcdDevice told apart, AUDIO's 384-byte configuration)") {
  Rig& rig = fresh();  // STANDARD
  USB_REQUIRE(rig.enumerate());
  std::uint8_t dev[18];
  Host::Result r = rig.get_descriptor(1, 0, 0, 18, dev, sizeof dev);
  USB_REQUIRE(r.ok);
  CHECK((dev[12] | (dev[13] << 8)) == 0x0001);
  CHECK(rig.pipe.config().mode == Mode::Standard);

  // The platform's detach / attach: rebuild into the same buffers, rebind, prepare, enumerate again.
  Config audio = rig.cfg;
  audio.mode = Mode::Audio;
  audio.profile = Profile::P8;
  rig.cfg = audio;
  rig.dev_len = build_device_descriptor(rig.cfg, rig.dev_desc, sizeof rig.dev_desc);
  rig.cfg_len = build_config_descriptor(rig.cfg, rig.cfg_desc, sizeof rig.cfg_desc);
  rig.pipe.rebind(rig.cfg, rig.dev_desc, rig.dev_len, rig.cfg_desc, rig.cfg_len);
  rig.dev.prepare();
  CHECK(rig.pipe.state() == DeviceState::Default);
  USB_REQUIRE(rig.enumerate());
  r = rig.get_descriptor(1, 0, 0, 18, dev, sizeof dev);
  USB_REQUIRE(r.ok);
  CHECK((dev[12] | (dev[13] << 8)) == 0x0101);
  CHECK(rig.cfg_len == 384u);
  CHECK(rig.pipe.config().mode == Mode::Audio);
  CHECK(rig.pipe.configured());
  CHECK(rig.hw.endpt(4) == (kEndptRxEn | kEndptTxEn | kEndptCtlDisable));  // AUDIO's endpoints came with the configuration
  // And back: STANDARD has no endpoint 4 or 5.
  rig.cfg.mode = Mode::Standard;
  rig.dev_len = build_device_descriptor(rig.cfg, rig.dev_desc, sizeof rig.dev_desc);
  rig.cfg_len = build_config_descriptor(rig.cfg, rig.cfg_desc, sizeof rig.cfg_desc);
  rig.pipe.rebind(rig.cfg, rig.dev_desc, rig.dev_len, rig.cfg_desc, rig.cfg_len);
  rig.dev.prepare();
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.hw.endpt(4) == 0u);
  CHECK(rig.hw.endpt(5) == 0u);
}

// OS-USBDISK U2a on the unit: the runtime hand-over (usb0::disk_layout_enter) re-enumerates the unit as DISK from AUDIO or STANDARD, with the host reading the DISK configuration the way Linux does (8 bytes, SET_ADDRESS, 18, the
// 9-byte header, the whole of it). The DISK set does not depend on the layout it comes from: the same bytes as the boot disk's (wTotalLength is what is sent), whatever profile and USB RETURN the AUDIO set had.
TEST_CASE("usb disk: the runtime DISK set from AUDIO or STANDARD enumerates, with the boot disk's descriptor bytes and a wTotalLength that is what is sent") {
  std::uint8_t boot[kMaxConfigDescriptorSize];
  Config disk;
  disk.mode = Mode::Disk;
  disk.speed = Speed::Full;
  const std::size_t boot_len = build_config_descriptor(disk, boot, sizeof boot);
  USB_REQUIRE(boot_len >= 9u);
  CHECK((boot[2] | (boot[3] << 8)) == static_cast<int>(boot_len));
  CHECK(boot_len <= 64u * 4u);  // several packets on endpoint 0, the last one short or closed by a ZLP
  for (const Mode from : {Mode::Audio, Mode::Standard}) {
    for (const Profile profile : {Profile::P8, Profile::P10, Profile::P2}) {
      for (const bool usb_return : {true, false}) {
        Rig& rig = fresh();
        rig.cfg.mode = from;
        rig.cfg.profile = profile;
        rig.cfg.usb_return = usb_return;
        rig.dev_len = build_device_descriptor(rig.cfg, rig.dev_desc, sizeof rig.dev_desc);
        rig.cfg_len = build_config_descriptor(rig.cfg, rig.cfg_desc, sizeof rig.cfg_desc);
        rig.pipe.rebind(rig.cfg, rig.dev_desc, rig.dev_len, rig.cfg_desc, rig.cfg_len);
        rig.dev.prepare();
        USB_REQUIRE(rig.enumerate());  // the first layout, configured (a host that has the device open)
        // The detach and the attach of usb0::attach(): the DISK set, built into the same buffers; the old layout's profile and USB RETURN stay in the Config, as queue_change leaves them.
        rig.cfg.mode = Mode::Disk;
        rig.dev_len = build_device_descriptor(rig.cfg, rig.dev_desc, sizeof rig.dev_desc);
        rig.cfg_len = build_config_descriptor(rig.cfg, rig.cfg_desc, sizeof rig.cfg_desc);
        USB_REQUIRE(rig.cfg_len == boot_len);
        USB_REQUIRE(std::memcmp(rig.cfg_desc, boot, boot_len) == 0);
        rig.pipe.rebind(rig.cfg, rig.dev_desc, rig.dev_len, rig.cfg_desc, rig.cfg_len);
        rig.dev.prepare();
        USB_REQUIRE(rig.enumerate(19));
        std::uint8_t whole[512];
        Host::Result r = rig.get_descriptor(2, 0, 0, 255, whole, sizeof whole);
        USB_REQUIRE(r.ok);
        CHECK(r.length == boot_len);
        CHECK(std::memcmp(whole, boot, boot_len) == 0);
        CHECK(rig.pipe.configured());
        CHECK(rig.dev.counters().tokens_stray == 0u);
        CHECK(rig.dev.counters().pingpong_resyncs == 0u);
      }
    }
  }
}

// OS-USBDISK U2a: the endpoint 0 trace (usb/ep0_trace.h) records a DISK enumeration in order, only while armed, and stops (does not wrap) when full.
TEST_CASE("usb ep0 trace: the events of an enumeration in order, setup fields, stall, address; nothing unarmed; the first 32 kept") {
  using Tag = Ep0Trace::Tag;
  Config disk;
  disk.mode = Mode::Disk;
  disk.speed = Speed::Full;
  Rig& rig = fresh(disk);
  Ep0Trace trace;
  rig.dev.bind_trace(&trace);
  rig.dev.prepare();  // unarmed: nothing recorded
  CHECK(trace.count() == 0u);
  trace.arm();
  rig.dev.prepare();  // the attach: the reset is the first event
  std::uint8_t buf[512];
  rig.hw.bus_reset();
  USB_REQUIRE(rig.get_descriptor(1, 0, 0, 8, buf, sizeof buf).ok);
  USB_REQUIRE(rig.set_address(19).ok);
  USB_REQUIRE(rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf).ok);
  CHECK_FALSE(rig.get_descriptor(6, 0, 0, 10, buf, sizeof buf).ok);  // the device qualifier: stalled
  USB_REQUIRE(rig.get_descriptor(2, 0, 0, 9, buf, sizeof buf).ok);
  USB_REQUIRE(trace.count() >= 20u);
  CHECK(trace.count() <= Ep0Trace::kEntries);
  CHECK(trace.entry(0).tag == static_cast<std::uint8_t>(Tag::Reset));
  CHECK(trace.entry(0).dt64 == 0u);
  // The first SETUP after the bus reset: GET_DESCRIPTOR(device) 8 bytes. bmRequestType 0x80, bRequest 6, wValue 0x0100 are the first four bytes.
  unsigned first_setup = 0;
  while (first_setup < trace.count() && trace.entry(first_setup).tag != static_cast<std::uint8_t>(Tag::Setup)) {
    ++first_setup;
  }
  USB_REQUIRE(first_setup + 2u < trace.count());
  CHECK(trace.entry(first_setup).w0 == 0x01000680u);
  CHECK(trace.entry(first_setup).bytes == 8u);
  CHECK(trace.entry(first_setup).extra == 0u);
  // The reply is armed as DATA1 from the control pipe's buffer, 8 bytes, and the controller finishes it.
  CHECK(trace.entry(first_setup + 1u).tag == static_cast<std::uint8_t>(Tag::ArmIn));
  CHECK(trace.entry(first_setup + 1u).bytes == 8u);
  CHECK((trace.entry(first_setup + 1u).flags & 4u) != 0u);
  CHECK((trace.entry(first_setup + 1u).flags & 2u) != 0u);
  CHECK(trace.entry(first_setup + 2u).tag == static_cast<std::uint8_t>(Tag::InDone));
  unsigned addresses = 0;
  unsigned stalls = 0;
  unsigned setups = 0;
  for (unsigned i = 0; i < trace.count(); ++i) {
    const Ep0Trace::Entry& e = trace.entry(i);
    addresses += e.tag == static_cast<std::uint8_t>(Tag::Address) && e.w0 == 19u ? 1u : 0u;
    stalls += e.tag == static_cast<std::uint8_t>(Tag::StallSet) || e.tag == static_cast<std::uint8_t>(Tag::StallSeen) ? 1u : 0u;
    setups += e.tag == static_cast<std::uint8_t>(Tag::Setup) ? 1u : 0u;
  }
  CHECK(addresses == 1u);
  CHECK(stalls == 2u);  // set by the control pipe, then the controller's stall handshake
  CHECK(setups == 5u);
  // Disarmed: the trace is kept as it is.
  const unsigned kept = trace.count();
  trace.disarm();
  USB_REQUIRE(rig.get_descriptor(2, 0, 0, 9, buf, sizeof buf).ok);
  CHECK(trace.count() == kept);
  // Full: the first entries stay, nothing wraps.
  trace.arm();
  for (unsigned i = 0; i < Ep0Trace::kEntries + 5u; ++i) {
    trace.record(Tag::Address, 1000u * (i + 1u), 0, i, 0, 0);
  }
  CHECK(trace.count() == Ep0Trace::kEntries);
  CHECK(trace.entry(0).w0 == 0u);
  CHECK(trace.entry(Ep0Trace::kEntries - 1u).w0 == Ep0Trace::kEntries - 1u);
  CHECK(trace.entry(1).dt64 == 1000u / 64u);
  // A long gap saturates.
  trace.arm();
  trace.record(Tag::Reset, 0u, 0, 0, 0, 0);
  trace.record(Tag::Reset, 0x10000000u, 0, 0, 0, 0);
  CHECK(trace.entry(1).dt64 == 0xFFFFu);
}

// OS-USBRET: with USB RETURN off the device is capture only. Endpoint 4 is the stems IN alone, endpoint 5 is not enabled, and the return interface does not exist (number 4 is the
// MIDI AudioControl), so no host request can start an OUT stream or the feedback loop.
TEST_CASE("usb iso: USB RETURN off - capture only, endpoint 5 off, the return cannot be started, the stems stream is the same") {
  Fixture f(Profile::P10, false);
  USB_REQUIRE(f.rig.enumerate());
  CHECK(f.rig.cfg_len == 293u);
  CHECK(f.rig.hw.endpt(4) == (kEndptTxEn | kEndptCtlDisable));  // isochronous IN only
  CHECK(f.rig.hw.endpt(5) == 0u);
  CHECK((f.rig.hw.endpt(2) & kEndptHandshake) != 0u);
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 1));
  CHECK(f.hook.in_active);
  CHECK(f.hook.channels == 10u);
  CHECK_FALSE(f.hook.out_active);
  for (unsigned i = 0; i < 3u; ++i) {
    f.engine.produce_block();
  }
  std::uint8_t buf[1024];
  std::uint32_t frames_seen = 0;
  for (unsigned frame = 0; frame < 200u; ++frame) {
    const int len = f.frame_in(buf, sizeof buf);
    USB_REQUIRE((len > 0 && len % 20 == 0));
    const std::uint32_t n = static_cast<std::uint32_t>(len) / 20u;
    CHECK((n == 44u || n == 45u));
    const std::int16_t* sm = reinterpret_cast<const std::int16_t*>(buf);
    if (sm[0] != 0) {
      for (std::uint32_t k = 0; k < n; ++k) {
        USB_REQUIRE(sm[k * 10u] == Engine::sample(frames_seen + k, 0));
      }
      frames_seen += n;
    }
    while (f.engine.blocks * kStemBlockFrames < frames_seen + 3u * kStemBlockFrames) {
      f.engine.produce_block();
    }
  }
  CHECK(frames_seen > 100u * 44u);
  CHECK(f.rig.audio.in_underruns == 0u);
  // A host that asks for what is not there is refused and nothing starts: the old return interface number is MIDI now.
  CHECK_FALSE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK_FALSE(f.hook.out_active);
  CHECK(f.rig.dev.counters().iso_out_starts == 0u);
  CHECK(f.rig.dev.counters().iso_fb_packets == 0u);
  std::uint8_t fb[4];
  CHECK(f.read_feedback(fb) == -1);  // nothing to read at endpoint 5
  USB_REQUIRE(f.set_alt(layout::kIfAudioIn, 0));
  CHECK_FALSE(f.hook.in_active);
  CHECK(f.rig.dev.counters().pingpong_resyncs == 0u);
}

TEST_CASE("usb iso: a layout change through ControlPipe::rebind - the return stream goes away and comes back with its endpoints") {
  Fixture f(Profile::P8, true);
  Rig& rig = f.rig;
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.cfg_len == 384u);
  CHECK(rig.hw.endpt(5) == (kEndptTxEn | kEndptCtlDisable));
  USB_REQUIRE(f.set_alt(layout::kIfAudioOut, 1));
  CHECK(f.hook.out_active);
  for (const bool usb_return : {false, true, false}) {
    rig.cfg.usb_return = usb_return;
    rig.dev_len = build_device_descriptor(rig.cfg, rig.dev_desc, sizeof rig.dev_desc);
    rig.cfg_len = build_config_descriptor(rig.cfg, rig.cfg_desc, sizeof rig.cfg_desc);
    rig.pipe.rebind(rig.cfg, rig.dev_desc, rig.dev_len, rig.cfg_desc, rig.cfg_len);
    rig.dev.prepare();  // (the bus reset the host sees after the detach)
    USB_REQUIRE(rig.enumerate());
    INFO("return " << usb_return);
    CHECK(rig.cfg_len == (usb_return ? 384u : 293u));
    CHECK(rig.hw.endpt(4) == (usb_return ? (kEndptRxEn | kEndptTxEn | kEndptCtlDisable) : (kEndptTxEn | kEndptCtlDisable)));
    CHECK(rig.hw.endpt(5) == (usb_return ? (kEndptTxEn | kEndptCtlDisable) : 0u));
    CHECK_FALSE(f.hook.out_active);  // a configuration event stopped the stream
    CHECK(f.set_alt(layout::kIfAudioOut, 1) == usb_return);
    CHECK(f.hook.out_active == usb_return);
    CHECK(f.set_alt(layout::kIfAudioIn, 1));
    CHECK(f.hook.in_active);
  }
}
