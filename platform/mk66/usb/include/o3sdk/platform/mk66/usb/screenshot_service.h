// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-SCREENSHOT (docs/specs/OS-SCREENSHOT.md): Shift then Power saves the screen as Screenshots/SHOT_nnnn.bmp. The service shares the console `shot`'s BandReader (usb/shot_stream.h): the FT812's
// coprocessor runs CMD_SNAPSHOT2 in RGB565 into the spare RAM_G region in two bands of 240 rows, and the main loop reads each band back over the SPI in pieces of kBufferBytes, at most one
// piece a pass and only when the chip's bus is free, and writes the piece to the open file. Pure: the chip sits behind ShotPort, the card behind the hal storage interfaces, the strip and the
// platform's yes/no behind ScreenshotHost; tests/mk66_screenshot_service_test.cpp and the simulator (sim/src/simulator.cpp, a ShotPort over its own frame buffer) run the same object.
//
//   request()   the Shift-then-Power press (core::PowerHold::Result::screenshot). Refused at once, with the strip's words, when a screenshot is already running, a console `shot` runs, or the card
//               cannot take a file (ScreenshotHost::card_refusal: no card, the card on the computer, not writable, storage busy).
//   WaitKeys    until Shift and Power are both released (the plain key row is then what the screen shows, not the Shift labels).
//   Prepare     the next free name (bounded: kNamesPerPass lookups a pass, the app goes on; the search starts after the last number taken or found taken, from 1 after a boot), and the first frame the app commits after the release plus kSettleMs for the swap to reach the
//               panel (or kFrameWaitMs when the app draws nothing: nothing changed).
//   Capture     step() returns true: the platform draws no frame (the display list is held, so both bands are one picture). Header + band 1 + band 2 are written to Screenshots/_o3save.tmp
//               and the file appears under its name by the rename (never over a file, the export's NewOnly rule). Audio goes on: it runs from interrupts and DMA.
//   An abort    (a card fault, the card taken away, a display reset, kStallTimeoutMs without progress) closes and removes the temporary file; the strip says SCREENSHOT FAILED. A key press does not abort.
//   cancel()    the platform ends it without a word (the shutdown's commit, a display or an app that failed): the same clean-up, the card released.
// The card is claimed (capturing()) while the file is open, so the hand-over to a computer waits.

#include <o3sdk/hal/screenshot_file.h>
#include <o3sdk/hal/storage.h>
#include <o3sdk/hal/storage_write.h>
#include <o3sdk/platform/mk66/usb/shot_stream.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::console {

// What the platform tells the service. Main loop only.
class ScreenshotHost {
 public:
  // The card as a screenshot may use it now: None when a file may be written (and no job holds the card), else NoCard, OnComputer, Unwritable or Busy.
  virtual hal::storage::shot::Refusal card_refusal() noexcept = 0;
  virtual bool console_shot_active() noexcept = 0;  // the console's `shot` stream uses the same RAM_G region
  virtual void notice(const char* text, bool warn) noexcept = 0;  // the strip

 protected:
  ScreenshotHost() = default;
  ~ScreenshotHost() = default;
  ScreenshotHost(const ScreenshotHost&) = delete;
  ScreenshotHost& operator=(const ScreenshotHost&) = delete;
};

class ScreenshotService {
 public:
  static constexpr std::size_t kBufferBytes = 2048;   // the one piece in hand: the header and the start of band 1 go out in the first write
  static constexpr unsigned kNamesPerPass = 2;        // directory lookups a pass while the next free name is searched (each walks the folder on the card)
  static constexpr std::uint32_t kKeysMaxMs = 10000;  // the keys never came up: the request lapses without a word
  static constexpr std::uint32_t kFrameWaitMs = 300;  // the app commits no frame after the release (nothing changed): take the picture anyway
  static constexpr std::uint32_t kSettleMs = 50;      // after that frame: two panel frames at 54 Hz, for the swap to reach the scan-out

  // Default-constructible (the image keeps it in the SDRAM window, built before the card's write side is); idle and refusing everything until attach(). `buffer`: kBufferBytes bytes,
  // word aligned (the caller declares and asserts it), for the service alone. `region`: the RAM_G bytes CMD_SNAPSHOT2 may write.
  void attach(ScreenshotHost& host, hal::storage::Storage& card, hal::storage::StorageWriter& writer, std::uint8_t* buffer, std::uint32_t region, std::uint32_t region_bytes) noexcept {
    host_ = &host;
    card_ = &card;
    writer_ = &writer;
    buf_ = buffer;
    region_ = region;
    region_bytes_ = region_bytes;
  }

  // The press. Says why when it is refused (the strip is told); true: armed.
  bool request(std::uint32_t now) noexcept;
  // One pass of the main loop. `keys_down`: Shift or Power is down. true: hold the app's frames this pass (a capture is running).
  bool step(std::uint32_t now, bool keys_down, ShotPort& port) noexcept;
  // Ends an armed or running request where it stands, with no strip text (the strip is going or gone). A capture in flight drops its snapshot, abandons and removes the temporary file and lets
  // go of the card (capturing() is false after); it counts as failed(). Idle: nothing happens.
  void cancel(ShotPort& port) noexcept;
  // From the request to the end: the service is armed or running, and a console `shot` is refused.
  bool active() const noexcept { return host_ != nullptr && phase_ != Phase::Idle; }
  // The capture is under way, the file open: the card is in use (the claim of the hand-over). Before it nothing is held across passes but a name search, which fails cleanly on a card that went.
  bool capturing() const noexcept { return phase_ == Phase::Capture; }
  // For `diag`: the numbers of the last run.
  std::uint32_t saved() const noexcept { return saved_; }
  std::uint32_t failed() const noexcept { return failed_; }
  std::uint32_t refused() const noexcept { return refused_; }

 private:
  enum class Phase : std::uint8_t { Idle, WaitKeys, Prepare, Capture };
  hal::storage::shot::Refusal refusal_now() noexcept;
  void refuse(hal::storage::shot::Refusal why) noexcept;
  void scan_names() noexcept;
  bool begin_capture(std::uint32_t now, ShotPort& port) noexcept;
  bool capture_pass(std::uint32_t now, ShotPort& port) noexcept;
  void finish() noexcept;
  void abort(ShotPort& port) noexcept;
  void drop_file() noexcept;

  ScreenshotHost* host_ = nullptr;
  hal::storage::Storage* card_ = nullptr;
  hal::storage::StorageWriter* writer_ = nullptr;
  std::uint8_t* buf_ = nullptr;
  std::uint32_t region_ = 0;
  std::uint32_t region_bytes_ = 0;
  BandReader reader_;
  Phase phase_ = Phase::Idle;
  std::uint32_t since_ = 0;        // WaitKeys: since when; Prepare: the release
  std::uint32_t frames_ = 0;       // the frame count at the release
  std::uint32_t seen_at_ = 0;
  bool frame_seen_ = false;
  unsigned cursor_ = hal::storage::shot::kFirstNumber;  // where the next search starts (RAM: 1 after a boot): the number after the last one taken, and every number a search found taken, so a search that was cut off resumes
  unsigned scan_n_ = 0;
  unsigned scan_tried_ = 0;
  unsigned number_ = 0;            // the name found (0: still searching)
  bool first_ = true;              // the next piece is the first: it carries the header
  std::uint32_t written_ = 0;
  std::uint32_t saved_ = 0;
  std::uint32_t failed_ = 0;
  std::uint32_t refused_ = 0;
};

}  // namespace o3sdk::platform::mk66::console
