// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/platform/mk66/console/inject_commands.h>

#include <o3sdk/platform/mk66/audio/diag.h>
#include <o3sdk/platform/mk66/console/core.h>
#include <o3sdk/usb/diag_log.h>

namespace o3sdk::platform::mk66::console {

namespace shell = o3sdk::usb::diag;

namespace {

InjectCommands* g_commands = nullptr;

const char* gesture_name(InjectGesture g) {
  switch (g) {
    case InjectGesture::Tap: return "tap";
    case InjectGesture::Down: return "down";
    case InjectGesture::Up: return "up";
    case InjectGesture::Hold: return "hold";
  }
  return "?";
}

}  // namespace

bool InjectCommands::push_thunk(void* self, core::InputKind kind, std::uint8_t id, std::int16_t value) {
  return static_cast<InjectCommands*>(self)->port_.push(kind, id, value);
}

void InjectCommands::service() noexcept { injector_.service(port_.now_ms(), port_.allowed(), &InjectCommands::push_thunk, this); }

// (run_inject of track3r-os console.cpp: the port stands for g_tee, injection_allowed(), uptime_ms32() and log().)
void InjectCommands::run(shell::Shell& sh, InjectParse parsed, const InjectRequest& r, const char* what, const char* unknown, const char* usage) noexcept {
  if (parsed == InjectParse::Usage) {
    sh.out().text("usage: ").text(usage).nl();
    return;
  }
  if (parsed == InjectParse::Unknown) {
    sh.out().text("ERR ").text(unknown).nl();
    return;
  }
  if (!port_.input_up()) {
    sh.out().text("ERR no input: the app has not started\n");
    return;
  }
  if (!port_.allowed()) {
    sh.out().text("ERR refused: an fw update is armed or running\n");
    return;
  }
  const Injector::Result result = injector_.inject(r, port_.now_ms(), &InjectCommands::push_thunk, this);
  if (result != Injector::Result::Ok) {
    sh.out().text(result == Injector::Result::NoRoom ? "ERR too many releases pending, try again\n" : "ERR the input queue is full, try again\n");
    return;
  }
  char line[shell::kLogTextMax + 1];  // the log: every injected input is marked as one (a person at the unit can tell it from a finger)
  audio::diag::Text t(line, sizeof line);
  t.str("inject ").str(what).chr(' ').dec(r.id);
  if (r.kind == core::InputKind::Encoder) {
    t.chr(' ').chr(r.value < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(r.value < 0 ? -r.value : r.value));
  } else {
    t.chr(' ').str(gesture_name(r.gesture));
    if (r.gesture == InjectGesture::Hold) {
      t.chr(' ').dec(r.hold_ms);
    }
  }
  port_.log(line);
  sh.out().text("ok ").text(what).chr(' ');
  if (r.kind == core::InputKind::Encoder) {
    sh.out().chr(r.value < 0 ? '-' : '+').dec(static_cast<std::uint32_t>(r.value < 0 ? -r.value : r.value));
  } else {
    sh.out().dec(r.id);
  }
  sh.out().nl();
}

void InjectCommands::key(shell::Shell& sh, const shell::Args& a) noexcept {
  InjectRequest r;
  const InjectParse p = parse_key(a.argc, a.argv, &button_code, r);
  run(sh, p, r, "key", "unknown button (a name like f1, play, shift, or 0..32)", "key <button> [tap|down|up|hold <ms>]   button by name (f1, play, shift ...) or 0..32");
}

void InjectCommands::pad(shell::Shell& sh, const shell::Args& a) noexcept {
  InjectRequest r;
  const InjectParse p = parse_pad(a.argc, a.argv, r);
  run(sh, p, r, "pad", "pad is 0..47", "pad <0..47> [tap|down|up] [vel 1..127]");
}

// (`enc accel` of track3r-os's cmd_enc is cut: o3-sdk has no encoder acceleration yet.)
void InjectCommands::enc(shell::Shell& sh, const shell::Args& a) noexcept {
  InjectRequest r;
  const InjectParse p = parse_enc(a.argc, a.argv, r);
  run(sh, p, r, "encoder", "n is -64..64, not 0", "enc <n>   n detents, -64..64, not 0 (minus = counter-clockwise)");
}

void bind_inject(InjectCommands* commands) noexcept { g_commands = commands; }

void key_command(shell::Shell& sh, const shell::Args& a) {
  if (g_commands == nullptr) {
    sh.out().text("ERR no input: the app has not started\n");
    return;
  }
  g_commands->key(sh, a);
}

void pad_command(shell::Shell& sh, const shell::Args& a) {
  if (g_commands == nullptr) {
    sh.out().text("ERR no input: the app has not started\n");
    return;
  }
  g_commands->pad(sh, a);
}

void enc_command(shell::Shell& sh, const shell::Args& a) {
  if (g_commands == nullptr) {
    sh.out().text("ERR no input: the app has not started\n");
    return;
  }
  g_commands->enc(sh, a);
}

}  // namespace o3sdk::platform::mk66::console
