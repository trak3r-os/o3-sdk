// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3sdk/eve/dl.h>
#include <o3sdk/hal/display.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::eve {

// Word storage for one display list with its alignment stated, not left to where the compiler puts an array. The
// list is handed to the sink as words and a device sends it with word or multi-word loads (or DMA), and a
// misaligned LDM/LDRD is a UsageFault on the Cortex-M4; alignment 8 also keeps it valid for LDRD. Use it for
// every buffer that holds display-list words:
//   static eve::DisplayListBuffer buffer;  eve::DisplayListBuilder b(sink, buffer.words, eve::kMaxDisplayListWords);
struct alignas(8) DisplayListBuffer {
  std::uint32_t words[kMaxDisplayListWords];
};
static_assert(alignof(DisplayListBuffer) >= 8, "a display-list buffer is 8-byte aligned");
static_assert(sizeof(DisplayListBuffer) % 8 == 0, "no padding hides in an aligned display-list buffer");

// Collects one frame's display-list words in a caller-provided fixed buffer and hands them to a
// hal::Display in a single write. No heap; the buffer is typically
//   static eve::DisplayListBuffer buffer;   // words in buffer.words, explicitly aligned
// and may live in any RAM. Main loop only.
//
// Frame:   if (b.begin_frame()) { b.emit(dl::clear(true, true, true)); ...; b.end_frame(); }
// emit() takes words from the dl:: encoders. One slot is always reserved for the closing DISPLAY that
// end_frame() appends, so a list handed to the sink is always terminated. A word that does not fit (or is
// emitted outside a frame) is dropped and clears ok(); end_frame() then aborts the frame instead of committing
// a truncated list, so the previous frame stays visible, and returns false so the caller can count it. A list
// the sink cannot take is aborted the same way (see the contract in hal/display.h). A frame is therefore
// either complete on screen or not there at all.
class DisplayListBuilder {
 public:
  // `storage` must stay valid for the builder's lifetime and be word aligned (DisplayListBuffer); a null or
  // misaligned one, or a capacity below 2, never accepts a frame (begin_frame() returns false).
  DisplayListBuilder(hal::Display& sink, std::uint32_t* storage, std::size_t capacity_words) noexcept
      : sink_(sink), words_(storage), capacity_(capacity_words) {}

  DisplayListBuilder(const DisplayListBuilder&) = delete;
  DisplayListBuilder& operator=(const DisplayListBuilder&) = delete;

  // Starts a frame and asks the sink to begin one; a frame still open from before is abandoned.
  // false: the sink is busy or the buffer is too small; no frame is open and the caller skips drawing
  // (emit would be dropped).
  bool begin_frame() noexcept;

  // Appends one word to the open frame. false: dropped (frame full, or no frame open).
  bool emit(std::uint32_t word) noexcept;

  // Appends DISPLAY and writes the list to the sink in one call. If every emitted word was kept and the sink
  // accepts the whole list the frame is committed (end_frame) and true is returned. Otherwise the sink's frame
  // is aborted (abort_frame), the previous frame stays visible and false is returned. The builder's frame is
  // closed either way; a call without an open frame returns false and does nothing.
  bool end_frame() noexcept;

  // The display the frames go to, for what is not part of a frame: RAM_G uploads (hal/display.h).
  hal::Display& sink() noexcept { return sink_; }

  bool in_frame() const noexcept { return in_frame_; }
  bool ok() const noexcept { return ok_; }
  // Words in the open frame, DISPLAY not counted.
  std::size_t size() const noexcept { return count_; }

 private:
  hal::Display& sink_;
  std::uint32_t* words_;
  std::size_t capacity_;
  std::size_t count_ = 0;
  bool in_frame_ = false;
  bool ok_ = true;
};

}  // namespace o3sdk::eve
