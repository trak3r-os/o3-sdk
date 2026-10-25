// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SHA-256 (FIPS 180-4) for the simulator's golden framebuffer hashes. Host only: platform/host may use the
// standard library, nothing below the app layer includes this.

#include <cstddef>
#include <cstdint>
#include <string>

namespace o3sdk::platform::host {

class Sha256 {
 public:
  Sha256() noexcept;
  void update(const void* data, std::size_t length) noexcept;
  // Writes the 32-byte digest. The object must not be used afterwards.
  void finish(std::uint8_t digest[32]) noexcept;

 private:
  void block(const std::uint8_t* chunk) noexcept;

  std::uint32_t state_[8];
  std::uint8_t buffer_[64];
  std::size_t buffered_ = 0;
  std::uint64_t total_ = 0;  // bytes hashed so far
};

// Lower-case hex digest (64 characters) of one buffer.
std::string sha256_hex(const void* data, std::size_t length);

}  // namespace o3sdk::platform::host
