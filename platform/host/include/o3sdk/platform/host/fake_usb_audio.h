// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB port and the computer behind it, with no hardware: hal::usb_audio::Device for the host tests and the simulator (docs/M2C-PLAN.md section 7). The device is the unit's own
// audio::UsbAudioService (platform/mk66/audio), over the real StemTap, StemMap and usb::StemReader / ReturnRing / ReturnReader, so what the "computer" captures is what the
// unit's USB interrupt would put on the wire, to the sample; only the USB driver itself (the descriptors, the packets, the re-enumeration's timing) is modelled.
//
//   the layout    request() starts a re-enumeration that takes kReenumerateMs of the simulated time (tick()), then the set is the asked one, the stem map is o3-sdk's factory map of the profile
//                 (usb0's attach does the same) and the streams the computer had open are opened again. The boot layout is power_up()'s.
//   the computer  listen(true) opens the capture stream (it reads the stem ring a block at a time into captured(), channels() of them) and, with the return, play_tone / play_wav feed the
//                 return stream (a block of 64 frames a block, in packets of at most 48 frames, through the real OutStream) while the layout has it. Nothing streams until the scenario says so.
//   the disk      disk_share() refuses what the platform refuses (no card, the card in use: through the `busy` hook, a hand-over already running), then the card is the computer's after
//                 kShareMs (the card hook is told: the harness puts the card under the host's gate) until disk_eject() (kEjectMs) hands it back. The USB layout is not changed by the model.
//
// Not for the target: standard library allowed here (platform/host); nothing below the app layer includes this header. The harness calls tick() once a millisecond and wrap()
// around the render callback.

#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/usb_audio.h>
#include <o3sdk/platform/mk66/audio/stem_map.h>
#include <o3sdk/platform/mk66/audio/taps.h>
#include <o3sdk/platform/mk66/audio/usb_audio_service.h>
#include <o3sdk/usb/counters.h>
#include <o3sdk/usb/stream.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace o3sdk::platform::host {

class FakeUsbAudio final : private mk66::audio::UsbAudioService::Link {
 public:
  using Profile = hal::usb_audio::Profile;

  static constexpr std::uint32_t kReenumerateMs = 1000;  // the detach (120 + 700 ms) and the attach, with margin
  static constexpr std::uint32_t kShareMs = 2500;        // the hand-over: the USB set, the computer's configure, the flip
  static constexpr std::uint32_t kEjectMs = 500;
  static constexpr float kRate = 44100.0f;

  FakeUsbAudio()
      : pool_(std::make_unique<mk66::audio::StemPool>()),
        tap_(*pool_),
        service_(tap_, map_, *this),
        reader_(tap_.ring()),
        return_reader_(return_ring_, counters_),
        out_(return_ring_, counters_) {
    service_.attach_return(&return_reader_);
    power_up(true, Profile::P8, true);
  }

  hal::usb_audio::Device& device() noexcept { return service_; }
  mk66::audio::UsbAudioService& service() noexcept { return service_; }
  const mk66::audio::StemMap& map() const noexcept { return map_; }

  // ---- The unit ---------------------------------------------------------------------------------------------------------------------------
  // The card: `present` (a card is in the slot and the unit's), `give(true)` the card is the computer's now, `give(false)` it is the unit's again (the harness tells the runtime and gates the writer).
  void bind_card(std::function<bool()> present, std::function<void(bool)> give) {
    card_present_ = std::move(present);
    card_give_ = std::move(give);
  }
  // The unit starts: the port presents this layout (the settings' choice), the factory map, no stream open, the card the unit's.
  void power_up(bool audio, Profile profile, bool duplex) {
    if (fall_back_) {  // usb0::start() refused the layout (the saved profile): the boot profile P8 in AUDIO, duplex
      audio = true;
      profile = Profile::P8;
      duplex = true;
    }
    audio_ = audio;
    profile_ = profile;
    duplex_ = duplex;
    changing_ = false;
    disk_ = hal::usb_audio::DiskPhase::Idle;
    listening_ = false;
    playing_ = false;
    mk66::audio::factory_map(map_, mk66::audio::to_usb(profile));
    map_.apply(tap_);
    apply_streams();
    return_ring_.reset();
    clear_capture();
  }
  // The driver refuses whatever layout it is started with and comes up in the boot layout (AUDIO, P8, duplex), as usb0::start() does for a profile it cannot feed: from the next power_up().
  void fall_back(bool on) noexcept { fall_back_ = on; }
  // Once a millisecond of simulated time.
  void tick(std::uint32_t now_ms) {
    now_ms_ = now_ms;
    if (changing_ && static_cast<std::int32_t>(now_ms - change_at_) >= 0) {
      changing_ = false;
      audio_ = to_audio_;
      profile_ = to_profile_;
      duplex_ = to_duplex_;
      mk66::audio::factory_map(map_, mk66::audio::to_usb(profile_));  // usb0's attach
      map_.apply(tap_);
      apply_streams();
      clear_capture();  // a new set: the stream starts over (a capture of the old one is another file)
      ++reconnects_;
    }
    if (disk_ == hal::usb_audio::DiskPhase::Working && static_cast<std::int32_t>(now_ms - disk_at_) >= 0) {
      disk_ = disk_to_out_ ? hal::usb_audio::DiskPhase::Out : hal::usb_audio::DiskPhase::Idle;
      if (card_give_) {
        card_give_(disk_to_out_);
      }
    }
  }

  // The render callback with the unit's service in it and the computer's side around it: `inner` is what the app renders (the record service's wrap around the runtime's block).
  hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, dsp::Engine& engine) {
    const hal::audio::BlockCallback service = service_.wrap(inner, engine);
    chain_ = service;
    return hal::audio::BlockCallback{&FakeUsbAudio::block, this};
  }

  // ---- The computer -----------------------------------------------------------------------------------------------------------------------
  // The computer opens or closes the capture stream.
  void listen(bool on) {
    listening_ = on;
    reader_.restart();
    clear_capture();
    apply_streams();
  }
  bool listening() const noexcept { return listening_; }
  // What the computer plays to the unit (the return stream: open while the layout has it). `dbfs` -96..0.
  void play_tone(float hz, float dbfs = -12.0f) {
    tone_hz_ = hz;
    tone_amp_ = std::pow(10.0f, dbfs / 20.0f);
    wav_.clear();
    start_play(Source::Tone);
  }
  // Interleaved stereo int16, played in a loop.
  void play_wav(std::vector<std::int16_t> lr) {
    wav_ = std::move(lr);
    wav_pos_ = 0;
    start_play(wav_.size() >= 2 ? Source::Wav : Source::Off);
  }
  // A 16-bit PCM WAV, mono or stereo. false (and `error`): not that.
  bool play_wav_file(const std::string& path, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
      error = "cannot read " + path;
      return false;
    }
    std::vector<std::uint8_t> b;
    std::uint8_t buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) != 0) {
      b.insert(b.end(), buf, buf + n);
    }
    std::fclose(f);
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint32_t>(b[at] | (b[at + 1] << 8)); };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    if (b.size() < 44 || std::string(reinterpret_cast<const char*>(b.data()), 4) != "RIFF" || std::string(reinterpret_cast<const char*>(b.data()) + 8, 4) != "WAVE") {
      error = path + " is not a WAV file";
      return false;
    }
    std::uint32_t channels = 0;
    std::uint32_t bits = 0;
    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
      const std::uint32_t len = u32(pos + 4);
      const std::string id(reinterpret_cast<const char*>(b.data()) + pos, 4);
      if (id == "fmt " && pos + 8 + 16 <= b.size()) {
        channels = u16(pos + 10);
        bits = u16(pos + 22);
      } else if (id == "data") {
        if ((channels != 1u && channels != 2u) || bits != 16u) {
          error = path + ": only 16-bit mono or stereo PCM is played";
          return false;
        }
        const std::size_t avail = std::min<std::size_t>(len, b.size() - pos - 8);
        std::vector<std::int16_t> lr;
        for (std::size_t i = 0; i + 2u * channels <= avail; i += 2u * channels) {
          const std::size_t at = pos + 8 + i;
          const auto l = static_cast<std::int16_t>(u16(at));
          lr.push_back(l);
          lr.push_back(channels == 2u ? static_cast<std::int16_t>(u16(at + 2)) : l);
        }
        play_wav(std::move(lr));
        return true;
      }
      pos += 8 + len + (len & 1u);
    }
    error = path + " has no audio";
    return false;
  }
  void play_off() { start_play(Source::Off); }

  // What the computer captured since the stream was opened: interleaved, channels() of them (the layout's), as the stem ring delivered it.
  const std::vector<std::int16_t>& captured() const noexcept { return captured_; }
  unsigned channels() const noexcept { return hal::usb_audio::profile_channels(profile_); }
  void clear_capture() { captured_.clear(); }
  std::uint32_t reconnects() const noexcept { return reconnects_; }
  const usb::Counters& counters() const noexcept { return counters_; }
  // The capture as a PCM WAV of channels() channels at 44.1 kHz.
  bool write_wav(const std::string& path) const {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
      return false;
    }
    const std::uint32_t ch = channels();
    const std::uint32_t bytes = static_cast<std::uint32_t>(captured_.size() * 2u);
    const auto put32 = [&](std::uint32_t v) {
      const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
      std::fwrite(b, 1, 4, f);
    };
    const auto put16 = [&](std::uint16_t v) {
      const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
      std::fwrite(b, 1, 2, f);
    };
    std::fwrite("RIFF", 1, 4, f);
    put32(36u + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(static_cast<std::uint16_t>(ch));
    put32(44100);
    put32(44100u * ch * 2u);
    put16(static_cast<std::uint16_t>(ch * 2u));
    put16(16);
    std::fwrite("data", 1, 4, f);
    put32(bytes);
    for (const std::int16_t s : captured_) {
      put16(static_cast<std::uint16_t>(s));
    }
    return std::fclose(f) == 0;
  }

 private:
  enum class Source : std::uint8_t { Off, Tone, Wav };

  // ---- the render, with the computer around it --------------------------------------------------------------------------------------------
  static void block(void* user, const hal::audio::BlockIo& io) noexcept {
    FakeUsbAudio& self = *static_cast<FakeUsbAudio*>(user);
    self.host_plays();
    self.chain_.fn(self.chain_.user, io);
    self.host_captures();
  }
  // The computer's OUT stream: one block of the return per block, in packets of at most kReturnMaxChunk frames.
  void host_plays() {
    if (!out_open() || source_ == Source::Off) {
      return;
    }
    alignas(4) std::uint32_t frames[hal::audio::kBlock];
    for (std::uint32_t i = 0; i < hal::audio::kBlock; ++i) {
      std::int16_t l = 0;
      std::int16_t r = 0;
      if (source_ == Source::Tone) {
        const float v = std::sin(phase_) * tone_amp_;
        l = r = static_cast<std::int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f));
        phase_ += 6.2831853f * tone_hz_ / kRate;
        if (phase_ > 6.2831853f) {
          phase_ -= 6.2831853f;
        }
      } else {
        l = wav_[wav_pos_];
        r = wav_[wav_pos_ + 1u];
        wav_pos_ = (wav_pos_ + 2u) % wav_.size();
      }
      frames[i] = static_cast<std::uint16_t>(l) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(r)) << 16);
    }
    for (std::uint32_t at = 0; at < hal::audio::kBlock; at += usb::kReturnMaxChunk) {
      const std::uint32_t n = std::min<std::uint32_t>(usb::kReturnMaxChunk, hal::audio::kBlock - at);
      out_.on_packet(frames + at, n * 4u);
    }
  }
  // The computer's IN stream: what the ring holds is read, whole frames of channels().
  void host_captures() {
    if (!in_open()) {
      return;
    }
    const unsigned ch = channels();
    while (reader_.fill_frames() != 0u) {
      alignas(4) std::int16_t out[hal::audio::kBlock * usb::kMaxChannels];
      const std::uint32_t want = std::min<std::uint32_t>(reader_.fill_frames(), hal::audio::kBlock);
      const std::uint32_t got = reader_.read_interleaved(out, want, ch);
      captured_.insert(captured_.end(), out, out + got * ch);
      if (got == 0u) {
        break;
      }
    }
  }

  bool in_open() const noexcept { return listening_ && audio_ && !changing_; }
  bool out_open() const noexcept { return playing_ && listening_ && audio_ && duplex_ && !changing_; }
  void start_play(Source s) {
    source_ = s;
    playing_ = s != Source::Off;
    phase_ = 0.0f;
    return_ring_.reset();
    return_reader_.request_restart();
    apply_streams();
  }
  // The hook usb0's interrupt calls when the host selects or drops an alternate setting.
  void apply_streams() {
    service_.streams_changed(in_open(), out_open());
    if (in_open()) {
      reader_.restart();
    }
  }

  // ---- UsbAudioService::Link --------------------------------------------------------------------------------------------------------------
  bool ready() const noexcept override { return true; }
  bool audio() const noexcept override { return changing_ ? to_audio_ : audio_; }
  Profile profile() const noexcept override { return changing_ ? to_profile_ : profile_; }
  bool duplex() const noexcept override { return changing_ ? to_duplex_ : duplex_; }
  bool changing() const noexcept override { return changing_ || disk_ != hal::usb_audio::DiskPhase::Idle; }
  bool request(bool audio, Profile profile, bool duplex) noexcept override {
    if (changing_ || disk_ != hal::usb_audio::DiskPhase::Idle) {
      return false;
    }
    if (audio == audio_ && profile == profile_ && duplex == duplex_) {
      return true;
    }
    if (!audio && !audio_) {  // STANDARD to STANDARD: nothing to re-enumerate, the next AUDIO attach uses it
      profile_ = profile;
      duplex_ = duplex;
      return true;
    }
    changing_ = true;
    to_audio_ = audio;
    to_profile_ = profile;
    to_duplex_ = duplex;
    change_at_ = now_ms_ + kReenumerateMs;
    apply_streams();  // the streams stop at once
    return true;
  }
  hal::usb_audio::DiskStatus disk() const noexcept override {
    hal::usb_audio::DiskStatus d;
    d.available = true;
    d.phase = disk_;
    d.card_present = card_present_ ? card_present_() : false;
    d.busy = busy_ ? busy_() : false;
    d.writable = !disk_ro_;
    d.audio = audio_;
    return d;
  }
  hal::usb_audio::DiskRefusal disk_share(bool read_only) noexcept override {
    if (disk_ != hal::usb_audio::DiskPhase::Idle) {
      return hal::usb_audio::DiskRefusal::NotIdle;
    }
    if (changing_) {
      return hal::usb_audio::DiskRefusal::Layout;
    }
    if (!(card_present_ ? card_present_() : false)) {
      return hal::usb_audio::DiskRefusal::NoCard;
    }
    if (busy_ && busy_()) {
      return hal::usb_audio::DiskRefusal::Busy;
    }
    disk_ro_ = read_only;
    disk_ = hal::usb_audio::DiskPhase::Working;
    disk_to_out_ = true;
    disk_at_ = now_ms_ + kShareMs;
    return hal::usb_audio::DiskRefusal::None;
  }
  bool disk_eject() noexcept override {
    if (disk_ != hal::usb_audio::DiskPhase::Out) {
      return false;
    }
    disk_ = hal::usb_audio::DiskPhase::Working;
    disk_to_out_ = false;
    disk_at_ = now_ms_ + kEjectMs;
    return true;
  }

 public:
  // Something else holds the card (the harness: the files service has a job): the share is refused with Busy.
  void bind_busy(std::function<bool()> busy) { busy_ = std::move(busy); }

 private:
  std::unique_ptr<mk66::audio::StemPool> pool_;
  mk66::audio::StemTap tap_;
  mk66::audio::StemMap map_;
  mk66::audio::UsbAudioService service_;
  usb::StemReader reader_;
  usb::Counters counters_;
  usb::ReturnRing return_ring_;
  usb::ReturnReader return_reader_;
  usb::OutStream out_;
  hal::audio::BlockCallback chain_{nullptr, nullptr};
  std::function<bool()> card_present_;
  std::function<void(bool)> card_give_;
  std::function<bool()> busy_;
  std::uint32_t now_ms_ = 0;
  bool fall_back_ = false;
  // the port
  bool audio_ = true;
  Profile profile_ = Profile::P8;
  bool duplex_ = true;
  bool changing_ = false;
  bool to_audio_ = true;
  Profile to_profile_ = Profile::P8;
  bool to_duplex_ = true;
  std::uint32_t change_at_ = 0;
  std::uint32_t reconnects_ = 0;
  // the disk
  hal::usb_audio::DiskPhase disk_ = hal::usb_audio::DiskPhase::Idle;
  bool disk_to_out_ = false;
  bool disk_ro_ = true;
  std::uint32_t disk_at_ = 0;
  // the computer
  bool listening_ = false;
  bool playing_ = false;
  Source source_ = Source::Off;
  float tone_hz_ = 1000.0f;
  float tone_amp_ = 0.25f;
  float phase_ = 0.0f;
  std::vector<std::int16_t> wav_;
  std::size_t wav_pos_ = 0;
  std::vector<std::int16_t> captured_;
};

}  // namespace o3sdk::platform::host
