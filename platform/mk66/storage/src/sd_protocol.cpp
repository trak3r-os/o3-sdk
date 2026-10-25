// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/storage/sd_protocol.h>

namespace o3sdk::platform::mk66::storage::sd {
namespace {

char printable(std::uint32_t c) noexcept { return c >= 0x20u && c < 0x7Fu ? static_cast<char>(c) : '.'; }

}  // namespace

void unpack_r2(const std::uint32_t word[4], std::uint8_t out[16]) noexcept {
  // The register R of 120 bits is spec bits 127:8; out is R shifted up by a byte, the CRC7 + end bit position (the last byte) left zero.
  out[0] = static_cast<std::uint8_t>(word[3] >> 16);
  out[1] = static_cast<std::uint8_t>(word[3] >> 8);
  out[2] = static_cast<std::uint8_t>(word[3]);
  for (unsigned w = 0; w < 3; ++w) {  // words 2, 1, 0: four bytes each
    const std::uint32_t v = word[2 - w];
    out[3 + 4 * w] = static_cast<std::uint8_t>(v >> 24);
    out[4 + 4 * w] = static_cast<std::uint8_t>(v >> 16);
    out[5 + 4 * w] = static_cast<std::uint8_t>(v >> 8);
    out[6 + 4 * w] = static_cast<std::uint8_t>(v);
  }
  out[15] = 0;
}

std::uint32_t reg_bits(const std::uint8_t reg[16], unsigned hi, unsigned lo) noexcept {
  std::uint32_t v = 0;
  for (unsigned n = hi;; --n) {
    v = (v << 1) | ((static_cast<std::uint32_t>(reg[(127u - n) / 8u]) >> (n % 8u)) & 1u);
    if (n == lo) {
      break;
    }
  }
  return v;
}

Cid parse_cid(const std::uint8_t reg[16]) noexcept {
  Cid cid{};
  cid.manufacturer = static_cast<std::uint8_t>(reg_bits(reg, 127, 120));
  cid.oem[0] = printable(reg_bits(reg, 119, 112));
  cid.oem[1] = printable(reg_bits(reg, 111, 104));
  cid.oem[2] = '\0';
  for (unsigned i = 0; i < 5; ++i) {
    cid.product[i] = printable(reg_bits(reg, 103u - 8u * i, 96u - 8u * i));
  }
  cid.product[5] = '\0';
  const std::uint32_t prv = reg_bits(reg, 63, 56);
  cid.revision_major = static_cast<std::uint8_t>(prv >> 4);
  cid.revision_minor = static_cast<std::uint8_t>(prv & 0xFu);
  cid.serial = reg_bits(reg, 55, 24);
  cid.year = static_cast<std::uint16_t>(2000u + reg_bits(reg, 19, 12));
  cid.month = static_cast<std::uint8_t>(reg_bits(reg, 11, 8));
  return cid;
}

Csd parse_csd(const std::uint8_t reg[16]) noexcept {
  Csd csd{};
  csd.ccc = static_cast<std::uint16_t>(reg_bits(reg, 95, 84));
  csd.write_protected = reg_bits(reg, 13, 12) != 0u;  // PERM_WRITE_PROTECT, TMP_WRITE_PROTECT
  const std::uint32_t structure = reg_bits(reg, 127, 126);
  if (structure == 0) {
    const std::uint32_t read_bl_len = reg_bits(reg, 83, 80);
    const std::uint32_t c_size = reg_bits(reg, 73, 62);
    const std::uint32_t c_size_mult = reg_bits(reg, 49, 47);
    csd.version = 1;
    csd.read_bl_len_log2 = static_cast<std::uint8_t>(read_bl_len);
    if (read_bl_len < 9u || read_bl_len > 11u) {
      return csd;  // a block under 512 bytes (or a value the spec does not define): not readable by this driver
    }
    // blocks of 2^READ_BL_LEN bytes, (C_SIZE + 1) << (C_SIZE_MULT + 2) of them; in 512-byte blocks that is << (READ_BL_LEN - 9). At most
    // 4096 << 9 << 2 = 2^23 here, so it fits.
    csd.blocks = (c_size + 1u) << (c_size_mult + 2u + read_bl_len - 9u);
    csd.valid = csd.blocks != 0u;
  } else if (structure == 1) {
    const std::uint32_t c_size = reg_bits(reg, 69, 48);  // 22 bits
    csd.version = 2;
    csd.read_bl_len_log2 = 9;
    csd.blocks = (c_size + 1u) << 10;  // 512 KiB per unit; at most 2^22 << 10 = 2^32, which does not fit: the largest card (2 TiB) is v3
    csd.valid = c_size != 0x3FFFFFu && csd.blocks != 0u;
  } else {
    csd.version = static_cast<std::uint8_t>(structure + 1u);  // v3 (SDUC) and the reserved values: not supported
  }
  return csd;
}

}  // namespace o3sdk::platform::mk66::storage::sd
