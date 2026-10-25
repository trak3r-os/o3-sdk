// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// hal::Display on the FT812: display lists into RAM_DL, uploads into RAM_G, both moved by a BulkEngine (DMA on the MK66)
// from one aligned staging buffer, with the polled path as the fallback. Main loop only (hal/display.h); the DMA
// completion handler belongs to the engine and never calls into this class.
//
// Double buffering. The FT812 has a single RAM_DL of 8 KiB (2048 words) and swaps it in at a frame boundary: the host
// writes the whole new list there, then writes REG_DLSWAP = 2 (DLSWAP_FRAME, "recommended in most cases": a swap at the next
// frame start, no tearing); the register reads 0 once the swap has happened and the list memory is safe to write again. That
// is the chip's own double buffer, and it is the scheme used: the coprocessor (CMD_APPEND, CMD_DLSTART/CMD_SWAP) is not in the
// frame path at all (ADR 0010: a frame is a direct RAM_DL write plus a swap), so a coprocessor fault cannot freeze the UI.
//   * Word budget: a frame is at most kMaxFrameWords = 2048 words including the closing DISPLAY (RAM_DL's size);
//     eve::kMaxDisplayListWords is the same number (checked by a test). The pattern grid's worst case is about 800 words.
//   * Host side: write() copies the words into the staging buffer (the list's owner may reuse its buffer at once), end_frame()
//     starts one transfer [RAM_DL write header][words], and when it finishes service() writes REG_DLSWAP. begin_frame() says
//     false while a transfer is in flight or the previous swap has not completed, which the contract allows: the app skips
//     the frame and retries on its next pass. At the panel's 54 Hz a swap completes within 18.5 ms and a 3,200-byte frame is on
//     the wire for 1.4 ms at 18 MHz, so frames pace at the display's own refresh, never faster.
//
// Two staging buffers, one for the frame (4 + 8192 bytes) and one for an upload chunk (4 + 4096), both alignas(8), each with
// the 3-byte write header at offset 1 so that the payload starts on a word (the DMA source may start at any byte). They are
// separate because the contract allows an upload() inside an open frame. One SPI carries one transfer at a time: a frame
// committed behind an upload in flight is queued and starts when the wire is free; an upload() waits (bounded) for the
// transfer in flight and for a queued frame, so uploads and frames take effect in call order and the asset loader never
// sees Busy merely because its previous chunk is still on the wire.
//
// Backlight. It stays off until the first frame the app committed is on screen (REG_DLSWAP back to 0), or until a timeout
// if the app draws nothing, and then ramps up in steps, so no garbage frame is ever lit (task rule: backlight only after the
// first good frame).

#include <o3sdk/hal/display.h>
#include <o3sdk/platform/mk66/display/chip.h>
#include <o3sdk/platform/mk66/display/ft812_regs.h>
#include <o3sdk/platform/mk66/display/link.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::platform::mk66::display {

// How long a transfer may stay in flight before it is abandoned and sent again by the polled path (a full 8 KiB frame is
// 3.7 ms on the wire at 18 MHz, 33 ms at the init speed).
inline constexpr std::uint32_t kBulkTimeoutMs = 50;
// A frame's swap (REG_DLSWAP back to 0) takes at most one frame time, 18.5 ms at the panel's 54 Hz. If it has not happened after this
// long the scan-out is dead: the driver re-initialises the chip once and, if the swap stalls again (or the re-init fails), gives
// the display up: `failed()`, held dark, frames and uploads refused, for main to show as an LED code.
inline constexpr std::uint32_t kSwapTimeoutMs = 500;
// upload() waits this long for the transfer in front of it: longer than a hung transfer's timeout, so that the wait outlasts
// the abandon-and-resend and never ends in Busy because of the engine.
inline constexpr std::uint32_t kUploadWaitMs = 70;
static_assert(kUploadWaitMs > kBulkTimeoutMs, "a hung transfer is abandoned before the wait behind it gives up");
// Backlight: 8 duty steps every 8 ms (about 130 ms to the target); if the app has not shown a frame this long after the
// display came up, the ramp starts anyway (a dark screen would be mistaken for a hang).
inline constexpr std::uint8_t kBacklightStep = 8;
inline constexpr std::uint32_t kBacklightStepMs = 8;
inline constexpr std::uint32_t kBacklightFallbackMs = 2000;
// The run-mode brightness: REG_PWM_DUTY 127 of 128.
inline constexpr std::uint8_t kBacklightDefault = 127;
// The last 64 bytes of RAM_G: the DMA self-test's scratch, in the map's spare region (ui/plot/ram_g_map.h).
inline constexpr std::uint32_t kSelfTestAddr = ft812::kRamGBytes - 64u;
inline constexpr std::size_t kSelfTestBytes = 64;

struct DisplayStats {
  std::uint32_t frames = 0;           // committed
  std::uint32_t frames_aborted = 0;   // abort_frame(), an empty list, or a list that could not be terminated
  std::uint32_t begin_refused = 0;    // begin_frame() said false: transfer in flight or swap pending
  std::uint32_t swaps_done = 0;       // REG_DLSWAP seen back at 0
  std::uint32_t uploads = 0;
  std::uint32_t upload_bytes = 0;
  std::uint32_t upload_busy = 0;      // upload() answered Busy
  std::uint32_t dma_sends = 0;
  std::uint32_t polled_sends = 0;
  std::uint32_t bulk_timeouts = 0;
  std::uint32_t bulk_failures = 0;
  std::uint32_t swap_stalls = 0;      // a swap that did not complete within kSwapTimeoutMs
  std::uint32_t reinits = 0;          // chip re-initialisations after a stall
};

class Ft812Display final : public hal::Display {
 public:
  static constexpr std::size_t kMaxFrameWords = ft812::kRamDlBytes / 4u;
  static constexpr std::size_t kHeaderOffset = 1;   // the 3-byte write header sits at [1, 4): the payload starts word aligned
  static constexpr std::size_t kPayloadOffset = 4;
  static constexpr std::size_t kFrameStageBytes = kPayloadOffset + ft812::kRamDlBytes;
  static constexpr std::size_t kUploadStageBytes = kPayloadOffset + hal::kMaxUploadBytes;

  Ft812Display(Chip& chip, BulkEngine& bulk) noexcept;

  // The chip is up (Chip::init() returned Ok): frames and uploads are taken from here on. `use_dma` false sends everything by
  // the polled path (a bus whose DMA failed its self-test).
  // `config` is kept for the re-initialisation after a stalled swap.
  void start(bool use_dma, std::uint8_t backlight_target = kBacklightDefault, const InitConfig& config = InitConfig{}) noexcept;
  bool ready() const noexcept { return ready_; }
  bool using_dma() const noexcept { return !use_polled_; }
  // Writes a pattern to scratch RAM_G through the bulk engine and reads it back by the polled path. true: the DMA path moved
  // 64 bytes exactly. false: it did not (or timed out): the display has switched itself to the polled path for good.
  bool self_test() noexcept;

  // After a stall and a successful re-init the chip's RAM_G is presumed lost: the generation counts the re-inits, `needs_assets()`
  // is true (frames are refused, uploads are not) until the owner of the bitmaps has uploaded them again and says so.
  std::uint32_t reinit_count() const noexcept { return stats_.reinits; }
  bool needs_assets() const noexcept { return assets_stale_; }
  void assets_loaded() noexcept { assets_stale_ = false; }
  // The display is given up: the swap stalled again after a re-init, or the re-init failed. Held dark, everything refused.
  bool failed() const noexcept { return failed_; }
  // The safe state: backlight off and held off (it never ramps again), whatever the app or the loader found wrong.
  void hold_dark() noexcept;
  // The level the backlight rests at, 0..128 (clamped; OS-CFGWIRE: the device setting DISPLAY). A light that is on goes there in the ramp's steps, up or down; one that is still coming up
  // takes it as its target; a display held dark stays dark. Cheap and idempotent: the app may call it every pass.
  void set_backlight_target(std::uint8_t target) noexcept;

  // Every pass of the main loop: finishes a transfer (and issues the swap behind a frame), watches the swap, runs the
  // backlight ramp. Cheap when idle.
  void service() noexcept;

  // hal::Display
  bool begin_frame() noexcept override;
  bool write(const std::uint32_t* words, std::size_t count) noexcept override;
  void end_frame() noexcept override;
  void abort_frame() noexcept override;
  hal::UploadResult upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept override;

  const DisplayStats& stats() const noexcept { return stats_; }
  std::uint8_t backlight() const noexcept { return duty_; }
  bool backlight_on() const noexcept { return light_ == Light::On; }
  bool idle() const noexcept { return state_ == State::Idle && !frame_queued_ && !swap_pending_; }
  // OS-HIL (`diag shot`): no frame or upload transfer owns the SPI, so the console may use the chip for a few transactions of its own. Weaker than idle(): a swap that has not
  // completed yet (REG_DLSWAP is a register, not a transfer) does not matter to the bus. Main loop only: nothing starts a transfer between two calls of the console.
  bool bus_idle() const noexcept { return state_ == State::Idle && !frame_queued_; }

 private:
  enum class State : std::uint8_t { Idle, Sending };
  enum class Kind : std::uint8_t { None, Frame, Upload };
  enum class Light : std::uint8_t { Waiting, Ramping, On, Held };

  void begin_send(Kind kind, std::uint8_t* buffer, std::size_t len) noexcept;
  void send_polled() noexcept;
  void finish_send() noexcept;
  void pump_transfer() noexcept;
  void kick() noexcept;
  void fall_back() noexcept;
  void poll_swap() noexcept;
  void recover_stall() noexcept;
  void give_up() noexcept;
  void step_backlight() noexcept;
  bool wait_quiescent(std::uint32_t timeout_ms) noexcept;
  std::uint8_t* frame_payload() noexcept { return frame_ + kPayloadOffset; }

  Chip& chip_;
  BulkEngine& bulk_;
  // The staging buffers: DMA reads them, so their alignment is declared, not left to where the linker puts them (ADR 0011).
  alignas(8) std::uint8_t frame_[kFrameStageBytes];
  alignas(8) std::uint8_t upload_[kUploadStageBytes];

  DisplayStats stats_{};
  State state_ = State::Idle;
  Kind kind_ = Kind::None;
  std::uint8_t* src_ = nullptr;   // the buffer of the transfer in flight (frame_ or upload_), header at [kHeaderOffset]
  std::size_t len_ = 0;           // bytes of the transfer in flight, header included
  bool frame_queued_ = false;     // end_frame() committed a list the wire has not taken yet
  std::uint32_t started_ms_ = 0;  // when it started
  bool ready_ = false;
  bool use_polled_ = true;        // until start(true)
  bool open_ = false;             // between begin_frame() and end_frame() / abort_frame()
  std::size_t words_ = 0;         // words of the open frame
  bool swap_pending_ = false;     // REG_DLSWAP written, completion not yet seen
  std::uint32_t swap_started_ms_ = 0;
  unsigned stalls_in_a_row_ = 0;  // stalls since the last swap that completed
  bool failed_ = false;
  bool assets_stale_ = false;
  InitConfig config_{};
  std::uint32_t last_swap_poll_ms_ = 0;
  Light light_ = Light::Waiting;
  std::uint8_t duty_ = 0;
  std::uint8_t target_ = kBacklightDefault;
  std::uint32_t ready_ms_ = 0;
  std::uint32_t light_step_ms_ = 0;
  std::uint32_t first_frames_ = 0;  // frames committed when the last swap completed, to find "the first one"
};

static_assert(alignof(Ft812Display) >= 8, "the staging buffers are alignas(8) members, so the class is too");
// DMA bounded to its buffer: a whole frame (header and 2048 words) and a whole upload chunk (header and 4096 bytes) end exactly
// at the end of their buffer, counted from the header at offset 1: the engine's max_len() can never take a transfer past it.
static_assert(ft812::kWriteHeaderBytes + 4u * Ft812Display::kMaxFrameWords == Ft812Display::kFrameStageBytes - Ft812Display::kHeaderOffset,
              "a full frame ends at the end of the frame buffer");
static_assert(ft812::kWriteHeaderBytes + hal::kMaxUploadBytes == Ft812Display::kUploadStageBytes - Ft812Display::kHeaderOffset,
              "a full upload chunk ends at the end of the upload buffer");
static_assert(Ft812Display::kFrameStageBytes % 4 == 0 && Ft812Display::kUploadStageBytes % 4 == 0 &&
                  Ft812Display::kPayloadOffset % 4 == 0,
              "the payload starts on a word and the buffers are whole words");
static_assert(Ft812Display::kHeaderOffset + ft812::kWriteHeaderBytes == Ft812Display::kPayloadOffset,
              "header and payload are contiguous: one DMA source");
static_assert(Ft812Display::kUploadStageBytes >= Ft812Display::kPayloadOffset + hal::kMaxUploadBytes,
              "the upload buffer takes a whole upload chunk");
static_assert(Ft812Display::kMaxFrameWords == 2048, "RAM_DL is 2048 words");
static_assert(kSelfTestAddr % hal::kUploadAlign == 0 && kSelfTestAddr + kSelfTestBytes == hal::kRamGBytes, "scratch is the last 64 bytes");
static_assert(kBacklightDefault <= ft812::kPwmDutyMax, "duty 0..128");

}  // namespace o3sdk::platform::mk66::display
