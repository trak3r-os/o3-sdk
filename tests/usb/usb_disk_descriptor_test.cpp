// SPDX-License-Identifier: GPL-3.0-or-later
// OS-USBDISK U1a: the DISK mode descriptors (STANDARD + one Mass Storage interface, EP 4 OUT / EP 5 IN), their goldens, the validator and the endpoint
// table. docs/specs/OS-USBDISK.md 3.1.
#include <doctest/doctest.h>

#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/endpoint.h>
#include <o3sdk/usb/validate.h>

#include "usb_golden.h"
#include "usb_test_util.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>

using namespace o3sdk::usb;

namespace {

Config disk(Speed s = Speed::Full) {
  Config c;
  c.mode = Mode::Disk;
  c.speed = s;
  return c;
}

struct Built {
  std::uint8_t bytes[kMaxConfigDescriptorSize];
  std::size_t size;
};
Built build(const Config& cfg) {
  Built b{};
  b.size = build_config_descriptor(cfg, b.bytes, sizeof b.bytes);
  return b;
}
std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

TEST_CASE("usb disk: the configuration descriptor equals the golden bytes, FS and HS") {
  const Built fs = build(disk(Speed::Full));
  const Built hs = build(disk(Speed::High));
  CHECK(fs.size == sizeof usb_golden::kDiskFS);
  CHECK(hs.size == sizeof usb_golden::kDiskHS);
  CHECK((fs.size == sizeof usb_golden::kDiskFS && std::memcmp(fs.bytes, usb_golden::kDiskFS, fs.size) == 0));
  CHECK((hs.size == sizeof usb_golden::kDiskHS && std::memcmp(hs.bytes, usb_golden::kDiskHS, hs.size) == 0));
  CHECK(validate_config_descriptor(fs.bytes, fs.size, disk(Speed::Full)).ok());
  CHECK(validate_config_descriptor(hs.bytes, hs.size, disk(Speed::High)).ok());
}

TEST_CASE("usb disk: the golden is the STANDARD golden plus a hand-written Mass Storage interface") {
  // An oracle that is not the builder: the reviewed STANDARD bytes, bNumInterfaces and wTotalLength patched, and the 23 bytes of the Bulk-Only
  // interface written out here from the Mass Storage Class (08h SCSI transparent 06h, Bulk-Only 50h) and the endpoint numbers of the spec.
  for (const bool fs : {true, false}) {
    std::uint8_t expect[198];
    const std::uint8_t* base = fs ? usb_golden::kStandardFS : usb_golden::kStandardHS;
    std::memcpy(expect, base, 175);
    expect[4] = 5;
    expect[2] = 198;
    expect[3] = 0;
    const std::uint8_t mps_lo = fs ? 0x40 : 0x00, mps_hi = fs ? 0x00 : 0x02;
    const std::uint8_t suffix[23] = {9, 4, 4, 0, 2, 0x08, 0x06, 0x50, 6,  //
                                     7, 5, 0x04, 0x02, mps_lo, mps_hi, 0,   //
                                     7, 5, 0x85, 0x02, mps_lo, mps_hi, 0};
    // (9 + 7 + 7 = 23 bytes)
    std::memcpy(expect + 175, suffix, 23);
    const Built b = build(disk(fs ? Speed::Full : Speed::High));
    CHECK(b.size == 198);
    CHECK((b.size == 198 && std::memcmp(b.bytes, expect, 198) == 0));
    const std::uint8_t* golden = fs ? usb_golden::kDiskFS : usb_golden::kDiskHS;
    CHECK(std::memcmp(golden, expect, 198) == 0);
  }
}

TEST_CASE("usb disk: STANDARD and AUDIO descriptors are unchanged by the new mode") {
  Config st;
  const Built b = build(st);
  CHECK(b.size == sizeof usb_golden::kStandardFS);
  CHECK(std::memcmp(b.bytes, usb_golden::kStandardFS, b.size) == 0);
  CHECK(layout::interface_count(Mode::Standard) == 4);
  CHECK(layout::interface_count(Mode::Audio, true) == 7);
  CHECK(layout::interface_count(Mode::Audio, false) == 6);
  CHECK(layout::interface_count(Mode::Disk) == 5);
  CHECK(layout::if_midi_control(Mode::Disk) == layout::if_midi_control(Mode::Standard));
  CHECK(static_cast<int>(Mode::Audio) == 1);  // the stored USB mode setting (model::kUsbModeMax) is Standard or Audio
}

TEST_CASE("usb disk: identity, bcdDevice 0x0002, strings") {
  std::uint8_t d[kDeviceDescriptorSize];
  CHECK(build_device_descriptor(disk(), d, sizeof d) == 18);
  static const std::uint8_t expect[18] = {0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40, 0xD0,
                                          0x16, 0x9F, 0x0E, 0x02, 0x00, 0x01, 0x02, 0x03, 0x01};
  CHECK(std::memcmp(d, expect, 18) == 0);
  CHECK(validate_device_descriptor(d, 18, disk()).ok());
  CHECK_FALSE(validate_device_descriptor(d, 18, Config{}).ok());  // not the STANDARD bcdDevice
  // The OS is told apart by bcdDevice and the strings, never by VID/PID (usb/include/o3sdk/usb/identity.h).
  CHECK(kVendorId == 0x16D0);
  CHECK(kProductId == 0x0E9F);
  CHECK(rd16(d + 12) == 0x0002);
  CHECK(rd16(d + 12) != kDeviceBcdStandard);
  CHECK(rd16(d + 12) != kDeviceBcdAudio);
  CHECK(device_bcd(Mode::Disk) == 0x0002);

  CHECK(string_count(disk()) == 7);
  std::uint8_t s[kMaxStringDescriptorSize];
  const auto text = [&](std::uint8_t index, char* out) {
    const std::size_t n = build_string_descriptor(disk(), index, s, sizeof s);
    std::size_t len = 0;
    for (std::size_t i = 2; i + 1 < n; i += 2) {
      out[len++] = static_cast<char>(s[i]);
    }
    out[len] = '\0';
    return n;
  };
  char t[40];
  CHECK(text(kStrManufacturer, t) != 0);
  CHECK(std::strcmp(t, "track3r-os") == 0);
  CHECK(text(kStrProduct, t) != 0);
  CHECK(std::strcmp(t, "Track3r") == 0);
  CHECK(text(kStrDisk, t) != 0);
  CHECK(std::strcmp(t, "Track3r Disk") == 0);
  CHECK(build_string_descriptor(disk(), 7, s, sizeof s) == 0);
  // The AUDIO mode still names string 6 "Track3r Audio".
  Config au;
  au.mode = Mode::Audio;
  const std::size_t n = build_string_descriptor(au, kStrAudio, s, sizeof s);
  CHECK(n == 2u + 2u * 13u);
}

TEST_CASE("usb disk: the endpoint table: EP 4 OUT and EP 5 IN are bulk, the pair is on two numbers") {
  CHECK(endpoint_kind(Mode::Disk, layout::kEpMscOut, true) == EndpointKind::Bulk);
  CHECK(endpoint_kind(Mode::Disk, layout::kEpMscIn, true) == EndpointKind::Bulk);
  CHECK(endpoint_kind(Mode::Disk, layout::kEpMscOut, false) == EndpointKind::Bulk);  // usb_return is an AUDIO-only flag
  CHECK(endpoint_kind(Mode::Disk, layout::kEpAudioIn, true) == EndpointKind::None);  // 0x84: no isochronous endpoint in DISK mode
  CHECK(endpoint_kind(Mode::Standard, layout::kEpMscOut, true) == EndpointKind::None);
  CHECK(endpoint_kind(Mode::Standard, layout::kEpMscIn, true) == EndpointKind::None);
  CHECK(endpoint_kind(Mode::Audio, layout::kEpMscOut, true) == EndpointKind::Isochronous);  // the AUDIO return stream, unchanged
  CHECK(endpoint_kind(Mode::Audio, layout::kEpMscIn, true) == EndpointKind::Isochronous);
  CHECK(endpoint_kind(Mode::Audio, layout::kEpMscIn, false) == EndpointKind::None);
  CHECK(last_endpoint(Mode::Disk, true) == 5);
  CHECK(last_endpoint(Mode::Disk, false) == 5);
  CHECK(last_endpoint(Mode::Standard, true) == 3);
  CHECK(last_endpoint(Mode::Audio, true) == 5);
  CHECK((layout::kEpMscOut & 0x0F) != (layout::kEpMscIn & 0x0F));
  // ENDPTn: no SETUP tokens, handshake and the direction's enable bit: one direction per number.
  CHECK(endpoint_config(Mode::Disk, 4, true) == (kEndptCtlDisable | kEndptRxEn | kEndptHandshake));
  CHECK(endpoint_config(Mode::Disk, 5, true) == (kEndptCtlDisable | kEndptTxEn | kEndptHandshake));
  CHECK(endpoint_config(Mode::Disk, 1, true) == endpoint_config(Mode::Standard, 1, true));
  CHECK(endpoint_config(Mode::Disk, 2, true) == endpoint_config(Mode::Standard, 2, true));
  CHECK(endpoint_config(Mode::Disk, 3, true) == endpoint_config(Mode::Standard, 3, true));
}

TEST_CASE("usb disk: the validator catches a broken Mass Storage interface") {
  const Config cfg = disk();
  const Built good = build(cfg);
  USB_REQUIRE(validate_config_descriptor(good.bytes, good.size, cfg).ok());
  const std::size_t msc = 175;  // the interface descriptor
  const std::size_t ep_out = msc + 9, ep_in = msc + 16;

  const auto expect = [&](const char* what, Built b, std::size_t cut, ValidateError e) {
    INFO(what);
    b.size -= cut;
    b.bytes[2] = static_cast<std::uint8_t>(b.size);
    b.bytes[3] = static_cast<std::uint8_t>(b.size >> 8);
    CHECK(static_cast<int>(validate_config_descriptor(b.bytes, b.size, cfg).error) == static_cast<int>(e));
  };
  {  // the interface cut out: DISK needs it
    Built b = good;
    b.bytes[4] = 4;
    expect("no MSC interface", b, 23, ValidateError::ModeCounts);
  }
  {  // wrong bulk OUT address
    Built b = good;
    b.bytes[ep_out + 2] = 0x06;
    CHECK(validate_config_descriptor(b.bytes, b.size, cfg).error == ValidateError::EndpointNumber);
  }
  {  // two OUT endpoints
    Built b = good;
    b.bytes[ep_in + 2] = 0x04;
    CHECK_FALSE(validate_config_descriptor(b.bytes, b.size, cfg).ok());
  }
  {  // not bulk
    Built b = good;
    b.bytes[ep_in + 3] = 0x03;
    CHECK(validate_config_descriptor(b.bytes, b.size, cfg).error == ValidateError::BadMaxPacket);
  }
  {  // wrong packet size for the speed
    Built b = good;
    b.bytes[ep_in + 4] = 0x00;
    b.bytes[ep_in + 5] = 0x02;
    CHECK(validate_config_descriptor(b.bytes, b.size, cfg).error == ValidateError::BadMaxPacket);
  }
  {  // the protocol is not Bulk-Only: the interface is no longer the Mass Storage one, its bulk endpoints are not valid class-specific
    Built b = good;
    b.bytes[msc + 7] = 0x62;
    CHECK_FALSE(validate_config_descriptor(b.bytes, b.size, cfg).ok());
  }
  {  // a string index past the end
    Built b = good;
    b.bytes[msc + 8] = 7;
    CHECK(validate_config_descriptor(b.bytes, b.size, cfg).error == ValidateError::BadString);
  }
  // The DISK bytes are not valid as STANDARD, and the STANDARD bytes not as DISK.
  CHECK_FALSE(validate_config_descriptor(good.bytes, good.size, Config{}).ok());
  Config st;
  const Built sb = build(st);
  CHECK_FALSE(validate_config_descriptor(sb.bytes, sb.size, cfg).ok());
}

TEST_CASE("usb disk: a Mass Storage interface outside DISK mode is refused") {
  // The lone interface is allowed outside an IAD only in DISK mode: the golden DISK bytes validated as AUDIO or STANDARD fail.
  Config au;
  au.mode = Mode::Audio;
  CHECK_FALSE(validate_config_descriptor(usb_golden::kDiskFS, sizeof usb_golden::kDiskFS, au).ok());
  CHECK_FALSE(validate_config_descriptor(usb_golden::kDiskFS, sizeof usb_golden::kDiskFS, Config{}).ok());
}
