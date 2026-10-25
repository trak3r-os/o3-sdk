// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// `diag shot` (OS-HIL): the screen of the unit as text on the console. The FT812's coprocessor takes the picture (CMD_SNAPSHOT2 into a free region of RAM_G, a band
// of rows at a time because the region is smaller than a whole screen), and the main loop reads it back over the SPI in lines of kLineBytes bytes and prints them as
// base64 with a CRC-32 each. It is a stream of the console's shell: ONE call of step() a pass of the main loop, at most kLinesPerPass lines (kLinesPerPass * kLineBytes
// bytes over the SPI, about 0.4 ms) and only when the chip's bus is free and the output has room for them, so neither the audio (interrupts and DMA), nor the display
// (its frames wait at most one pass for the bus), nor the console (nothing is dropped from its ring) ever stalls. Pure: the chip sits behind ShotPort, the text goes
// through the shell's Output; tests/mk66_shot_stream_test.cpp runs it against a fake chip.
//
// The text, one line each, all ASCII:
//   SHOT BEGIN fmt=565 w=800 h=480 bytes=768000 line=96
//   SHOT D <offset: 6 hex digits> <base64 of the bytes at that offset of the image> <CRC-32 of those bytes, 8 hex digits>      (many)
//   SHOT END lines=<n> crc=<CRC-32 of the whole image, 8 hex digits> moved=<0|1>
//   SHOT ERR <reason>                                                                                                           (instead of END)
// The image is the screen row by row, top first, little endian pixels (RGB565: 2 bytes; ARGB8: 4 bytes in the chip's order, B G R A), `w` * bytes-per-pixel bytes a row.
// Offsets are absolute in the image, ascending; a line is kLineBytes bytes except the last of a band. `moved=1`: the app committed a frame while the bands were taken,
// so they may belong to different pictures (the host can ask again). tools/o3console.py --shot turns it into a PNG.

#include <o3sdk/platform/mk66/update/crc32.h>
#include <o3sdk/usb/diag_shell.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::console {

enum class ShotFormat : std::uint8_t { Rgb565, Argb8 };

// The `w` CMD_SNAPSHOT2 is given when nothing says otherwise. The chip counts `w` in 16-bit units, so an RGB565 row of the screen's 800 pixels is w = 800 and an ARGB8 row is
// w = 1600. Seen on a 0.4.8 unit (OS-SIMKEY): `shot 8888` with w = 800 made the chip write 1600 bytes a
// row (the left 400 pixels of the screen), which read back as 3200-byte rows is two screen rows side by side, and the second half of each band was what the region held before (white).
inline constexpr std::uint32_t kArgb8WidthParam = 1600;

struct ShotParams {
  ShotFormat format = ShotFormat::Rgb565;
  std::uint32_t width_param = 800;  // what CMD_SNAPSHOT2 is given as `w`: default_width_param(format) unless the console names one (kArgb8WidthParam for ARGB8)
  std::uint32_t height = 480;       // rows of the picture, from the top (1..kHeight): the console always takes the whole screen; the host tests take a few rows
};

constexpr std::uint32_t default_width_param(ShotFormat format) noexcept { return format == ShotFormat::Argb8 ? kArgb8WidthParam : 800u; }

// The chip, as the stream needs it. Main loop only.
class ShotPort {
 public:
  enum class Capture : std::uint8_t { Pending, Done, Fault };
  virtual bool ready() noexcept = 0;     // the display is up and its chip answers
  virtual bool bus_free() noexcept = 0;  // no frame or upload transfer owns the SPI: a transaction of ours may run
  // Queues CMD_SNAPSHOT2 of `rows` rows from row `y`, `width_param` as its w, into RAM_G at `addr` in the chip's format code `format`. false: the command FIFO took nothing.
  virtual bool start_capture(std::uint32_t format, std::uint32_t addr, std::uint32_t y, std::uint32_t width_param, std::uint32_t rows) noexcept = 0;
  virtual Capture poll_capture() noexcept = 0;  // the coprocessor has run it (Done), is running it (Pending), or stopped (Fault: the port has recovered it)
  virtual void abort_capture() noexcept = 0;    // drop a command that is queued or stuck: reset the coprocessor and its FIFO pointers (the driver's own recovery)
  virtual std::uint32_t reinits() noexcept = 0; // how many times the display driver has set the chip up again (its RAM_G is lost each time)
  virtual void read(std::uint32_t addr, std::uint8_t* out, std::size_t n) noexcept = 0;  // RAM_G, one chip-select cycle
  virtual std::uint32_t frames() noexcept = 0;  // frames the app has committed so far
  virtual std::uint32_t now_ms() noexcept = 0;
  // The console's `shot ... pclk0`: stop the panel clock while a band is taken. The flag stays set until the next caller says otherwise, so the screenshot service clears it when it starts. Default: no clock to stop.
  virtual void stop_clock(bool) noexcept {}

 protected:
  ShotPort() = default;
  ~ShotPort() = default;
  ShotPort(const ShotPort&) = delete;
  ShotPort& operator=(const ShotPort&) = delete;
};

// The band reader (OS-SCREENSHOT split it out of the console's stream, which and the screenshot service now share): the bands of a picture taken into a RAM_G region one after the
// other, each CMD_SNAPSHOT2 started when the bus is free, polled until the coprocessor has run it, then read out in pieces the caller sizes. It owns the geometry, the timeouts and the
// recovery (a display set up again meanwhile, a display that stopped, a snapshot that never ends, a progress that stalls). It prints nothing and knows no sink; the caller asks it for a
// piece when it has room for one. Main loop only: one step() a pass.
class BandReader {
 public:
  enum class State : std::uint8_t { Idle, StartBand, WaitBand, Reading, Done, Failed };

  static constexpr std::uint32_t kWidth = 800;
  static constexpr std::uint32_t kHeight = 480;
  static constexpr std::uint32_t kCaptureTimeoutMs = 400;  // CMD_SNAPSHOT2 of one band (two frames at the panel's 54 Hz is 37 ms)
  static constexpr std::uint32_t kStallTimeoutMs = 3000;   // no progress (the sink does not take, the bus is never free): give up
  static constexpr std::uint32_t kMaxRegionBytes = 0x100000u;

  // Arms the reader for a picture taken into RAM_G [region, region + region_bytes); nothing is touched yet. The bands are sized so that what the chip may write (rows * max(the
  // screen's row, width_param pixels)) fits the region. false: the parameters are not usable (a region that is not inside RAM_G or cannot hold one row of max(the screen's,
  // width_param pixels), a width parameter of 0 or past 4095, a height of 0 or past kHeight).
  bool start(const ShotParams& params, std::uint32_t region, std::uint32_t region_bytes) noexcept;
  // The first pass of a run (a later call does nothing): the stall clock starts and the count of the display's set-ups is taken.
  void begin(ShotPort& port, std::uint32_t now) noexcept;
  // One pass: the checks (the display was set up again, the display stopped: Failed), then StartBand starts the snapshot when the bus is free, WaitBand polls it, Reading stays (the
  // caller reads). Returns the state after.
  State step(ShotPort& port, std::uint32_t now) noexcept;
  // In Reading: reads up to `cap` bytes of the band, never past its end, from RAM_G into `out`; `offset` is where the first of them lies in the image. The caller has asked
  // port.bus_free(). Returns the count (0 when not Reading). After the last byte of a band the state is StartBand, or Done after the last band.
  std::size_t read(ShotPort& port, std::uint8_t* out, std::size_t cap, std::uint32_t now, std::uint32_t& offset) noexcept;
  // The caller made progress of its own (a piece was taken, the first line was printed): the stall clock starts over.
  void touch(std::uint32_t now) noexcept { progress_ms_ = now; }
  bool stalled(std::uint32_t now) const noexcept { return now - progress_ms_ > kStallTimeoutMs; }
  // The reader ends here (its caller gave up): a snapshot still queued is dropped.
  void cancel(ShotPort& port) noexcept;
  State state() const noexcept { return state_; }
  const char* error() const noexcept { return error_; }  // why Failed, a few words
  std::uint32_t left_in_band() const noexcept { return band_bytes_ - band_done_; }
  std::uint32_t rows() const noexcept { return height_; }
  std::uint32_t row_bytes() const noexcept { return row_bytes_; }

 private:
  State fail(const char* reason) noexcept {
    error_ = reason;
    state_ = State::Failed;
    return state_;
  }

  State state_ = State::Idle;
  const char* error_ = "";
  ShotParams params_{};
  std::uint32_t region_ = 0;
  std::uint32_t row_bytes_ = 0;
  std::uint32_t band_rows_ = 0;
  std::uint32_t band_ = 0;           // the band in hand
  std::uint32_t bands_ = 0;
  std::uint32_t height_ = 480;
  std::uint32_t band_row0_ = 0;
  std::uint32_t band_bytes_ = 0;
  std::uint32_t band_done_ = 0;      // bytes of the band read
  std::uint32_t band_started_ms_ = 0;
  std::uint32_t progress_ms_ = 0;
  bool fresh_ = false;               // no pass has run since start(): the stall clock has no zero yet
  std::uint32_t reinits_at_start_ = 0;
};

class ShotStream {
 public:
  static constexpr std::uint32_t kWidth = BandReader::kWidth;
  static constexpr std::uint32_t kHeight = BandReader::kHeight;
  static constexpr std::size_t kLineBytes = 96;                 // raw bytes a data line carries (128 base64 characters)
  static constexpr unsigned kLinesPerPass = 4;                  // read from the chip in one call of step()
  static constexpr std::size_t kLineTextMax = 160;              // the longest line, CR LF included: what the output must have room for per line
  static constexpr std::uint32_t kCaptureTimeoutMs = BandReader::kCaptureTimeoutMs;
  static constexpr std::uint32_t kStallTimeoutMs = BandReader::kStallTimeoutMs;
  static constexpr std::uint32_t kMaxRegionBytes = BandReader::kMaxRegionBytes;

  // Arms the stream for a picture taken into RAM_G [region, region + region_bytes); nothing is printed or touched yet (BandReader::start has the rules). false: the parameters
  // are not usable.
  bool start(const ShotParams& params, std::uint32_t region, std::uint32_t region_bytes) noexcept;
  // One pass. true: still running. false: finished (END or ERR printed) or not started.
  bool step(usb::diag::Output& out, ShotPort& port) noexcept;
  bool active() const noexcept { return phase_ != Phase::Idle; }
  // The stream was interrupted: nothing is owed to the host; a snapshot still queued in the coprocessor is dropped (abort_capture).
  void cancel(ShotPort& port) noexcept;

  // RFC 4648 base64 with '=' padding; `out` gets 4 * ceil(n / 3) characters, no NUL. Returns how many.
  static std::size_t base64(const std::uint8_t* in, std::size_t n, char* out) noexcept;

 private:
  enum class Phase : std::uint8_t { Idle, Begin, Run, End };
  void fail(usb::diag::Output& out, const char* reason) noexcept;

  Phase phase_ = Phase::Idle;
  BandReader reader_;
  std::uint32_t lines_ = 0;
  update::Crc32 crc_;                // the whole image's
  std::uint32_t frames_at_start_ = 0;
};

}  // namespace o3sdk::platform::mk66::console
