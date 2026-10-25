// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/usb/hil_inject.h>

#include <o3sdk/platform/mk66/usb/mem_access.h>

#include <cstring>

namespace o3sdk::platform::mk66::console {
namespace {

bool is_word(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

// tap | down | up
bool simple_gesture(const char* word, InjectGesture& g) noexcept {
  if (is_word(word, "tap")) {
    g = InjectGesture::Tap;
  } else if (is_word(word, "down")) {
    g = InjectGesture::Down;
  } else if (is_word(word, "up")) {
    g = InjectGesture::Up;
  } else {
    return false;
  }
  return true;
}

}  // namespace

InjectParse parse_key(unsigned argc, const char* const* argv, ButtonNameFn names, InjectRequest& out) noexcept {
  if (argc < 2 || argc > 4 || names == nullptr) {
    return InjectParse::Usage;
  }
  InjectRequest r;
  r.kind = core::InputKind::Button;
  if (argc >= 3) {
    if (is_word(argv[2], "hold")) {
      std::uint32_t ms = 0;
      if (argc != 4 || !parse_dec(argv[3], kInjectMaxHoldMs, ms) || ms == 0u) {
        return InjectParse::Usage;
      }
      r.gesture = InjectGesture::Hold;
      r.hold_ms = ms;
    } else if (argc != 3 || !simple_gesture(argv[2], r.gesture)) {
      return InjectParse::Usage;
    }
  }
  const int code = names(argv[1]);
  if (code < 0) {
    return InjectParse::Unknown;
  }
  r.id = static_cast<std::uint8_t>(code);
  out = r;
  return InjectParse::Ok;
}

InjectParse parse_pad(unsigned argc, const char* const* argv, InjectRequest& out) noexcept {
  if (argc < 2 || argc > 4) {
    return InjectParse::Usage;
  }
  InjectRequest r;
  r.kind = core::InputKind::Pad;
  r.value = 127;
  std::uint32_t pad = 0;
  if (!parse_dec(argv[1], 999u, pad)) {
    return InjectParse::Usage;
  }
  if (pad >= kInjectPads) {
    return InjectParse::Unknown;
  }
  r.id = static_cast<std::uint8_t>(pad);
  std::uint32_t velocity = 0;
  bool have_velocity = false;
  if (argc >= 3) {
    if (simple_gesture(argv[2], r.gesture)) {
      if (argc == 4) {
        have_velocity = true;
        if (!parse_dec(argv[3], 127u, velocity) || velocity == 0u) {
          return InjectParse::Usage;
        }
      }
    } else if (argc == 3 && parse_dec(argv[2], 127u, velocity) && velocity != 0u) {  // `pad 5 90`: a tap at that velocity
      have_velocity = true;
    } else {
      return InjectParse::Usage;
    }
  }
  if (have_velocity) {
    if (r.gesture == InjectGesture::Up) {
      return InjectParse::Usage;  // a release has no velocity
    }
    r.value = static_cast<std::int16_t>(velocity);
  }
  out = r;
  return InjectParse::Ok;
}

InjectParse parse_enc(unsigned argc, const char* const* argv, InjectRequest& out) noexcept {
  if (argc != 2) {
    return InjectParse::Usage;
  }
  const char* text = argv[1];
  bool negative = false;
  if (*text == '-' || *text == '+') {
    negative = *text == '-';
    ++text;
  }
  std::uint32_t n = 0;
  if (!parse_dec(text, 9999u, n)) {
    return InjectParse::Usage;
  }
  if (n == 0u || n > static_cast<std::uint32_t>(kInjectMaxDetents)) {
    return InjectParse::Unknown;
  }
  InjectRequest r;
  r.kind = core::InputKind::Encoder;
  r.id = 0;
  r.value = static_cast<std::int16_t>(negative ? -static_cast<int>(n) : static_cast<int>(n));
  out = r;
  return InjectParse::Ok;
}

InjectParse parse_enc_accel(unsigned argc, const char* const* argv, EncAccelRequest& out) noexcept {
  if (argc < 2 || !is_word(argv[1], "accel")) {
    return InjectParse::Usage;
  }
  EncAccelRequest r;
  if (argc == 2) {
    out = r;
    return InjectParse::Ok;
  }
  r.set = true;
  if (argc == 3 && is_word(argv[2], "off")) {
    r.on = false;
    out = r;
    return InjectParse::Ok;
  }
  std::uint32_t t1 = 0;
  std::uint32_t t2 = 0;
  std::uint32_t gap = 60;  // EncAccel::kDefaultGapMs
  if (argc < 4 || argc > 5 || !parse_dec(argv[2], 9999u, t1) || !parse_dec(argv[3], 9999u, t2) || (argc == 5 && !parse_dec(argv[4], 9999u, gap))) {
    return InjectParse::Usage;
  }
  if (t1 < 1u || t2 < t1 || t2 > 255u || gap < 5u || gap > 250u) {
    return InjectParse::Unknown;
  }
  r.t1 = static_cast<std::uint8_t>(t1);
  r.t2 = static_cast<std::uint8_t>(t2);
  r.gap_ms = static_cast<std::uint8_t>(gap);
  out = r;
  return InjectParse::Ok;
}

Injector::Result Injector::inject(const InjectRequest& request, std::uint32_t now_ms, PushFn push, void* context) noexcept {
  const bool needs_release = request.kind != core::InputKind::Encoder && (request.gesture == InjectGesture::Tap || request.gesture == InjectGesture::Hold);
  Slot* free_slot = nullptr;
  if (needs_release) {
    for (Slot& s : slots_) {
      if (!s.used) {
        free_slot = &s;
        break;
      }
    }
    if (free_slot == nullptr) {
      return Result::NoRoom;
    }
  }
  const bool release_only = request.kind != core::InputKind::Encoder && request.gesture == InjectGesture::Up;
  if (!push(context, request.kind, request.id, release_only ? std::int16_t{0} : (request.kind == core::InputKind::Button ? std::int16_t{1} : request.value))) {
    return Result::QueueFull;
  }
  if (needs_release) {
    free_slot->used = true;
    free_slot->due_ms = now_ms + (request.gesture == InjectGesture::Hold ? request.hold_ms : kInjectTapMs);
    free_slot->kind = request.kind;
    free_slot->id = request.id;
  }
  return Result::Ok;
}

void Injector::service(std::uint32_t now_ms, bool allowed, PushFn push, void* context) noexcept {
  if (!allowed) {
    return;
  }
  for (Slot& s : slots_) {
    if (s.used && static_cast<std::int32_t>(now_ms - s.due_ms) >= 0) {
      if (!push(context, s.kind, s.id, 0)) {
        return;  // the queue is full: this one and the later ones wait (order kept)
      }
      s.used = false;
    }
  }
}

unsigned Injector::pending() const noexcept {
  unsigned n = 0;
  for (const Slot& s : slots_) {
    n += s.used ? 1u : 0u;
  }
  return n;
}

}  // namespace o3sdk::platform::mk66::console
