// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace o3sdk::usb {

// USB identity. The maintainer's decision (ADR 0012 amendment 3): the tracker keeps ONE identity in both modes, and the device name is "Track3r".
// The VID:PID 16D0:0E9F are the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included;
// the maintainer's statement is the evidence (docs/hw/tracker-hardware-notes.md). The risks are accepted: the VID belongs to another vendor, and a host tool
// that matches on VID/PID may take the unit for the official Tracker. The OS is told apart by bcdDevice (the values below) and by the strings (manufacturer
// "track3r-os", product "Track3r"), never by VID/PID (docs/specs/OS-USBDISK.md 3.1).
// The modes are told apart by bcdDevice, not by PID: a host that keys its driver and descriptor cache on VID+PID+REV
// (Windows does) then re-reads the descriptors when the mode changes, and no second PID has to be obtained. The bcdDevice
// values must differ (static_assert below; the validator and a test check the bytes). Still provisional: the serial
// number (kSerialPlaceholder below, ADR 0011 item 6), bMaxPower and the release number inside bcdDevice.
inline constexpr std::uint16_t kVendorId = 0x16D0;
inline constexpr std::uint16_t kProductId = 0x0E9F;
inline constexpr std::uint16_t kDeviceBcdStandard = 0x0001;  // bcdDevice: release 0.01, STANDARD (MIDI + CDC diag)
inline constexpr std::uint16_t kDeviceBcdAudio = 0x0101;     // the same release, AUDIO mode: a different REV for the host's cache
inline constexpr std::uint16_t kDeviceBcdDisk = 0x0002;      // DISK mode (OS-USBDISK, maintainer 2026-10-16): the STANDARD set plus a Mass Storage interface
static_assert(kDeviceBcdStandard != kDeviceBcdAudio && kDeviceBcdStandard != kDeviceBcdDisk && kDeviceBcdAudio != kDeviceBcdDisk,
              "the modes must be told apart by bcdDevice (ADR 0012 amendment 3)");

// bMaxPower in 2 mA units (USB 2.0 9.6.3). 100 mA is the bus-powered hub limit per port: the safest
// placeholder until the real draw is measured (hw-session).
inline constexpr std::uint8_t kMaxPowerUnits = 50;

inline constexpr char kManufacturerString[] = "track3r-os";
inline constexpr char kProductString[] = "Track3r";  // iProduct, both modes (maintainer, 2026-10-11)
// Interface strings carry the product name as their prefix.
inline constexpr char kCdcString[] = "Track3r diag";
inline constexpr char kMidiString[] = "Track3r MIDI";
inline constexpr char kAudioString[] = "Track3r Audio";
inline constexpr char kDiskString[] = "Track3r Disk";

// ADR 0011 item 6: the real serial is derived from the chip's unique ID by the platform (006b) and passed
// in Config::serial. ASCII, 1..kMaxSerialChars characters. Until then every unit reports this placeholder.
inline constexpr char kSerialPlaceholder[] = "000000000000";
inline constexpr std::uint32_t kMaxSerialChars = 24;

}  // namespace o3sdk::usb
