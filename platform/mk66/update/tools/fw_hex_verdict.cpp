// SPDX-License-Identifier: GPL-3.0-or-later
// Host helper of ctest `mk66_fw_hex_cross` (OS-006c): runs the device's streaming HEX validator over files and prints one line each,
//   <path> TAB ok TAB <image length> TAB <crc32 of the file, 8 hex digits>
//   <path> TAB fail TAB <reason> TAB <offset>
// so that platform/mk66/tools/fw_hex_cross.py can compare its verdicts with tools/check_image.py's on the same files. The files are fed in 1,000-byte
// pieces. Not part of any image.
#include <o3sdk/platform/mk66/update/hex_validator.h>

#include <cstdint>
#include <cstdio>

int main(int argc, char** argv) {
  namespace up = o3sdk::platform::mk66::update;
  int status = 0;
  for (int i = 1; i < argc; ++i) {
    std::FILE* f = std::fopen(argv[i], "rb");
    if (f == nullptr) {
      std::printf("%s\terror\tcannot open\t0\n", argv[i]);
      status = 2;
      continue;
    }
    up::HexValidator v;
    std::uint8_t buf[1000];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) != 0u && v.feed(buf, n)) {
    }
    std::fclose(f);
    if (v.finish()) {
      std::printf("%s\tok\t%lu\t%08lX\n", argv[i], static_cast<unsigned long>(v.image_length()), static_cast<unsigned long>(v.crc()));
    } else {
      std::printf("%s\tfail\t%s\t%lu\n", argv[i], up::fault_name(v.fault()), static_cast<unsigned long>(v.fault_offset()));
    }
  }
  return status;
}
