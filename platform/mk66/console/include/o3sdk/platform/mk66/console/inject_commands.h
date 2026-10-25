// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `key`, `pad` and `enc` (OS-HIL): the commands that give the app a finger's events. The parsing and the release scheduler are platform/mk66/usb/hil_inject.h; this is the handler
// that glues them to the shell, with the image's four needs behind a port (the scanner's queue, whether the firmware path allows injection, the clock, the log) so that the
// handler is the same code on the unit and in the host test. The bodies are track3r-os platform/mk66/usb/src/console.cpp's (run_inject, cmd_key, cmd_pad, cmd_enc, and gesture_name);
// the only rewritten lines are the ones that named a global (g_tee, injection_allowed, g_injector, uptime_ms32, log).

#include <o3sdk/core/input_event.h>
#include <o3sdk/platform/mk66/usb/hil_inject.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstdint>

namespace o3sdk::platform::mk66::console {

class InjectPort {
 public:
  // The app has started and its input is tapped (track3r-os: the tee exists).
  virtual bool input_up() noexcept = 0;
  // No `fw` update is armed or running: a stray key must not be what cancels or confirms anything.
  virtual bool allowed() noexcept = 0;
  // One event into the scanner's own queue (Scanner::inject). false: the queue is full.
  virtual bool push(core::InputKind kind, std::uint8_t id, std::int16_t value) noexcept = 0;
  virtual std::uint32_t now_ms() noexcept = 0;
  virtual void log(const char* text) noexcept = 0;

 protected:
  InjectPort() = default;
  ~InjectPort() = default;
  InjectPort(const InjectPort&) = delete;
  InjectPort& operator=(const InjectPort&) = delete;
};

class InjectCommands {
 public:
  explicit InjectCommands(InjectPort& port) noexcept : port_(port) {}

  // Once a pass of the main loop: the releases that are due (nothing while the port does not allow injection; they go out when it ends, so no key stays held).
  void service() noexcept;

  void key(usb::diag::Shell& shell, const usb::diag::Args& args) noexcept;
  void pad(usb::diag::Shell& shell, const usb::diag::Args& args) noexcept;
  void enc(usb::diag::Shell& shell, const usb::diag::Args& args) noexcept;

 private:
  void run(usb::diag::Shell& shell, InjectParse parsed, const InjectRequest& request, const char* what, const char* unknown, const char* usage) noexcept;
  static bool push_thunk(void* self, core::InputKind kind, std::uint8_t id, std::int16_t value);

  InjectPort& port_;
  Injector injector_;
};

// The shell's handlers, over the instance bind_inject() was given (one per image; none: "ERR no input: the app has not started").
void bind_inject(InjectCommands* commands) noexcept;
void key_command(usb::diag::Shell& shell, const usb::diag::Args& args);
void pad_command(usb::diag::Shell& shell, const usb::diag::Args& args);
void enc_command(usb::diag::Shell& shell, const usb::diag::Args& args);

}  // namespace o3sdk::platform::mk66::console
