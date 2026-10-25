// SPDX-License-Identifier: GPL-3.0-or-later
//
// The USB bring-up as far as the host can prove it (docs/M2B-PLAN.md section 6 (f), slice D). usb0.cpp is registers and cannot be compiled here, so this test makes the calls its
// `Descriptors` struct and start() make, with the platform's facts (the serial from the unique ID, the configuration buffer) and the kit's builders, and holds the result against
// track3r-os's reviewed golden bytes (tests/usb/usb_golden.h, the same file as track3r-os's tests/usb_golden.h):
//   * the layout every boot presents - AUDIO, profile 8, USB RETURN on ("8 OUT / 2 IN", track3r-os's default: its Config USB AUDIO row is On, usb_boot_layout()) - is byte for
//     byte the golden kAudioFSP8, so the proven layout and the host's cached descriptors are reused;
//   * the fallback set, STANDARD, is byte for byte kStandardFS whatever profile and return are remembered (the console's way back asks for (STANDARD, P2, no return));
//   * both pass the kit's structural validator, fit the platform's buffer and satisfy the preconditions start() checks before the pull-up;
//   * a host that enumerates the boot layout through the control pipe (after the STANDARD constructor-time set, as start() rebinds it) reads those bytes, the identity 16D0:0E9F with
//     bcdDevice 0x0101, "track3r-os" / "Track3r" and the serial of the unique ID.
// The refusals of start() and request_layout() (P4S, the DISK set) are usb0.cpp's own lines and are checked on the cross build and the unit.
#include <doctest/doctest.h>

#include "usb_golden.h"
#include "usb_rig.h"
#include "usb_test_util.h"

#include <o3sdk/platform/mk66/usb/boot_layout.h>
#include <o3sdk/platform/mk66/usb/soc_facts.h>
#include <o3sdk/platform/mk66/usb/usb_audio.h>
#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/identity.h>
#include <o3sdk/usb/validate.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace soc = o3sdk::platform::mk66::usb0::soc;
namespace boot_layout = o3sdk::platform::mk66::usb0::boot_layout;
using namespace o3sdk::usb;

namespace {

// What usb0.cpp's `Descriptors` does, call for call: the serial from the unique ID, the speed, then build(mode, profile, usb_return) into word-aligned buffers of the platform's size.
struct Boot {
  Boot() {
    const std::uint32_t uid[4] = {0x0123ABCDu, 0x45670011u, 0x89AB2233u, 0xCDEF4455u};
    soc::serial_from_uid(uid, serial);
    cfg.speed = Speed::Full;
    cfg.serial = serial;
  }
  bool build(Mode mode, Profile profile, bool usb_return) {
    cfg.mode = mode;
    cfg.profile = profile;
    cfg.usb_return = usb_return;
    device_len = build_device_descriptor(cfg, device, sizeof device);
    config_len = build_config_descriptor(cfg, config, sizeof config);
    return device_len == kDeviceDescriptorSize && config_len >= 9u;
  }
  Config cfg;
  char serial[soc::kSerialChars + 1] = {};
  alignas(4) std::uint8_t device[kDeviceDescriptorSize] = {};
  alignas(4) std::uint8_t config[soc::kConfigBufferBytes] = {};
  std::size_t device_len = 0;
  std::size_t config_len = 0;
};

// The checks start() makes before it touches the chip (usb0.cpp step 0).
bool start_preconditions(const Boot& b) {
  return b.device_len == kDeviceDescriptorSize && b.config_len >= 9u && (b.config[2] | (b.config[3] << 8)) == static_cast<int>(b.config_len);
}

}  // namespace

TEST_CASE("usb bring-up: the platform's configuration buffer holds the largest set, and the table is 512-byte aligned by its type") {
  static_assert(soc::kConfigBufferBytes >= kMaxConfigDescriptorSize, "the AUDIO configuration descriptor fits the buffer (usb0.cpp static_assert)");
  static_assert(alignof(BdtTable) == 512, "the buffer descriptor table is 512-byte aligned by type (usb0.cpp static_assert)");
  static_assert(kBdtAlign == 512, "BDT_PAGE1..3 hold address bits 31..9");
  CHECK(soc::kSerialChars == 16u);
}

TEST_CASE("usb bring-up: the boot layout (AUDIO, P8, USB RETURN on) is byte for byte track3r-os's golden kAudioFSP8, and passes the validator") {
  // The three values main.cpp starts the driver with (usb/boot_layout.h): pinned to the proven layout here.
  static_assert(boot_layout::kMode == Mode::Audio && boot_layout::kProfile == Profile::P8 && boot_layout::kReturn, "AUDIO, profile 8, the return on");
  Boot b;
  USB_REQUIRE(b.build(boot_layout::kMode, boot_layout::kProfile, boot_layout::kReturn));
  CHECK(start_preconditions(b));
  CHECK(b.config_len == sizeof(usb_golden::kAudioFSP8));
  CHECK(b.config_len == 384u);
  CHECK((b.config_len == sizeof(usb_golden::kAudioFSP8) && std::memcmp(b.config, usb_golden::kAudioFSP8, b.config_len) == 0));
  CHECK(validate_config_descriptor(b.config, b.config_len, b.cfg).ok());
  CHECK(validate_device_descriptor(b.device, b.device_len, b.cfg).ok());
  // The identity: 16D0:0E9F, bcdDevice 0x0101 (AUDIO), strings 1 2 3 and one configuration.
  static const std::uint8_t audio_device[18] = {0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xD0, 0x16, 0x9F, 0x0E, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01};
  CHECK(std::memcmp(b.device, audio_device, sizeof audio_device) == 0);
  // 8 channels out of the unit (16 bit, 44.1 kHz) and the stereo return with its feedback endpoint: "8 OUT / 2 IN".
  CHECK(profile_channels(Profile::P8) == 8u);
  CHECK(max_packet_bytes(profile_channels(Profile::P8), Speed::Full) == 720u);
  const std::uint8_t* ep_in = nullptr;
  const std::uint8_t* ep_fb = nullptr;
  for (std::size_t pos = 0; pos + 7u <= b.config_len; pos += b.config[pos]) {
    if (b.config[pos + 1u] == 5u && b.config[pos + 2u] == layout::kEpAudioIn) {
      ep_in = &b.config[pos];
    }
    if (b.config[pos + 1u] == 5u && b.config[pos + 2u] == layout::kEpAudioFeedback) {
      ep_fb = &b.config[pos];
    }
  }
  USB_REQUIRE(ep_in != nullptr);
  CHECK((ep_in[4] | (ep_in[5] << 8)) == 720);  // 8 channels x 2 bytes x 45 frames
  CHECK(ep_fb != nullptr);                      // the return's feedback endpoint (USB RETURN on)
}

TEST_CASE("usb bring-up: the fallback set STANDARD is byte for byte the golden kStandardFS, whatever profile and return are remembered") {
  struct Remembered {
    Profile profile;
    bool usb_return;
  };
  const Remembered all[] = {{Profile::P8, true}, {Profile::P2, false}, {Profile::P10, true}, {Profile::P11, false}, {Profile::P4S, true}};
  for (const Remembered& r : all) {
    Boot b;
    USB_REQUIRE(b.build(Mode::Standard, r.profile, r.usb_return));
    CHECK(start_preconditions(b));
    CHECK(b.config_len == 175u);
    CHECK((b.config_len == sizeof(usb_golden::kStandardFS) && std::memcmp(b.config, usb_golden::kStandardFS, b.config_len) == 0));
    CHECK(validate_config_descriptor(b.config, b.config_len, b.cfg).ok());
    CHECK(validate_device_descriptor(b.device, b.device_len, b.cfg).ok());
    // bcdDevice 0x0001: the descriptors a host cached for track3r-os's STANDARD set (same VID, PID and REV).
    static const std::uint8_t standard_device[18] = {0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xD0, 0x16, 0x9F, 0x0E, 0x01, 0x00, 0x01, 0x02, 0x03, 0x01};
    CHECK(std::memcmp(b.device, standard_device, sizeof standard_device) == 0);
  }
}

TEST_CASE("usb bring-up: the tap can feed the set the image boots in, and the boot is not a profile it refuses") {
  namespace ua = o3sdk::platform::mk66::usb_audio;
  CHECK(ua::supported(Profile::P8));
  CHECK(ua::supported(Profile::P2));  // the console's way back asks for (STANDARD, P2, false)
  CHECK_FALSE(ua::supported(Profile::P4S));
  // The stream's channel count, which the hook gives the tap, is the profile's: the master lands on its last two.
  CHECK(ua::UsbAudioTap::kDefaultChannels == profile_channels(Profile::P8));
}

TEST_CASE("usb bring-up: a host enumerating the boot layout through the control pipe - after the constructor-time STANDARD set, as start() rebinds it - reads the golden bytes, the identity and the serial") {
  // usb0.cpp's static constructor builds STANDARD; start() builds (AUDIO, P8, true) into the same buffers and ControlPipe::rebind()s over it before the pull-up.
  Boot audio;
  USB_REQUIRE(audio.build(boot_layout::kMode, boot_layout::kProfile, boot_layout::kReturn));
  Boot standard;
  USB_REQUIRE(standard.build(Mode::Standard, Profile::P8, true));
  usb_rig::Rig& rig = usb_rig::fresh(standard.cfg);
  USB_REQUIRE(rig.enumerate());
  std::uint8_t buf[512];
  usb_fake::Host::Result r = rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf);
  USB_REQUIRE(r.ok);
  CHECK((buf[12] | (buf[13] << 8)) == 0x0001);
  // The boot's rebind: the pipe over the boot set's buffers, the controller prepared again, the host enumerates (it has seen nothing before the pull-up: this is its first look).
  rig.cfg_len = audio.config_len;
  rig.pipe.rebind(audio.cfg, audio.device, audio.device_len, audio.config, audio.config_len);
  rig.dev.prepare();
  USB_REQUIRE(rig.enumerate());
  CHECK(rig.pipe.config().mode == Mode::Audio);
  r = rig.get_descriptor(2, 0, 0, 512, buf, sizeof buf);
  USB_REQUIRE(r.ok);
  CHECK(r.length == 384u);
  CHECK(std::memcmp(buf, usb_golden::kAudioFSP8, 384u) == 0);
  r = rig.get_descriptor(1, 0, 0, 18, buf, sizeof buf);
  USB_REQUIRE(r.ok);
  CHECK(r.length == 18u);
  CHECK((buf[8] | (buf[9] << 8)) == 0x16D0);
  CHECK((buf[10] | (buf[11] << 8)) == 0x0E9F);
  CHECK((buf[12] | (buf[13] << 8)) == 0x0101);
  // The strings, UTF-16LE: manufacturer "track3r-os", product "Track3r", serial = the unique ID's sixteen digits.
  const auto text = [&](std::uint8_t index) {
    std::string out;
    const usb_fake::Host::Result sr = rig.get_descriptor(3, index, 0x0409, 64, buf, sizeof buf);
    if (!sr.ok || sr.length < 2u || buf[1] != 3u) {
      return std::string("?");
    }
    for (std::uint32_t i = 2; i + 1u < sr.length; i += 2u) {
      out.push_back(static_cast<char>(buf[i]));
    }
    return out;
  };
  CHECK(text(kStrManufacturer) == "track3r-os");
  CHECK(text(kStrProduct) == "Track3r");
  CHECK(text(kStrSerial) == std::string(audio.serial));
  CHECK(std::strlen(audio.serial) == soc::kSerialChars);
}
