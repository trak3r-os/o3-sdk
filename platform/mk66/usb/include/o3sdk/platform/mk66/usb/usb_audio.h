// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB audio of the image (docs/M2C-PLAN.md section 7; M2b slice D put the master on the capture stream, M2c slice U adds the stems, the return and the disk). The logic is the pure
// audio::UsbAudioService (platform/mk66/audio usb_audio_service.h: hal::usb_audio::Device over the stem tap, the stem map and the return); this header is the image's ONE tap and the way the
// USB driver and main reach the service.
//
//   WHAT THE HOST SEES   the channels of the set presented (usb0::profile(), P8 at boot: eight channels, 16 bit, 44.1 kHz), the master on the LAST TWO, as in every profile of track3r-os (P8: T1..T6
//                        then master left and right; P10: T1..T8 then master; P11: T1..T9 then master; P2: the master alone). In the boot set the master is on channels 7 and 8 and 1 to 6 are digital
//                        silence until an app or the user maps a slot (audio::factory_map), so a DAW's routing is the same for track3r-os and o3-sdk (the maintainer, 2026-10-25). The descriptors are
//                        track3r-os's, byte for byte. P4S (four stereo post-pan stems) is refused, as in track3r-os.
//   THE TAP              audio::StemTap over an audio::StemPool in SRAM_L's free half (`.tapmem_l`, O3SDK_TAP_MEM_L): the render fills it a block at a time from the engine's stem table, the USB
//                        interrupt reads it a packet at a time through usb::StemRing. tap() is that one object; usb0 binds its ring to usb::IsoAudio and its stream hook to the service.
//   THE SERVICE          service(), wrap() and device() are the image's (usb/src/usb_audio_device.cpp, with usb0 and the card's hand-over): the pure service over a Link to usb0. main wraps the render
//                        with wrap(); the app reaches it as Platform::usb_audio = &device().

#include <o3sdk/hal/audio.h>
#include <o3sdk/hal/usb_audio.h>
#include <o3sdk/platform/mk66/audio/taps.h>
#include <o3sdk/platform/mk66/audio/usb_audio_service.h>
#include <o3sdk/usb/usb_types.h>

#include <cstdint>

namespace o3sdk::dsp {
class Engine;
}

namespace o3sdk::platform::mk66::usb_audio {

// The profiles the tap can feed: every profile but P4S, which track3r-os cannot feed either (StemMap::supported). usb0::start() and request_layout() refuse the others.
constexpr bool supported(o3sdk::usb::Profile profile) noexcept { return audio::StemMap::supported(profile); }

// The image's one stem tap (and its pool, SRAM_L on the unit): defined in the pure library, so that the host tests of the driver can link it.
audio::StemTap& tap() noexcept;

// ---- The image's service (platform_mk66_usb_hw: not in a host build) ------------------------------------------------------------------------------------------------------------------
audio::UsbAudioService& service() noexcept;
hal::usb_audio::Device& device() noexcept;
// The render callback of the image with the service in it: calls `inner` (the record service's, around the runtime's), then feeds the stems and mixes the return. Call once, before
// audio_device().start(); the returned callback points at static state. The tone images do not wrap.
hal::audio::BlockCallback wrap(hal::audio::BlockCallback inner, dsp::Engine& engine) noexcept;

// ---- The card as a disk: the hand-over's pass (platform_mk66_usb_hw) ----------------------------------------------------------------------------------------------------------------
// What a pass of the hand-over wants the runtime to show: a strip text (null: none) held `hold_ms`, and whether the card changed hands (Runtime::card_changed()).
struct DiskNote {
  const char* text = nullptr;
  std::uint32_t hold_ms = 0;
  bool card_changed = false;
};
// Once a pass of the main loop, `now` the millisecond clock, `power_latched` the shutdown has the card (the machine is frozen), `render_load_percent` the render's load of the last block (the pump is
// held above 85 %). Nothing happens while no hand-over runs but the pump of a card that is out.
DiskNote disk_pass(std::uint32_t now, bool power_latched, unsigned render_load_percent) noexcept;
// The console's `disk [share [ro] | eject]`: nullptr when the request went through, else the refusal in words; the state in words.
const char* disk_request(bool read_only) noexcept;
const char* disk_eject_request() noexcept;
const char* disk_state_name() noexcept;

}  // namespace o3sdk::platform::mk66::usb_audio
