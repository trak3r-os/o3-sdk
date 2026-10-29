// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// An app's USB audio (docs/M2C-PLAN.md section 7): what Context::usb_audio() hands the app on top of the platform's hal::usb_audio::Device (Platform::usb_audio; null on a platform or a
// test rig without one: then every call is refused, available() is false, the system Config has no USB rows and no CARD AS DISK, and the runtime and the render are as they were).
//
//   The layout. request(audio, profile, duplex) asks the USB port for the AUDIO set of a profile (P2, P8, P10, P11; the master pair is always the last two channels) with or without the
//   stereo return stream, or for STANDARD (console and MIDI only). The port drops and comes back about a second later. The image boots in AUDIO, P8, duplex: the master on channels 7 and 8
//   and silence on 1..6, as track3r-os's, so a DAW's routing is unchanged. The kit keeps the choice with its settings and starts the port with it at the next boot.
//
//   The stems. set_stem(channel, sources) makes a channel of the profile (0-based, below profile_channels()) the saturating sum of the stem sources its mask names (hal::usb_audio:
//   track_source(1..12), kSourceMasterL/R, kSourceSendA/B; the engine's slots 1..8 are T1..T8: post-fader, pre-pan, mono). A profile change gives the profile's factory slots (P10: T1..T8, P11:
//   T1..T9, P8 and P2: none); an edit is kept with the settings (and dropped again by a profile change). The engine stages a source only while the computer has the capture stream open and a slot
//   names it: an app that never streams stems pays nothing in the render.
//
//   The return. What the computer plays is added to the output after the render at return_level() (per cent, linear in amplitude, 100 = as sent; 0 by default: the return is drained, not heard, until the user raises RETURN LEVEL), outside the master inserts, and never captured. With
//   the audio in's source Usb (AudioIn) it is also the engine's input: a unit, a hook or the recorder can take it.
//
//   The disk. share_disk() hands the card to the computer as a USB disk, never at boot (kit settings that are due are written first: the hand-over waits for them, up to kShareWaitMs): the port re-enumerates with a mass-storage function, the files service says Card::OnComputer
//   (every save and load is refused with "CARD ON COMPUTER - EJECT FIRST", the kit's settings stay dirty) until eject_disk() or the computer's own eject, then the card is mounted again and the
//   previous layout is back. Refused while a files job runs (Busy). set_read_only() chooses, while the disk is not out, whether the computer gets a write-protected disk (the default: it does).
//   The state is not kept with the settings.
//
// Main loop only; the render and the USB interrupt see only the platform's atomics.

#include <o3sdk/app/files.h>
#include <o3sdk/hal/usb_audio.h>

#include <cstdint>

namespace o3sdk::app {

class Runtime;
struct KitValues;

class UsbAudio {
 public:
  using Profile = hal::usb_audio::Profile;
  static constexpr unsigned kLevelDefault = 0;  // per cent: the verified boot behaviour (the return drained, not heard)
  static constexpr unsigned kMaxChannels = hal::usb_audio::kMaxChannels;
  static constexpr std::uint32_t kShareWaitMs = 3000;  // the kit's settings write that a share waits for (the debounce is cut short, so a write takes a few passes)

  // The platform has a USB audio device. false: every request is refused.
  bool available() const noexcept { return dev_ != nullptr; }

  // ---- The layout ------------------------------------------------------------------------------------------------------------------------
  // The layout presented now, or the one a change is on its way to; before the driver runs, the one the settings chose (what the port will start with).
  bool audio() const noexcept { return dev_ != nullptr && dev_->ready() ? dev_->audio() : audio_; }
  Profile profile() const noexcept { return dev_ != nullptr && dev_->ready() ? dev_->profile() : profile_; }
  bool duplex() const noexcept { return dev_ != nullptr && dev_->ready() ? dev_->duplex() : duplex_; }
  // A re-enumeration is on its way (the port is about to drop, or is away).
  bool changing() const noexcept { return dev_ != nullptr && dev_->changing(); }
  // true: it is presented already or the change is on its way. false: refused for now (a change is running, the card is out, no device); nothing changed.
  bool request(bool audio, Profile profile, bool duplex) noexcept;
  bool request_profile(Profile profile, bool duplex) noexcept { return request(audio(), profile, duplex); }

  // ---- The stems -------------------------------------------------------------------------------------------------------------------------
  bool set_stem(unsigned channel, std::uint32_t sources) noexcept;
  std::uint32_t stem(unsigned channel) const noexcept { return dev_ != nullptr ? dev_->stem(channel) : 0u; }
  // The factory slots of the profile; drops the kept edits.
  void reset_stems() noexcept;
  // The computer has the capture stream open.
  bool streaming() const noexcept { return dev_ != nullptr && dev_->streaming(); }

  // ---- The return ------------------------------------------------------------------------------------------------------------------------
  unsigned return_level() const noexcept { return level_; }
  void set_return_level(unsigned percent) noexcept;
  bool return_streaming() const noexcept { return dev_ != nullptr && dev_->return_streaming(); }

  // ---- The disk --------------------------------------------------------------------------------------------------------------------------
  hal::usb_audio::DiskStatus disk() const noexcept {
    hal::usb_audio::DiskStatus d = dev_ != nullptr ? dev_->disk() : hal::usb_audio::DiskStatus{};
    if (share_wait_ && d.phase == hal::usb_audio::DiskPhase::Idle) {
      d.phase = hal::usb_audio::DiskPhase::Working;  // the settings are being written first
    }
    return d;
  }
  bool read_only() const noexcept { return read_only_; }
  // Only while the disk is not out and no hand-over runs; true: taken.
  bool set_read_only(bool on) noexcept;
  // None: taken (the hand-over runs, or waits for the settings write first; a refusal the platform makes after the wait comes from take_share_refusal()).
  hal::usb_audio::DiskRefusal share_disk() noexcept;
  // The platform's refusal of a share that waited for the settings, once (None when there is none).
  hal::usb_audio::DiskRefusal take_share_refusal() noexcept {
    const hal::usb_audio::DiskRefusal r = refusal_;
    refusal_ = hal::usb_audio::DiskRefusal::None;
    return r;
  }
  bool eject_disk() noexcept { return dev_ != nullptr && dev_->disk_eject(); }

  // The USB layout the port starts with at boot, from the settings read before it: what the platform's main passes to its driver. `audio` false is STANDARD.
  struct Layout {
    bool audio;
    Profile profile;
    bool duplex;
  };
  Layout boot_layout() const noexcept { return Layout{audio_, profile_, duplex_}; }

 private:
  friend class Runtime;
  void bind(hal::usb_audio::Device* dev, Files* files) noexcept {
    dev_ = dev;
    files_ = files;
  }
  // Every pass of the main loop: once the driver runs, the kept return level and edited slots go to it; after that the kit's copy of the layout follows the device (the console's `usb profile`
  // changes it too).
  void poll(std::uint64_t now_ms) noexcept;
  // The settings' way in and out (docs/formats/o3sdk-settings.md). restore() only keeps the values: poll() applies them once the driver runs. Out-of-range values are clamped.
  void restore(const KitValues& v) noexcept;
  void fill(KitValues& v) const noexcept;
  void snapshot_stems() noexcept;

  hal::usb_audio::Device* dev_ = nullptr;
  Files* files_ = nullptr;
  bool audio_ = true;
  Profile profile_ = Profile::P8;
  bool duplex_ = true;
  std::uint8_t level_ = kLevelDefault;
  bool map_set_ = false;  // an edited map is kept (stems_ are its slots)
  std::uint16_t stems_[kMaxChannels] = {};
  bool applied_ = false;  // the kept values went to the driver
  bool stems_pending_ = false;  // the saved slots wait for the device to report the profile they were saved for
  bool read_only_ = true;
  bool share_wait_ = false;  // share_disk() is waiting for the kit's settings to be written
  std::uint64_t share_until_ = 0;
  std::uint64_t now_ms_ = 0;
  hal::usb_audio::DiskRefusal refusal_ = hal::usb_audio::DiskRefusal::None;
};

}  // namespace o3sdk::app
