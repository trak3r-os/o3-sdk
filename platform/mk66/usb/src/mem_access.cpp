// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio_config.h>
#include <o3sdk/platform/mk66/usb/mem_access.h>

namespace o3sdk::platform::mk66::console {
namespace {

static_assert(kPokeWindow.first == audio_cfg::i2s::kBase, "poke reaches the audio clock registers and nothing else");

const AccessWindow* window_of(std::uint32_t addr) noexcept {
  for (const AccessWindow& w : kPeekWindows) {
    if (addr >= w.first && addr <= w.last) {
      return &w;
    }
  }
  return nullptr;
}

}  // namespace

const char* peek_window_name(std::uint32_t addr) noexcept {
  const AccessWindow* w = window_of(addr);
  return w != nullptr ? w->name : nullptr;
}

Access check_peek(std::uint32_t addr, std::uint32_t count) noexcept {
  if ((addr & 3u) != 0u) {
    return Access::Unaligned;
  }
  if (count == 0u || count > kPeekMaxWords) {
    return Access::Refused;
  }
  const AccessWindow* w = window_of(addr);
  // addr <= w->last, so the subtraction cannot wrap; the last word starts at addr + 4 * (count - 1) and ends 3 bytes later.
  if (w == nullptr || (w->last - addr) < 4u * count - 1u) {
    return Access::Refused;
  }
  return Access::Ok;
}

Access check_poke(std::uint32_t addr) noexcept {
  if ((addr & 3u) != 0u) {
    return Access::Unaligned;
  }
  return addr >= kPokeWindow.first && addr <= kPokeWindow.last ? Access::Ok : Access::Refused;
}

bool parse_hex32(const char* text, std::uint32_t& out) noexcept {
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text += 2;
  }
  std::uint32_t v = 0;
  unsigned digits = 0;
  for (; *text != '\0'; ++text, ++digits) {
    const char c = *text;
    std::uint32_t d = 0;
    if (c >= '0' && c <= '9') {
      d = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      d = static_cast<std::uint32_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      d = static_cast<std::uint32_t>(c - 'A') + 10u;
    } else {
      return false;
    }
    if (digits == 8u) {
      return false;
    }
    v = (v << 4) | d;
  }
  if (digits == 0u) {
    return false;
  }
  out = v;
  return true;
}

bool parse_dec(const char* text, std::uint32_t max, std::uint32_t& out) noexcept {
  if (*text == '\0') {
    return false;
  }
  std::uint32_t v = 0;
  for (; *text != '\0'; ++text) {
    if (*text < '0' || *text > '9' || v > max) {
      return false;
    }
    v = v * 10u + static_cast<std::uint32_t>(*text - '0');
  }
  if (v > max) {
    return false;
  }
  out = v;
  return true;
}

}  // namespace o3sdk::platform::mk66::console
