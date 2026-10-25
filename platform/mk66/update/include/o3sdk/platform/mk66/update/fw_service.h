// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The `fw` command of the `diag` console and the state machines behind it (OS-006c). Pure: the store, the host's side effects (the clock, the outputs,
// the PTA1 pin) and the shell are arguments, so the host tests and QEMU run the very code the image runs, with a fake store and the real shell and pump.
//
//   fw                              the write path (enabled, why not, the volume serial, write protect), the Firmware/ listing in directory order with
//                                   the first .ptf (the reset button's target), the upload state, the last result
//   fw put <name> <bytes> <crc32>   receive a HEX file of exactly <bytes> bytes into Firmware/<name> (see below)
//   fw rm <name>                    remove a temporary file or a dev-namespace image (path_guard.h removable_name), never the last .ptf
//   fw arm <name> <crc32>           re-read and fully re-validate Firmware/<name>, check its CRC-32, print `armed <token>`
//   fw update <token>               hand over to the bootloader (below)
//
// `fw put`. One `ERR <reason>` line and nothing else on a bad name, an existing target, no Firmware/ folder, size 0 or over 4 MiB, an
// upload in progress or a project loading. Otherwise the temporary file Firmware/_o3up.tmp is created (the room reserved) and `ready` is printed. The
// receiver then OWNS THE TERMINAL (Shell::start_raw: no echo, no line editor) and takes exactly <bytes> raw bytes of the HEX text. It validates while it
// receives (HexValidator), writes a buffer at a time (at most one write per pass of the main loop), and acknowledges `+<received>` at least every 4096
// bytes (it does so every 2048: the host sends at most 4096 bytes past the last acknowledgement, so the receive ring never overruns). On the first error
// it prints `ERR <reason> at <offset>`, deletes the temporary file and discards the rest of the declared bytes (or ends after 5 s of silence, the same
// timeout that aborts a stalled upload), then the prompt returns. At the end of the bytes: the validator's final checks, the CRC-32 against the announced
// one, sync and close, a FULL READ-BACK of the temporary file through a fresh validator (length, CRC, the same checks), the rename to Firmware/<name>, and
// `ok <name> <bytes> <crc32> image <length>` and `first-ptf <name>`.
//
// While the card is handed to a computer (OS-USBDISK) `put`, `rm`, `arm` and `update` answer `ERR card on host` and touch nothing, and a token armed before a hand-over is
// dead after it (FwHost::host_sessions).
//
// `fw arm` / `fw update`. `arm` prints an 8-digit hexadecimal token (mixed from the clock and the hardware's entropy; one use; valid 30 s; any other `fw`
// command cancels it). `update <token>` accepts only that token, re-validates the file once more (as `arm` does: every check and the CRC), then stops the sequencer and mutes the outputs (FwHost::quiesce), deletes a stale
// Firmware/_fwinfo, writes the bare image name into a new one, reads it back, prints `update: handing over to the bootloader (<name>)`, lets the console's
// output drain (at most 40 ms), then drives the bootloader request pin low (FwHost::bootloader_request). If the chip is still running 5 s later the pin is
// released, _fwinfo is deleted and `ERR bootloader did not take over` is printed (USB keeps being serviced meanwhile: the wait is a stream of the shell).
//
// The client API (OS-FWCONFIG), for a screen that has no shell: ui_check_begin(name) is `fw arm` without a CRC to compare and without a token, ui_install() is `fw update`
// for the file that check passed, and ui_slice() runs the slices of either (2 KiB of the file a call, the buffer and the validator the console uses). There is ONE implementation of
// the check (verify_slice), of the hand-over (begin_handover) and of the wait for the bootloader (handover_check): they take the shell as a pointer and print only when they have
// one. Rules:
//   * one owner at a time: ui_check_begin() is refused (Busy) while any console operation is in progress (phase() not Idle or Done), and every console command is answered
//     `ERR busy` while a client operation is. A console command (any `fw`) also drops a client check that is waiting for ui_install(), as it drops a token; a ui_check_begin()
//     that starts drops a token (one that is refused changes nothing: a good check that waits stays good). ui_install() is Busy, touching nothing, while a re-check or the
//     hand-over runs: a second INSTALL cannot turn the job the screen reports into a failure;
//   * a client check is good for kArmLifetimeMs and only while the card has not been handed to a computer; ui_install() re-validates the file once more (every check and the
//     CRC-32 of the check) before anything is touched, then does exactly what `fw update` does. ui_install() does not wait: the re-check and the wait for the bootloader are
//     slices of ui_slice(). poll() ends the wait by the clock even if nobody calls ui_slice();
//   * a closed terminal does not abort a client operation. Nothing the client does prints or needs the shell;
//   * the listing shares the store's ONE directory handle with the console's `fw` and with `fw put`: ui_list_begin() .. ui_list_end() is read in ONE pass of the main loop (the screen
//     copies the entries), never held across passes. Another listing opened meanwhile closes this one, and list_next() then answers false.

#include <o3sdk/platform/mk66/update/crc32.h>
#include <o3sdk/platform/mk66/update/fault.h>
#include <o3sdk/platform/mk66/update/firmware_store.h>
#include <o3sdk/platform/mk66/update/hex_validator.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::update {

inline constexpr std::uint32_t kIdleTimeoutMs = 5000;    // no byte for this long ends an upload
inline constexpr std::uint32_t kAckEveryBytes = 2048;    // an acknowledgement at least this often; the host's window is 4096
inline constexpr std::uint32_t kArmLifetimeMs = 30000;   // a token expires after this
inline constexpr std::uint32_t kHandoverWaitMs = 5000;   // the bootloader has this long to take over before the pin is released
inline constexpr std::uint32_t kDrainMs = 40;            // the console's output may take this long to leave before the pin goes low
inline constexpr std::size_t kTransferBuffer = 2048;     // one write (four sectors) or one read-back slice per pass of the main loop

// What the image does for the update path, and the tests fake: the clock, entropy, the outputs and the pin. Main loop only.
class FwHost {
 public:
  virtual std::uint32_t now_ms() noexcept = 0;
  virtual std::uint32_t entropy() noexcept = 0;              // a hardware-derived word (cycle counter, unique id, uptime)
  virtual bool storage_busy() noexcept = 0;                  // the card is in use by something else (a project is loading, a save is running)
  virtual bool card_on_host() noexcept = 0;                  // the card is handed to a computer (OS-USBDISK): every `fw put|rm|arm|update` answers `ERR card on host`
  virtual std::uint32_t host_sessions() noexcept = 0;        // counts the hand-overs to a computer: a token armed before one is dead after it (the card may have been rewritten)
  virtual void quiesce() noexcept = 0;                       // stop the sequencer's transport and mute the outputs
  virtual void drain_output(std::uint32_t max_ms) noexcept = 0;  // let the console's output leave, waiting at most max_ms
  virtual void bootloader_request(bool low) noexcept = 0;    // true: drive the pin low; false: release it (input)
  virtual void log(const char* text) noexcept = 0;           // one line (55 characters) into the diag log

 protected:
  FwHost() = default;
  ~FwHost() = default;
  FwHost(const FwHost&) = delete;
  FwHost& operator=(const FwHost&) = delete;
};

enum class FwPhase : std::uint8_t { Idle, Receiving, Discarding, Finalizing, Verifying, Arming, Rechecking, Handover, Done };

// The update path holds the card from `fw put` to the end of `fw update`'s hand-over (OS-007o); Idle and Done do not. One of the claims of the card (storage/card_claims.h).
constexpr bool fw_phase_holds_card(FwPhase p) noexcept { return p != FwPhase::Idle && p != FwPhase::Done; }

struct FwLast {
  enum class Kind : std::uint8_t { None, Put, Arm, Update } kind = Kind::None;
  Fault fault = Fault::Ok;
  std::uint32_t offset = 0;
  std::uint32_t bytes = 0;
  std::uint32_t crc = 0;
  std::uint32_t image = 0;
  char name[kImageNameMax + 1] = {};
};

// What a client operation is doing (ui_slice()).
struct UiStatus {
  enum class State : std::uint8_t { Idle, Running, Checked, Failed, DidNotStart };
  State state = State::Idle;
  bool installing = false;      // the job is ui_install()'s
  Fault fault = Fault::Ok;      // Failed
  std::uint32_t offset = 0;     // Failed: the byte of the file where the fault was found
  std::uint32_t done = 0;       // Running: bytes of the file read
  std::uint32_t total = 0;      // Running: the file's size
  std::uint32_t image = 0;      // Checked: the image's length
  std::uint32_t crc = 0;        // Checked: the CRC-32 of the whole file
};

class FwService {
 public:
  FwService(FirmwareStore& store, FwHost& host) noexcept : store_(store), host_(host) {}

  // The command (argv[0] is "fw"). Registered by the image's command table; the tests call it through a real Shell.
  void run(usb::diag::Shell& sh, const usb::diag::Args& args) noexcept;

  // Once per pass of the main loop, whatever the terminal does: a hand-over that has started ends by its own clock (the pin is released and _fwinfo deleted
  // after kHandoverWaitMs) even if the terminal was closed meanwhile and its stream is gone.
  void poll(usb::diag::Shell& sh) noexcept;
  // The terminal went away (DTR dropped: the cable was pulled or the program closed) or a new one opened: an upload or a re-validation in progress is
  // abandoned (the temporary file deleted, nothing printed) and a token is cancelled. A hand-over in progress is left to poll().
  void terminal_closed() noexcept;

  // ---- the client API (see the top of this file)
  // The *.ptf files of Firmware/ in directory order, with `first` set on the first one (the reset button's target). Refused like a check: Busy, OnHost, CardBusy. One pass of the
  // main loop from begin to end (the store has one directory handle; see the top of this file).
  Fault ui_list_begin() noexcept;
  bool ui_list_next(FwEntry& out, bool& first) noexcept;
  void ui_list_end() noexcept;
  // Starts the check of Firmware/<name>: BadName, Busy, OnHost, CardBusy, the write path's reason (Disabled, NoCard, WriteProtected, NoDir, Unsupported), NotFound, Io. Ok: ui_slice() runs it.
  Fault ui_check_begin(const char* name) noexcept;
  // One slice of the running check or install, or the state of the last one. Call once per pass of the main loop.
  UiStatus ui_slice() noexcept;
  // Starts the install of the file the last check passed: Busy (a re-check, the hand-over or a console job runs: nothing is touched), NotArmed, Expired, OnHost, CardBusy, the write
  // path's reason, Readback (the file changed). A refusal other than Busy uses the check up (one use). Ok: ui_slice() runs the re-check and the wait.
  Fault ui_install() noexcept;
  // Drops a check in progress, or a result waiting for ui_install(). An install that has reached the hand-over cannot be dropped. The screen calls it on EVERY way out (a check
  // that is left running holds the card; a passed one that is left waiting keeps the HIL injection refused until it lapses).
  void ui_cancel() noexcept;

  FwPhase phase() const noexcept { return phase_; }
  const FwLast& last() const noexcept { return last_; }
  bool armed() const noexcept { return armed_; }
  // The HIL console injects no key while this is true: a console token is armed, a passed client check waits for its INSTALL question (only a pressed key answers it), or any
  // put / arm / check / hand-over is running (the token is bound to what the unit shows, and a stray key must not be what cancels or confirms anything).
  bool blocks_injection() const noexcept { return armed_ || ui_armed_ || phase_ != FwPhase::Idle; }

 private:
  static std::size_t raw_thunk(usb::diag::Shell& sh, const std::uint8_t* bytes, std::size_t n, void* self) noexcept;
  static bool stream_thunk(usb::diag::Shell& sh, void* self) noexcept;

  void status(usb::diag::Shell& sh) noexcept;
  void put(usb::diag::Shell& sh, const usb::diag::Args& a) noexcept;
  void rm(usb::diag::Shell& sh, const usb::diag::Args& a) noexcept;
  void arm(usb::diag::Shell& sh, const usb::diag::Args& a) noexcept;
  void update(usb::diag::Shell& sh, const usb::diag::Args& a) noexcept;
  std::size_t take(usb::diag::Shell& sh, const std::uint8_t* bytes, std::size_t n) noexcept;
  bool tick(usb::diag::Shell& sh) noexcept;
  // The shared halves of the console and the client API: `sh` null means a client operation (nothing is printed, the outcome goes to ui_).
  bool verify_slice(usb::diag::Shell* sh) noexcept;
  void fail(usb::diag::Shell* sh, Fault f, std::uint32_t offset) noexcept;
  bool handover_check(usb::diag::Shell* sh) noexcept;
  bool begin_handover(usb::diag::Shell* sh) noexcept;
  Fault begin_check(const char* name) noexcept;
  Fault begin_recheck() noexcept;
  void ui_end(UiStatus::State state, Fault f, std::uint32_t offset) noexcept;
  void ui_expire() noexcept;
  void finish_ok(usb::diag::Shell& sh) noexcept;
  void print_first_ptf(usb::diag::Shell& sh) noexcept;

  FirmwareStore& store_;
  FwHost& host_;
  FwPhase phase_ = FwPhase::Idle;
  FwLast last_;

  // an upload in progress (and, for `arm`, the file being re-validated)
  HexValidator v_;
  std::uint8_t buf_[kTransferBuffer];
  std::size_t fill_ = 0;
  char name_[kImageNameMax + 1] = {};
  std::uint32_t declared_ = 0;      // bytes announced
  std::uint32_t crc_announced_ = 0;
  std::uint32_t received_ = 0;
  std::uint32_t acked_ = 0;
  std::uint32_t remaining_ = 0;
  std::uint32_t idle_since_ = 0;
  std::uint32_t image_len_ = 0;
  std::uint32_t crc_seen_ = 0;
  std::uint32_t read_total_ = 0;
  std::uint32_t read_size_ = 0;

  // arm
  bool armed_ = false;
  std::uint32_t token_ = 0;
  std::uint32_t armed_at_ = 0;
  std::uint32_t armed_size_ = 0;
  std::uint32_t armed_crc_ = 0;
  std::uint32_t armed_sessions_ = 0;  // host_sessions() when the token was made: a different count at `update` or at poll() kills the token
  char armed_name_[kImageNameMax + 1] = {};
  char armed_serial_[10] = {};
  std::uint32_t counter_ = 0;
  std::uint32_t handover_at_ = 0;

  // the client API: client_ is true from ui_check_begin() / ui_install() until a console command takes the service back; ui_armed_ is a passed check that ui_install() may use (it
  // shares the armed_* fields, the console's token flag armed_ stays false); ui_ is what ui_slice() reports. Main loop only, like everything here.
  bool client_ = false;
  bool ui_armed_ = false;
  bool list_first_ = false;
  UiStatus ui_;
};

}  // namespace o3sdk::platform::mk66::update
