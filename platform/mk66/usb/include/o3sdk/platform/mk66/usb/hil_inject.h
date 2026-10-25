// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The input injection of the `diag` console (OS-HIL: `key`, `pad`, `enc`): the argument parsing and the small scheduler that gives a tap or a hold its release
// later. Pure, host tested: console.cpp hands the events to the input scanner's queue (Scanner::inject), the same queue the key chips feed, so what the app sees
// is a finger's events. Nothing here knows the app: a button is a code 0..32 (app/buttons.h) that the image's name table turns from a word.
//
//   key <button> [tap|down|up|hold <ms>]    a button by name (play, f1, shift, ...) or code; tap (default): down now, up kTapMs later; hold <ms>: down now, up in 1..10000 ms
//   pad <0..47> [tap|down|up] [vel 1..127]  a pad (vel 127 by default: the pads have no velocity)
//   enc <n>                                 n detents (-64..64, not 0) as one encoder event, the sign as the knob's
//
// A release that is not due yet waits in a table of kCapacity entries; a request that would not fit is refused whole (nothing is injected). `service()` pushes the
// releases that are due, and nothing at all while `allowed` is false (an `fw` update is armed: no injection then, and a release that came due meanwhile goes out when it
// ends, so no key stays held).

#include <o3sdk/core/input_event.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

enum class InjectGesture : std::uint8_t { Tap, Down, Up, Hold };

struct InjectRequest {
  core::InputKind kind = core::InputKind::Button;
  std::uint8_t id = 0;
  std::int16_t value = 0;  // a pad's velocity 1..127, an encoder's signed delta; a button has none
  InjectGesture gesture = InjectGesture::Tap;
  std::uint32_t hold_ms = 0;
};

enum class InjectParse : std::uint8_t { Ok, Usage, Unknown };  // Unknown: a name or number that is no button, pad or count

// The code of a button word, or -1 (the image gives app::button_code).
using ButtonNameFn = int (*)(const char* name);

inline constexpr std::uint32_t kInjectTapMs = 40;
inline constexpr std::uint32_t kInjectMaxHoldMs = 10000;
inline constexpr unsigned kInjectPads = 48;
inline constexpr int kInjectMaxDetents = 64;

// `argv[0]` is the command word. These never touch anything.
InjectParse parse_key(unsigned argc, const char* const* argv, ButtonNameFn names, InjectRequest& out) noexcept;
InjectParse parse_pad(unsigned argc, const char* const* argv, InjectRequest& out) noexcept;
InjectParse parse_enc(unsigned argc, const char* const* argv, InjectRequest& out) noexcept;

// OS-AUDIT-28: `enc accel [off | <t1> <t2> [gap_ms]]`, the tuning of the encoder's acceleration in lists (app/enc_accel.h; the ranges are its EncAccel::valid). No argument: a query (set false).
// `off`: a row a detent. Otherwise the run's detents for x4 (t1) and x8 (t2), t1 <= t2, both 1..255, and the gap between two reports of one run, 5..250 ms (default 60). Not an injection:
// nothing is pushed, the request is for the app.
struct EncAccelRequest {
  bool set = false;
  bool on = true;
  std::uint8_t t1 = 0;
  std::uint8_t t2 = 0;
  std::uint8_t gap_ms = 0;
};
InjectParse parse_enc_accel(unsigned argc, const char* const* argv, EncAccelRequest& out) noexcept;

class Injector {
 public:
  static constexpr unsigned kCapacity = 8;
  // Takes one event: false when the queue is full.
  using PushFn = bool (*)(void* context, core::InputKind kind, std::uint8_t id, std::int16_t value);
  enum class Result : std::uint8_t { Ok, NoRoom, QueueFull };

  // Pushes what the request starts with and files its release. NoRoom: the table is full (nothing was pushed). QueueFull: the first push was refused (nothing is filed).
  Result inject(const InjectRequest& request, std::uint32_t now_ms, PushFn push, void* context) noexcept;
  // The releases that are due at `now_ms`, in table order; one the queue refuses stays (with the ones after it) for the next call. Nothing while `allowed` is false.
  void service(std::uint32_t now_ms, bool allowed, PushFn push, void* context) noexcept;
  unsigned pending() const noexcept;

 private:
  struct Slot {
    bool used = false;
    std::uint32_t due_ms = 0;
    core::InputKind kind = core::InputKind::Button;
    std::uint8_t id = 0;
  };
  Slot slots_[kCapacity];
};

}  // namespace o3sdk::platform::mk66::console
