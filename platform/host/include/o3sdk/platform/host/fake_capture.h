// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The codec's input and the FM radio with no hardware behind them: hal::capture::CaptureHw for the host tests and the simulator (docs/M2C-PLAN.md section 5).
// It records what the kit asks for, scales what the "codec hears" by the gain the kit set, and plays a receiver that finds the stations it is told about. It is also the
// reference model of what a real device (platform/mk66's capture.cpp over the Si4703 driver) promises the kit (hal/capture.h).
//
//   the world   input_tone(hz, dbfs) / input_wav(...) / input_wav_file(path) / input_off(): what is at the line jack and the microphone (the same signal at both;
//               silence by default). radio_stations({8870, 9370, ...}) is what the air holds (10 kHz units); a tuned station is a sine at freq / 10 Hz (9370 is 937 Hz,
//               -12 dBFS, in both channels) while the source is Radio, anything else is silence. radio_absent() is a board with no receiver: the power-up ends in Absent.
//   the ADC     read_block(lr, frames) is what the codec delivers for the next block (interleaved int16, the engine's input): the world through the source in use and its
//               gain (the line 0 .. 22.5 dB in 1.5 dB steps of gain * 15 / 100, the microphone 0 .. 25 dB or 0 .. 44 dB of gain * 25 / 100 and gain * 44 / 100), clipped to 16
//               bits. Source::Usb hears nothing here (the return is the USB layer's).
//   the radio   follows the driver's states and its times (hal/capture.h RadioStatus): power() -> Starting for kStartMs -> Ready (or Absent after kProbeMs), tune() ->
//               Tuning for kTuneMs, seek() -> Seeking for kSeekMs, then the nearest station in that direction (wrapping at the band's end; none: band_limit and the same
//               frequency). Requests made while it is starting, tuning or seeking are held (the last one wins). All of it advances in service(now_ms), which the platform's main
//               loop (the harness) calls once a pass.
//   refusals    refuse_writes(n): the next n calls of select_source() and set_gain() answer false (a codec that does not acknowledge), to test the retry.
//
// Not for the target: standard library allowed here (platform/host); nothing below the app layer includes this header.

#include <o3sdk/core/audio_clock.h>
#include <o3sdk/hal/capture.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace o3sdk::platform::host {

class FakeCapture final : public hal::capture::CaptureHw {
 public:
  using Source = hal::capture::Source;
  using Region = hal::capture::Region;
  using Status = hal::capture::RadioStatus;

  static constexpr std::uint32_t kStartMs = 700;   // the driver's power-up (OS-RAD1: about 0.7 s)
  static constexpr std::uint32_t kProbeMs = 20;    // a chip that does not answer says so after a few passes
  static constexpr std::uint32_t kTuneMs = 80;
  static constexpr std::uint32_t kSeekMs = 400;

  // ---- The world ------------------------------------------------------------------------------------------------------------------------
  void input_tone(float hz, float dbfs = -12.0f) {
    tone_hz_ = hz;
    tone_amp_ = std::pow(10.0f, dbfs / 20.0f);
    wav_.clear();
    world_ = World::Tone;
  }
  // Interleaved stereo int16, played in a loop.
  void input_wav(std::vector<std::int16_t> lr) {
    wav_ = std::move(lr);
    wav_pos_ = 0;
    world_ = wav_.size() >= 2 ? World::Wav : World::Off;
  }
  // A 16-bit PCM WAV, mono or stereo. false (and `error`): not that.
  bool input_wav_file(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      error = "cannot read " + path;
      return false;
    }
    const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(b[at]) | (static_cast<unsigned>(b[at + 1]) << 8); };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    if (b.size() < 12 || std::string(b.begin(), b.begin() + 4) != "RIFF" || std::string(b.begin() + 8, b.begin() + 12) != "WAVE") {
      error = path + ": not a RIFF / WAVE file";
      return false;
    }
    unsigned channels = 0;
    bool pcm16 = false;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
      const std::string id(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at) + 4);
      const std::size_t size = u32(at + 4);
      const std::size_t body = at + 8;
      if (id == "fmt " && size >= 16 && body + 16 <= b.size()) {
        pcm16 = u16(body) == 1u && u16(body + 14) == 16u;
        channels = u16(body + 2);
      } else if (id == "data") {
        if (!pcm16 || (channels != 1u && channels != 2u)) {
          error = path + ": only 16-bit PCM, mono or stereo";
          return false;
        }
        const std::size_t n = std::min(size, b.size() - body) / 2u;
        std::vector<std::int16_t> lr;
        for (std::size_t i = 0; i + (channels - 1u) < n; i += channels) {
          const auto l = static_cast<std::int16_t>(u16(body + 2u * i));
          const auto r = channels == 2u ? static_cast<std::int16_t>(u16(body + 2u * (i + 1u))) : l;
          lr.push_back(l);
          lr.push_back(r);
        }
        if (lr.empty()) {
          error = path + ": no audio";
          return false;
        }
        input_wav(std::move(lr));
        return true;
      }
      at = body + size + (size & 1u);
    }
    error = path + ": no data chunk";
    return false;
  }
  void input_off() {
    world_ = World::Off;
    wav_.clear();
  }
  void radio_stations(std::vector<std::uint16_t> freqs) {
    stations_ = std::move(freqs);
    std::sort(stations_.begin(), stations_.end());
  }
  void radio_absent(bool absent = true) { absent_ = absent; }
  // The next `n` select_source() / set_gain() calls are refused.
  void refuse_writes(unsigned n) { refuse_ = n; }

  // ---- What the kit did ----------------------------------------------------------------------------------------------------------------
  unsigned selects() const { return selects_; }          // select_source() calls
  unsigned gains() const { return gains_; }              // set_gain() calls
  unsigned refused() const { return refused_; }          // of those, refused
  unsigned power_calls() const { return power_calls_; }  // radio_power(true) calls
  unsigned seeks() const { return seeks_; }
  unsigned tunes() const { return tunes_; }
  Source source() const { return source_; }              // what the codec is pointed at
  unsigned gain() const { return gain_; }                // its gain, per cent
  bool mux_radio() const { return source_ == Source::Radio; }  // the analogue switch's position
  Region region() const { return status_.region; }

  // ---- The codec's ADC ------------------------------------------------------------------------------------------------------------------
  // The next `frames` frames of the input, interleaved L R int16 into `lr`.
  void read_block(std::int16_t* lr, unsigned frames) noexcept {
    const float g = gain_linear();
    for (unsigned i = 0; i < frames; ++i) {
      float l = 0.0f;
      float r = 0.0f;
      if (source_ == Source::Radio) {
        if (radio_tone_hz() > 0.0f) {
          l = r = std::sin(phase_) * kStationAmp;
          phase_ = advance(phase_, radio_tone_hz());
        }
      } else if (source_ != Source::Usb) {
        if (world_ == World::Tone) {
          l = r = std::sin(phase_) * tone_amp_ * g;
          phase_ = advance(phase_, tone_hz_);
        } else if (world_ == World::Wav) {
          l = static_cast<float>(wav_[wav_pos_]) * (1.0f / 32768.0f) * g;
          r = static_cast<float>(wav_[wav_pos_ + 1u]) * (1.0f / 32768.0f) * g;
          wav_pos_ = (wav_pos_ + 2u) % wav_.size();
        }
      }
      lr[2u * i] = to_s16(l);
      lr[2u * i + 1u] = to_s16(r);
    }
  }

  // ---- hal::capture::CaptureHw ----------------------------------------------------------------------------------------------------------
  bool select_source(Source source, std::uint8_t gain_percent) noexcept override {
    ++selects_;
    if (refuse()) {
      return false;
    }
    source_ = source;
    gain_ = gain_percent > 100u ? 100u : gain_percent;
    if (source == Source::Radio) {
      radio_power(true);  // the receiver is powered when it is first wanted
    }
    return true;
  }
  bool set_gain(Source source, std::uint8_t gain_percent) noexcept override {
    ++gains_;
    if (refuse()) {
      return false;
    }
    if (source == source_) {
      gain_ = gain_percent > 100u ? 100u : gain_percent;
    }
    return true;
  }
  void radio_power(bool on) noexcept override {
    if (on) {
      ++power_calls_;
      const Status::State s = status_.state;
      if (s == Status::State::Off || s == Status::State::Absent || s == Status::State::Failed) {
        want_tune_ = false;
        want_seek_ = false;
        status_.fail[0] = '\0';
        status_.freq = static_cast<std::uint16_t>(hal::capture::snap_freq(status_.region, status_.freq));
        due_valid_ = false;
        set_state(Status::State::Starting);
      }
      return;
    }
    set_state(Status::State::Off);
    want_tune_ = false;
    want_seek_ = false;
  }
  void radio_region(Region region) noexcept override {
    if (region == status_.region || static_cast<unsigned>(region) >= hal::capture::kRegionCount) {
      return;
    }
    status_.region = region;
    status_.freq = static_cast<std::uint16_t>(hal::capture::snap_freq(region, status_.freq));
    want_freq_ = status_.freq;
    touch();
    if (alive()) {
      want_tune_ = true;
    }
  }
  void radio_tune(std::uint16_t freq) noexcept override {
    ++tunes_;
    const auto f = static_cast<std::uint16_t>(hal::capture::snap_freq(status_.region, freq));
    want_freq_ = f;
    status_.freq = f;
    touch();
    want_seek_ = false;
    if (status_.state != Status::State::Off && status_.state != Status::State::Absent) {
      want_tune_ = true;
    }
  }
  void radio_seek(bool up) noexcept override {
    ++seeks_;
    if (status_.state == Status::State::Off || status_.state == Status::State::Absent) {
      return;
    }
    want_seek_ = true;
    seek_up_ = up;
    want_tune_ = false;
  }
  void service(std::uint64_t now_ms) noexcept override {
    now_ = now_ms;
    if (!due_valid_ && status_.state == Status::State::Starting) {
      due_ = now_ + (absent_ ? kProbeMs : kStartMs);
      due_valid_ = true;
      return;
    }
    switch (status_.state) {
      case Status::State::Starting:
        if (now_ >= due_) {
          due_valid_ = false;
          if (absent_) {
            status_.fail[0] = 'P';
            status_.fail[1] = 'B';
            status_.fail[2] = '/';
            status_.fail[3] = 'N';
            status_.fail[4] = '\0';
            set_state(Status::State::Absent);
          } else {
            status_.chip_id = 0x1253;  // an Si4703's DEVICEID
            set_state(Status::State::Ready);
            update_signal();
          }
        }
        break;
      case Status::State::Tuning:
        if (now_ >= due_) {
          due_valid_ = false;
          set_state(Status::State::Ready);
          update_signal();
        }
        break;
      case Status::State::Seeking:
        if (now_ >= due_) {
          due_valid_ = false;
          finish_seek();
          set_state(Status::State::Ready);
          update_signal();
        }
        break;
      case Status::State::Ready:
        if (want_tune_) {
          due_ = now_ + kTuneMs;
          due_valid_ = true;
          want_tune_ = false;
          status_.freq = want_freq_;
          set_state(Status::State::Tuning);
        } else if (want_seek_) {
          due_ = now_ + kSeekMs;
          due_valid_ = true;
          want_seek_ = false;
          status_.band_limit = false;
          set_state(Status::State::Seeking);
        }
        break;
      case Status::State::Off:
      case Status::State::Absent:
      case Status::State::Failed:
        break;
    }
  }
  Status radio_status() const noexcept override { return status_; }

 private:
  enum class World : std::uint8_t { Off, Tone, Wav };
  static constexpr float kStationAmp = 0.25118864f;  // -12 dBFS
  static constexpr float kTwoPi = 6.28318530718f;

  bool refuse() noexcept {
    if (refuse_ == 0u) {
      return false;
    }
    --refuse_;
    ++refused_;
    return true;
  }
  bool alive() const noexcept { return status_.state != Status::State::Off && status_.state != Status::State::Absent && status_.state != Status::State::Failed; }
  void touch() noexcept { ++status_.generation; }
  void set_state(Status::State s) noexcept {
    if (status_.state != s) {
      status_.state = s;
      touch();
    }
  }
  float advance(float phase, float hz) const noexcept {
    phase += kTwoPi * hz / core::kRefRateHz;
    return phase >= kTwoPi ? phase - kTwoPi : phase;
  }
  static std::int16_t to_s16(float v) noexcept {
    const float s = v * 32768.0f;
    return static_cast<std::int16_t>(s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s));
  }
  float gain_linear() const noexcept {
    float db = 0.0f;
    switch (source_) {
      case Source::LineMix:
      case Source::LineLeft:
      case Source::LineRight:
      case Source::Radio: db = static_cast<float>(gain_ * 15u / 100u) * 1.5f; break;
      case Source::MicLow: db = static_cast<float>(gain_ * 25u / 100u); break;
      case Source::MicHigh: db = static_cast<float>(gain_ * 44u / 100u); break;
      case Source::Usb:
      case Source::Count: break;
    }
    return std::pow(10.0f, db / 20.0f);
  }
  // The tone of the tuned station (0: none under the dial, or the receiver is not listening).
  float radio_tone_hz() const noexcept {
    if (status_.state != Status::State::Ready) {
      return 0.0f;
    }
    return std::binary_search(stations_.begin(), stations_.end(), status_.freq) ? static_cast<float>(status_.freq) * 0.1f : 0.0f;
  }
  void update_signal() noexcept {
    const bool on_station = std::binary_search(stations_.begin(), stations_.end(), status_.freq);
    const std::uint8_t rssi = on_station ? 40u : 5u;
    if (rssi != status_.rssi) {
      status_.rssi = rssi;
      touch();
    }
  }
  // The nearest listed station past the dial in the seek's direction on a channel of the region, wrapping; none: the band limit and the same frequency.
  void finish_seek() noexcept {
    const unsigned here = status_.freq;
    unsigned best = 0;
    bool found = false;
    unsigned wrap_best = 0;
    bool wrap_found = false;
    for (const std::uint16_t s : stations_) {
      const unsigned f = hal::capture::snap_freq(status_.region, s);
      if (f == here) {
        continue;
      }
      if (seek_up_ ? f > here : f < here) {
        if (!found || (seek_up_ ? f < best : f > best)) {
          best = f;
          found = true;
        }
      } else if (!wrap_found || (seek_up_ ? f < wrap_best : f > wrap_best)) {
        wrap_best = f;
        wrap_found = true;
      }
    }
    status_.band_limit = !(found || wrap_found);
    if (found || wrap_found) {
      status_.freq = static_cast<std::uint16_t>(found ? best : wrap_best);
    }
    want_freq_ = status_.freq;
    touch();
  }

  // The world and the codec.
  World world_ = World::Off;
  float tone_hz_ = 440.0f;
  float tone_amp_ = 0.25f;
  std::vector<std::int16_t> wav_;
  std::size_t wav_pos_ = 0;
  float phase_ = 0.0f;
  Source source_ = Source::LineMix;
  unsigned gain_ = 0;
  unsigned refuse_ = 0;
  // What the kit did.
  unsigned selects_ = 0;
  unsigned gains_ = 0;
  unsigned refused_ = 0;
  unsigned power_calls_ = 0;
  unsigned seeks_ = 0;
  unsigned tunes_ = 0;
  // The receiver.
  Status status_;
  std::vector<std::uint16_t> stations_;
  bool absent_ = false;
  bool want_tune_ = false;
  bool want_seek_ = false;
  bool seek_up_ = true;
  std::uint16_t want_freq_ = 8750;
  std::uint64_t now_ = 0;
  std::uint64_t due_ = 0;
  bool due_valid_ = false;
};

}  // namespace o3sdk::platform::host
