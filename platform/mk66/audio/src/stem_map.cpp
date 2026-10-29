// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/audio/stem_map.h>

namespace o3sdk::platform::mk66::audio {

namespace {

constexpr std::uint16_t bit(unsigned source) noexcept { return static_cast<std::uint16_t>(1u << source); }

char upper(char c) noexcept { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

}  // namespace

bool StemMap::reset(usb::Profile p) noexcept {
  if (!supported(p)) {
    return false;
  }
  profile_ = p;
  const std::uint32_t n = usb::profile_channels(p);
  for (std::uint32_t c = 0; c < usb::kMaxChannels; ++c) {
    mask_[c] = 0;
  }
  for (std::uint32_t c = 0; c + 2u < n; ++c) {
    mask_[c] = bit(c);  // S(c+1) = T(c+1)
  }
  mask_[n - 2u] = bit(stem_src::kMasterL);
  mask_[n - 1u] = bit(stem_src::kMasterR);
  return true;
}

bool StemMap::set_slot(unsigned slot, std::uint32_t sources) noexcept {
  if (slot >= channels()) {
    return false;
  }
  mask_[slot] = static_cast<std::uint16_t>(sources & 0xFFFFu);
  return true;
}

void StemMap::apply(StemTap& tap) const noexcept {
  tap.set_channels(channels());
  for (unsigned c = 0; c < usb::kMaxChannels; ++c) {
    tap.set_slot(c, c < channels() ? mask_[c] : 0u);
  }
}

std::size_t format_sources(std::uint32_t mask, char* out, std::size_t cap) noexcept {
  if (cap == 0u) {
    return 0;
  }
  std::size_t len = 0;
  const auto put = [&](char c) {
    if (len + 1u < cap) {
      out[len++] = c;
    }
  };
  const auto put_number = [&](unsigned v) {
    if (v >= 10u) {
      put(static_cast<char>('0' + v / 10u));
    }
    put(static_cast<char>('0' + v % 10u));
  };
  bool first = true;
  for (unsigned s = 0; s < usb::kSlotSources; ++s) {
    if ((mask & (1u << s)) == 0u) {
      continue;
    }
    if (!first) {
      put('+');
    }
    first = false;
    if (s < stem_src::kTracks) {
      put('T');
      put_number(s + 1u);
    } else {
      put(s == stem_src::kMasterL || s == stem_src::kMasterR ? 'M' : 'S');
      put(s == stem_src::kMasterL ? 'L' : (s == stem_src::kMasterR ? 'R' : (s == stem_src::kSendA ? 'A' : 'B')));
    }
  }
  if (first) {
    put('-');
  }
  out[len] = '\0';
  return len;
}

bool parse_sources(const char* text, std::uint32_t& mask) noexcept {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  if (text[0] == '-' && text[1] == '\0') {
    mask = 0;
    return true;
  }
  if (upper(text[0]) == 'N' && upper(text[1]) == 'O' && upper(text[2]) == 'N' && upper(text[3]) == 'E' && text[4] == '\0') {
    mask = 0;
    return true;
  }
  std::uint32_t result = 0;
  const char* p = text;
  for (;;) {
    const char a = upper(p[0]);
    if (a == '\0') {
      return false;  // a '+' with nothing after it
    }
    const char b = upper(p[1]);
    unsigned source = 0;
    if (a == 'T' && b >= '0' && b <= '9') {
      unsigned n = static_cast<unsigned>(b - '0');
      ++p;  // now at the first digit
      if (p[1] >= '0' && p[1] <= '9') {
        n = n * 10u + static_cast<unsigned>(p[1] - '0');
        ++p;
      }
      if (n < 1u || n > stem_src::kTracks) {
        return false;
      }
      source = n - 1u;
      ++p;  // past the last digit
    } else if (a == 'M' && (b == 'L' || b == 'R')) {
      source = b == 'L' ? stem_src::kMasterL : stem_src::kMasterR;
      p += 2;
    } else if (a == 'S' && (b == 'A' || b == 'B')) {
      source = b == 'A' ? stem_src::kSendA : stem_src::kSendB;
      p += 2;
    } else {
      return false;
    }
    result |= 1u << source;
    if (*p == '\0') {
      break;
    }
    if (*p != '+') {
      return false;
    }
    ++p;
  }
  mask = result;
  return true;
}

}  // namespace o3sdk::platform::mk66::audio
