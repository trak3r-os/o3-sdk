// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3sdk::hal {

// FT81x RAM_G, the graphics memory bitmaps are drawn from: 0x000000..0x0FFFFF, 1024 KB (FT81X Series
// Programmers Guide v1.2, "Memory Map" table, p.253; the vendor library's FT81x RAM_G_SIZE).
inline constexpr std::uint32_t kRamGBytes = 1u << 20;

// Alignment of both the RAM_G address and the source pointer of Display::upload(): a word. The device
// implementation may read the source with word or multi-word loads or hand it to DMA, and a misaligned LDM/LDRD
// is a UsageFault on the Cortex-M4, so callers stage uploads in an explicitly aligned buffer (alignas) and never
// rely on where the compiler happened to put a byte array.
inline constexpr std::size_t kUploadAlign = 4;

// The most bytes one Display::upload() call takes (ADR 0010: uploads of at most 4 KB per pass of the job runner).
// A device implementation stages the bytes in a buffer of this size, so a larger call is refused, not split;
// callers chunk their data (at word-aligned offsets, so every chunk keeps the alignment the contract asks for).
inline constexpr std::size_t kMaxUploadBytes = 4096;

// Why an upload was or was not taken. When several reasons apply the first of this list is reported: TooLarge,
// NoData, Misaligned, BadRange, Busy.
enum class UploadResult : std::uint8_t {
  Ok = 0,      // taken
  TooLarge,    // count > kMaxUploadBytes: the caller chunks
  NoData,      // a null source with count > 0
  Misaligned,  // addr or the source pointer is not a multiple of kUploadAlign
  BadRange,    // addr + count lies outside RAM_G
  Busy,        // the transport refuses for now: nothing was written, retry later
};

// Sink for FT81x display-list words.
//
// Frame contract: begin_frame(), then write() any number of times, then EXACTLY ONE of
//   end_frame()    commit: the words written become the visible list (FT812: swap RAM_DL), or
//   abort_frame()  discard: nothing changes on screen, the previously committed frame stays visible.
// A frame must be committed only if every write() succeeded and the list ends with DISPLAY (the chip
// would otherwise run off the end of an unterminated list); after a failed write() the caller aborts.
// eve::DisplayListBuilder follows this and appends DISPLAY itself.
//
// The words are 32-bit display-list commands (see eve/dl.h) in native byte order; the implementation
// serialises them (FT812: little-endian into RAM_DL; simulator: raster them). Main loop only.
//
// RAM_G is written with upload(), outside the frame protocol: bitmap data (faces, tiles, icons, waveform
// columns) goes there, and display lists point at it with BITMAP_SOURCE.
class Display {
 public:
  // Starts a new frame, abandoning any frame begun earlier and not yet committed (as if abort_frame()
  // had been called). false: the sink cannot take one right now (e.g. the previous swap is still
  // pending); the caller skips this frame and write()/end_frame()/abort_frame() are not called.
  virtual bool begin_frame() noexcept = 0;

  // Appends `count` words to the current frame, in order. false: they do not fit and nothing of this
  // call was kept; the frame is incomplete and the caller must abort_frame().
  virtual bool write(const std::uint32_t* words, std::size_t count) noexcept = 0;

  // Commits the frame. Without an open frame it does nothing.
  virtual void end_frame() noexcept = 0;

  // Discards the frame. Without an open frame it does nothing.
  virtual void abort_frame() noexcept = 0;

  // Copies `count` bytes from `bytes` into RAM_G at byte address `addr`. Contract:
  //  * Size: count <= kMaxUploadBytes, else TooLarge; the caller splits larger data into chunks.
  //  * Bounds: addr + count <= kRamGBytes. Nothing is written otherwise (BadRange; all or nothing).
  //  * Alignment: `addr` and `bytes` are multiples of kUploadAlign, else nothing is written (Misaligned).
  //    `count` may be any size up to the cap (a tail of 1..3 bytes is allowed); count 0 is a no-op that
  //    succeeds when the arguments are valid. `bytes` may be null only for count 0 (else NoData). BITMAP_SOURCE
  //    needs the address aligned to the bitmap format too (guide section 4.11, p.106); that is the caller's business.
  //  * Ordering: when upload() returns Ok the bytes are in RAM_G, or queued behind everything already queued,
  //    before any frame committed (end_frame()) after this call is executed. Uploads and frames therefore
  //    take effect in call order. An upload is not part of a frame: it may be made inside an open frame, and
  //    abort_frame() does not undo it.
  //  * Visibility: the chip re-reads RAM_G on every scan-out, so an upload overwriting a bitmap that the picture
  //    on screen still uses changes that picture immediately and may tear. To replace bitmap data in use, upload
  //    it to another region and commit a frame that points there. (The simulator keeps the finished picture, so
  //    it shows the change only from the next committed frame; see sim/README.md.)
  //  * Ownership: the bytes are consumed before Ok is returned, so the caller may reuse or free its buffer at
  //    once. (A device that sends them by DMA copies them into a staging buffer of kMaxUploadBytes first.)
  //  * Busy means the transport refused the data for now: nothing was written, the caller keeps its copy and
  //    may retry later.
  //  * Main loop only: never from an ISR or an audio path.
  virtual UploadResult upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept = 0;

 protected:
  Display() = default;
  ~Display() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;
};

}  // namespace o3sdk::hal
