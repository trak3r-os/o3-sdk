// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB audio service (docs/M2C-PLAN.md section 7, decisions D11, D12, D14): hal::usb_audio::Device over the stem tap (taps.h), the stem map (stem_map.h), the return stream and a Link to
// the USB driver (usb0 on the unit, a fake in the simulator). Pure: the simulator runs this very code, so the stems and the return mix there are the unit's to the sample.
//
//   THE STEMS (render). wrap() returns the block callback that runs the app's, then, ONLY while the computer has the capture stream open, hands the engine's stem table (Engine::stem_block(),
//   the post-fader pre-pan mono blocks of the slots the profile's channels name, the master pair and the sends) to StemTap::push. The platform sets the engine's source mask from the union of
//   the slots' masks when the stream opens and clears it when it closes (streams_changed(), the USB interrupt's hook), so the engine stages nothing while no host listens. The master pair
//   Engine::stem_block() gives is "what planes_to_s16 sends to the codec": the same samples the main output plays, so the boot profile P8 carries on channels 7 and 8 what it always did.
//   THE RETURN (render). While the OUT stream runs, every block pulls its 64 frames out of the return ring (usb::ReturnReader::pull: silence while priming or after an underrun, both counted);
//   they are (1) the engine's input, replacing the codec's block, when the audio in's source is Usb (the app layer says so: route_return_to_input), so a unit, a hook or the recorder hears
//   it, and (2) added to the output AFTER the app's block at return_level(), with saturation: outside the master inserts, and no take, stem or scope sees it. The pull happens whatever the
//   source and the level, so the ring never overruns. A silenced block (kFlagSilence) gets no return.
//   COST. Idle (no stream, no Usb source): five relaxed loads (the tap's enable, the return flag, the Usb-source flag, the engine pointer, the engine's mask), their branches and one indirect call
//   (the app's callback), under 50 cycles [est]; active_blocks() counts the blocks that did more and a test holds it at 0.
//   THE SILENCED BLOCK. The stems come from the engine, which renders a block whatever the pipeline's flags say, while the main output is zeroed under kFlagSilence (an underrun's mute): the master
//   channels of the stream keep carrying the engine's audio then, where the old master-only tap read io.out and carried the zeros. The return is not mixed into a silenced block. Stems on: Engine's staging (feature_work().stem_frames) plus the tap's sum, about 130 cycles a slot [est]; the return on: the pull (about 1,100) and
//   the mix (about 1,000) [est, skill rt-optimize: measured on the unit later].
//
// Contexts (one writer per datum). Render (priority kRender): block(); it owns the tap's pool, the return planes and the gain's ramp. USB interrupt (64): streams_changed() (atomic stores
// and the engine's mask), attach_return() before the interrupt is enabled. Main loop: everything of Device. The USB interrupt preempts the render; every datum it writes is one atomic word.
//
// THE FACTORY MAP of o3-sdk (factory_map): StemMap's own for every profile but P8, whose slots are EMPTY: the boot profile carries the master on its last two channels and digital silence on
// 1..6 (docs/M2B-PLAN.md D5), and an app opts into stems with set_stem() or by choosing P10 or P11. The image's usb0 calls it where track3r-os's resets the map.

#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/usb_audio.h>
#include <o3sdk/platform/mk66/audio/stem_map.h>
#include <o3sdk/platform/mk66/audio/taps.h>
#include <o3sdk/usb/stream.h>
#include <o3sdk/usb/usb_types.h>

#include <atomic>
#include <cstdint>

namespace o3sdk::dsp {
class Engine;
}

namespace o3sdk::platform::mk66::audio {

// hal::usb_audio::Profile <-> usb::Profile (the same four, in the same order; usb::Profile::P4S has no counterpart and is never presented).
constexpr usb::Profile to_usb(hal::usb_audio::Profile p) noexcept {
  switch (p) {
    case hal::usb_audio::Profile::P2: return usb::Profile::P2;
    case hal::usb_audio::Profile::P8: return usb::Profile::P8;
    case hal::usb_audio::Profile::P10: return usb::Profile::P10;
    case hal::usb_audio::Profile::P11: return usb::Profile::P11;
  }
  return usb::Profile::P8;
}
constexpr hal::usb_audio::Profile from_usb(usb::Profile p) noexcept {
  switch (p) {
    case usb::Profile::P2: return hal::usb_audio::Profile::P2;
    case usb::Profile::P8: return hal::usb_audio::Profile::P8;
    case usb::Profile::P10: return hal::usb_audio::Profile::P10;
    case usb::Profile::P11: return hal::usb_audio::Profile::P11;
    case usb::Profile::P4S: break;
  }
  return hal::usb_audio::Profile::P8;
}
static_assert(static_cast<unsigned>(hal::usb_audio::Profile::P11) + 1u == hal::usb_audio::kProfileCount, "four profiles");
static_assert(hal::usb_audio::kMaxChannels == usb::kMaxChannels, "the widest profile");

// o3-sdk's factory slots of `p` (see the header): false (nothing changed) for a profile the map cannot take.
bool factory_map(StemMap& map, usb::Profile p) noexcept;

class UsbAudioService final : public hal::usb_audio::Device {
 public:
  // The USB driver, as the service sees it. The image's is over usb0 and the card's hand-over (usb/src/usb_audio_device.cpp), the simulator's a fake.
  class Link {
   public:
    virtual bool ready() const noexcept = 0;
    virtual bool audio() const noexcept = 0;
    virtual hal::usb_audio::Profile profile() const noexcept = 0;
    virtual bool duplex() const noexcept = 0;
    virtual bool changing() const noexcept = 0;
    virtual bool request(bool audio, hal::usb_audio::Profile profile, bool duplex) noexcept = 0;
    virtual hal::usb_audio::DiskStatus disk() const noexcept = 0;
    virtual hal::usb_audio::DiskRefusal disk_share(bool read_only) noexcept = 0;
    virtual bool disk_eject() noexcept = 0;

   protected:
    Link() = default;
    ~Link() = default;
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
  };

  // `tap` and `map` are the stem tap the USB stream reads and the map that feeds it (the map's owner applies it to the tap on every change of profile: usb0 on the unit); all outlive the service.
  UsbAudioService(StemTap& tap, StemMap& map, Link& link) noexcept : tap_(tap), map_(map), link_(link) {}

  // ---- hal::usb_audio::Device (main loop) -------------------------------------------------------------------------------------------------
  bool ready() const noexcept override { return link_.ready(); }
  bool audio() const noexcept override { return link_.audio(); }
  hal::usb_audio::Profile profile() const noexcept override { return link_.profile(); }
  bool duplex() const noexcept override { return link_.duplex(); }
  bool changing() const noexcept override { return link_.changing(); }
  bool request(bool audio, hal::usb_audio::Profile profile, bool duplex) noexcept override { return link_.request(audio, profile, duplex); }
  bool set_stem(unsigned channel, std::uint32_t sources) noexcept override;
  std::uint32_t stem(unsigned channel) const noexcept override { return map_.slot(channel); }
  void reset_stems() noexcept override;
  void set_return_level(float level) noexcept override;
  float return_level() const noexcept override { return static_cast<float>(gain_q16_.load(std::memory_order_relaxed)) * (1.0f / 65536.0f); }
  void route_return_to_input(bool on) noexcept override { input_usb_.store(on, std::memory_order_relaxed); }
  bool streaming() const noexcept override { return tap_.enabled(); }
  bool return_streaming() const noexcept override { return return_on_.load(std::memory_order_relaxed); }
  hal::usb_audio::DiskStatus disk() const noexcept override { return link_.disk(); }
  hal::usb_audio::DiskRefusal disk_share(bool read_only) noexcept override { return link_.disk_share(read_only); }
  bool disk_eject() noexcept override { return link_.disk_eject(); }

  // ---- The driver's side ----------------------------------------------------------------------------------------------------------------
  // The render's end of the return ring: once, before the USB interrupt is enabled (release / acquire).
  void attach_return(usb::ReturnReader* reader) noexcept { reader_.store(reader, std::memory_order_release); }
  // The streams started or stopped (the USB interrupt, usb::IsoAudio's hook): the tap and the return pull follow the host's alternate settings, and the engine's source mask with them.
  void streams_changed(bool in_active, bool out_active) noexcept;
  // The map changed (an edit, a profile change at an attach): the engine's mask is recomputed from the tap's slots. Main loop or USB interrupt.
  void refresh_stems() noexcept;

  // ---- Render ----------------------------------------------------------------------------------------------------------------------------
  // The callback to give the audio device in place of `inner` (the record service's, or the runtime's). `engine` must outlive the service. Once, before the device starts.
  hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, dsp::Engine& engine) noexcept;
  // Blocks in which the service did more than look at its flags; stem blocks published; return blocks pulled.
  std::uint32_t active_blocks() const noexcept { return active_.load(std::memory_order_relaxed); }
  std::uint32_t pulled_blocks() const noexcept { return pulled_.load(std::memory_order_relaxed); }
  StemTap& tap() noexcept { return tap_; }
  const StemMap& map() const noexcept { return map_; }

 private:
  static void block(void* user, const hal::audio::BlockIo& io) noexcept;
  void process(const hal::audio::BlockIo& io) noexcept;
  void mix_return(std::int16_t* out) noexcept;

  StemTap& tap_;
  StemMap& map_;
  Link& link_;
  std::atomic<dsp::Engine*> engine_{nullptr};
  std::atomic<usb::ReturnReader*> reader_{nullptr};
  std::atomic<bool> return_on_{false};
  std::atomic<bool> input_usb_{false};
  std::atomic<std::int32_t> gain_q16_{0};  // the return's level, Q16 (0 at the start: the return is drained, not heard, until the user turns RETURN LEVEL up)
  hal::audio::BlockCallback inner_{nullptr, nullptr};
  // render only
  std::int32_t gain_now_q16_ = 0;  // where the ramp is (the level reached at the end of the last block)
  alignas(4) std::int16_t ret_[2][hal::audio::kBlock] = {};
  alignas(4) std::int16_t in_[hal::audio::kBlockSamples] = {};  // the engine's input when it is the return
  std::atomic<std::uint32_t> active_{0};
  std::atomic<std::uint32_t> pulled_{0};
};

}  // namespace o3sdk::platform::mk66::audio
