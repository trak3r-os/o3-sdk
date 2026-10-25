// SPDX-License-Identifier: GPL-3.0-or-later
// OS-006b: the USB0 platform facts (usb/soc_facts.h): the clock choice and its proof, the register values the driver writes, the
// unique-ID serial number, the interrupt row. The values are pinned here as literals so that a change shows in review.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/usb/soc_facts.h>
#include <o3sdk/usb/descriptors.h>
#include <o3sdk/usb/endpoint.h>
#include <o3sdk/usb/validate.h>

#include <cstdint>
#include <cstring>

using namespace o3sdk::platform::mk66;

TEST_CASE("mk66 usb: the 48 MHz clock cannot come from the 180 MHz PLL, so it is the IRC48M") {
  CHECK(clock::mcgout_hz() == 180'000'000u);
  CHECK_FALSE(usb0::soc::divider_makes_usb_clock(180'000'000u));
  // What would have worked, and what does: the divider is out = in x (frac + 1) / (div + 1).
  CHECK(usb0::soc::divider_makes_usb_clock(192'000'000u));  // 192 / 4
  CHECK(usb0::soc::divider_makes_usb_clock(144'000'000u));  // 144 / 3
  CHECK(usb0::soc::divider_makes_usb_clock(96'000'000u));   // 96 / 2
  CHECK(usb0::soc::divider_makes_usb_clock(48'000'000u));   // the IRC48M itself
  CHECK(usb0::soc::divider_makes_usb_clock(120'000'000u));  // 120 x 2 / 5
  CHECK(usb0::soc::divider_makes_usb_clock(168'000'000u));  // 168 x 2 / 7
  CHECK_FALSE(usb0::soc::divider_makes_usb_clock(216'000'000u));
  CHECK_FALSE(usb0::soc::divider_makes_usb_clock(49'000'000u));  // 2 % off is far outside 0.25 %
  CHECK(usb0::soc::kUsbClockToleranceHz == 120'000u);
  // The IRC48M register values the driver writes.
  CHECK(usb0::soc::kClkRecoverIrcEnable == 0x03);
  CHECK(usb0::soc::kClkRecoverCtrlEnable == 0xA0);
  CHECK(usb0::soc::kSopt2UsbBits == 0x00070000u);   // USBSRC (bit 18) | PLLFLLSEL = 3 (bits 17..16)
  CHECK(usb0::soc::kSopt2PllFllSelIrc48 == 0x00030000u);
  CHECK(usb0::soc::kClkdiv2 == 0u);
  CHECK(usb0::soc::kScgc4UsbOtg == 0x00040000u);
}

TEST_CASE("mk66 usb: register addresses") {
  CHECK(usb0::soc::kBase == 0x40072000u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffIstat == 0x40072080u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffInten == 0x40072084u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffErrstat == 0x40072088u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffErren == 0x4007208Cu);
  CHECK(usb0::soc::kBase + usb0::soc::kOffStat == 0x40072090u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffCtl == 0x40072094u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffAddr == 0x40072098u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffBdtPage1 == 0x4007209Cu);
  CHECK(usb0::soc::kBase + usb0::soc::kOffBdtPage2 == 0x400720B0u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffBdtPage3 == 0x400720B4u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffFrmNumL == 0x400720A0u);  // OS-006d: the SOF frame number (kinetis.h USB0_FRMNUML / USB0_FRMNUMH)
  CHECK(usb0::soc::kBase + usb0::soc::kOffFrmNumH == 0x400720A4u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffEndpt0 == 0x400720C0u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffEndpt0 + 5u * usb0::soc::kEndptStride == 0x400720D4u);  // ENDPT5
  CHECK(usb0::soc::kBase + usb0::soc::kOffUsbCtrl == 0x40072100u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffControl == 0x40072108u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffClkRecoverCtrl == 0x40072140u);
  CHECK(usb0::soc::kBase + usb0::soc::kOffClkRecoverIrcEn == 0x40072144u);
  CHECK(usb0::soc::kControlPullUp == 0x10);
  CHECK(usb0::soc::kUsbCtrlRun == 0x00);
  // Outside USB0.
  CHECK(usb0::soc::kSimSopt2 == 0x40048004u);
  CHECK(usb0::soc::kSimScgc4 == 0x40048034u);
  CHECK(usb0::soc::kSimClkdiv2 == 0x40048048u);
  CHECK(usb0::soc::kMpuRgdaac0 == 0x4000D800u);
  CHECK(usb0::soc::kMpuUsbMaster == 0x03000000u);
  CHECK(usb0::soc::kSimUidh == 0x40048054u);
  CHECK(usb0::soc::kSimUidl == 0x40048060u);
  CHECK(usb0::soc::kSimUidl - usb0::soc::kSimUidh == 12u);
}

TEST_CASE("mk66 usb: the BDT page registers hold the table address") {
  const std::uint32_t table = 0x1FFF8200u;  // any 512-byte aligned SRAM address
  CHECK(usb0::soc::bdt_page1(table) == 0x82);
  CHECK(usb0::soc::bdt_page2(table) == 0xFF);
  CHECK(usb0::soc::bdt_page3(table) == 0x1F);
  const std::uint32_t rebuilt = (static_cast<std::uint32_t>(usb0::soc::bdt_page3(table)) << 24) |
                                (static_cast<std::uint32_t>(usb0::soc::bdt_page2(table)) << 16) |
                                (static_cast<std::uint32_t>(usb0::soc::bdt_page1(table)) << 8);
  CHECK(rebuilt == table);
  // The table's alignment is the page granularity: bit 9 is the lowest address bit the registers hold.
  CHECK(o3sdk::usb::kBdtAlign == 512u);
  CHECK((usb0::soc::bdt_page1(0x20000200u) & 0x01u) == 0u);  // PAGE1 bit 0 is reserved
}

TEST_CASE("mk66 usb: the serial number is a stable function of the unique ID") {
  const std::uint32_t uid_a[4] = {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u};
  char a[usb0::soc::kSerialChars + 1];
  usb0::soc::serial_from_uid(uid_a, a);
  CHECK(std::strlen(a) == usb0::soc::kSerialChars);
  for (std::size_t i = 0; i < usb0::soc::kSerialChars; ++i) {
    CHECK(((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'F')));
  }
  char again[usb0::soc::kSerialChars + 1];
  usb0::soc::serial_from_uid(uid_a, again);
  CHECK(std::strcmp(a, again) == 0);  // deterministic
  // Every word and every bit of the ID matters (all 128 bits are in play, none cut by the descriptor's 24-character limit).
  for (unsigned word = 0; word < 4u; ++word) {
    for (unsigned bit = 0; bit < 32u; bit += 7u) {
      std::uint32_t uid_b[4] = {uid_a[0], uid_a[1], uid_a[2], uid_a[3]};
      uid_b[word] ^= 1u << bit;
      char b[usb0::soc::kSerialChars + 1];
      usb0::soc::serial_from_uid(uid_b, b);
      CHECK(std::strcmp(a, b) != 0);
    }
  }
  // An erased or zero ID is still a valid serial (and not the placeholder the descriptors fall back to).
  const std::uint32_t ones[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const std::uint32_t zeros[4] = {0, 0, 0, 0};
  char o[usb0::soc::kSerialChars + 1];
  char z[usb0::soc::kSerialChars + 1];
  usb0::soc::serial_from_uid(ones, o);
  usb0::soc::serial_from_uid(zeros, z);
  CHECK(std::strcmp(o, z) != 0);
  CHECK(std::strcmp(o, o3sdk::usb::kSerialPlaceholder) != 0);
  // Known values, FNV-1a 64 over the sixteen bytes (computed independently): pin the hash and its byte order.
  CHECK(usb0::soc::fnv1a64_words(zeros, 4) == 0x88201FB960FF6465ULL);
  CHECK(std::strcmp(a, "9CC819A7AD93F175") == 0);
  // And the descriptor takes it: kMaxSerialChars is 24, the serial is 16 characters.
  CHECK(usb0::soc::kSerialChars <= o3sdk::usb::kMaxSerialChars);
  o3sdk::usb::Config cfg;
  cfg.serial = a;
  std::uint8_t d[o3sdk::usb::kMaxStringDescriptorSize];
  CHECK(o3sdk::usb::build_string_descriptor(cfg, o3sdk::usb::kStrSerial, d, sizeof d) == 2u + 2u * usb0::soc::kSerialChars);
}

TEST_CASE("mk66 usb: the STANDARD configuration fits the driver's buffer and the descriptors validate") {
  o3sdk::usb::Config cfg;
  cfg.mode = o3sdk::usb::Mode::Standard;
  std::uint8_t config[usb0::soc::kConfigBufferBytes];
  const std::size_t n = o3sdk::usb::build_config_descriptor(cfg, config, sizeof config);
  CHECK(n == 175u);
  CHECK(o3sdk::usb::validate_config_descriptor(config, n, cfg).ok());
  std::uint8_t device[o3sdk::usb::kDeviceDescriptorSize];
  CHECK(o3sdk::usb::build_device_descriptor(cfg, device, sizeof device) == 18u);
  CHECK(o3sdk::usb::validate_device_descriptor(device, 18, cfg).ok());
  // One byte too few and the builder refuses: the driver would not pull the line up.
  std::uint8_t small[174];
  CHECK(o3sdk::usb::build_config_descriptor(cfg, small, sizeof small) == 0u);
}

TEST_CASE("mk66 usb: USB0 is interrupt 53 in the Usb row") {
  CHECK(static_cast<unsigned>(usb0::soc::kIrq) == 53u);
  CHECK(role_of(usb0::soc::kIrq) == o3sdk::hal::irq::Role::Usb);
  CHECK(o3sdk::hal::irq::priority(role_of(usb0::soc::kIrq)) == 64u);
  // Nothing else in the map shares the Usb role (the other USB lines, USBDCD and USBHS, stay unassigned and disabled).
  unsigned usb_lines = 0;
  for (const LineRole& entry : kLineRoles) {
    if (entry.role == o3sdk::hal::irq::Role::Usb) {
      ++usb_lines;
    }
  }
  CHECK(usb_lines == 1u);
}

TEST_CASE("mk66 usb: the interrupt's worst entry is bounded against the audio block") {
  CHECK(usb0::soc::kEstimatedPassCycles == 600u);
  CHECK(usb0::soc::kEstimatedWorstEntryCycles == 16u * 600u);  // 9,600 cycles = 53 us at 180 MHz
  CHECK(usb0::soc::kEstimatedWorstEntryCycles * 20u <= o3sdk::hal::audio::kCyclesPerBlock);
  CHECK(o3sdk::usb::kMaxIsrLoops == 16u);
}
