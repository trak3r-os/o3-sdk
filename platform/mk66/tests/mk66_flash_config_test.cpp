// SPDX-License-Identifier: GPL-3.0-or-later
//
// The flash configuration field (flash_config.h): 16 bytes at 0x400 that decide whether the MCU is secured. The required
// bytes are written here again as a literal, independently of the header and of tools/check_image.py: three copies that
// must agree (the flashing-safety rules holds the fourth). A wrong value can permanently lock the MCU.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/flash_config.h>

#include <cstdint>
#include <cstring>

namespace mk66 = o3sdk::platform::mk66;

TEST_CASE("flash config: the 16 bytes are ff ff ff ff ff ff ff ff ff ff ff ff de f9 ff ff") {
  const std::uint8_t required[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xF9, 0xFF, 0xFF};
  std::uint8_t actual[16];
  static_assert(sizeof(mk66::kFlashConfig) == 16, "the field is sixteen bytes");
  std::memcpy(actual, &mk66::kFlashConfig, sizeof actual);
  CHECK(std::memcmp(actual, required, sizeof required) == 0);
}

TEST_CASE("flash config: FSEC leaves the MCU unsecured") {
  CHECK(mk66::kFsec == 0xDE);
  CHECK((mk66::kFsec & 0x03u) == 0x02u);  // SEC = 10b: the only unsecure encoding
  CHECK(mk66::kFsec != 0xDC);              // a "locked" variant
  CHECK(mk66::kFopt == 0xF9);
  CHECK(mk66::kFlashConfig.fsec == 0xDE);
  CHECK(mk66::kFlashConfig.fopt == 0xF9);
  CHECK(mk66::kFlashConfig.feprot == 0xFF);
  CHECK(mk66::kFlashConfig.fdprot == 0xFF);
  for (const std::uint8_t b : mk66::kFlashConfig.backdoor_key) {
    CHECK(b == 0xFF);
  }
  for (const std::uint8_t b : mk66::kFlashConfig.fprot) {
    CHECK(b == 0xFF);   // nothing write-protected
  }
}

TEST_CASE("flash config: the validator rejects every single-byte difference") {
  CHECK(mk66::flash_config_is_required(mk66::kFlashConfig));
  std::uint8_t bytes[16];
  std::memcpy(bytes, &mk66::kFlashConfig, sizeof bytes);
  unsigned accepted = 0;
  for (unsigned position = 0; position < 16; ++position) {
    for (unsigned value = 0; value < 256; ++value) {
      if (value == bytes[position]) {
        continue;
      }
      std::uint8_t changed[16];
      std::memcpy(changed, bytes, sizeof changed);
      changed[position] = static_cast<std::uint8_t>(value);
      mk66::FlashConfig c;
      std::memcpy(&c, changed, sizeof c);
      if (mk66::flash_config_is_required(c)) {
        ++accepted;
      }
    }
  }
  CHECK(accepted == 0u);
}
