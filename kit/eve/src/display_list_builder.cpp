// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/eve/display_list_builder.h>

#include <o3sdk/eve/dl.h>

#include <cstdint>

namespace o3sdk::eve {

bool DisplayListBuilder::begin_frame() noexcept {
  // An unfinished frame (a missing end_frame) is abandoned, never left to wedge the next one.
  in_frame_ = false;
  count_ = 0;
  ok_ = true;
  // A word buffer that is not word aligned (it can only come from a cast) would fault the device's word loads.
  if (capacity_ < 2 || words_ == nullptr || reinterpret_cast<std::uintptr_t>(words_) % alignof(std::uint32_t) != 0 ||
      !sink_.begin_frame()) {
    return false;
  }
  in_frame_ = true;
  return true;
}

bool DisplayListBuilder::emit(std::uint32_t word) noexcept {
  if (!in_frame_ || count_ + 1 >= capacity_) {  // keep one slot for DISPLAY
    ok_ = false;
    return false;
  }
  words_[count_] = word;
  ++count_;
  return true;
}

bool DisplayListBuilder::end_frame() noexcept {
  if (!in_frame_) {
    return false;
  }
  in_frame_ = false;
  if (!ok_) {
    // A word was dropped: this list is incomplete. Never commit it (hal/display.h): the previous frame stays.
    sink_.abort_frame();
    return false;
  }
  words_[count_] = dl::display();  // always fits: emit() stops at capacity_ - 1 words
  ++count_;
  if (!sink_.write(words_, count_)) {
    sink_.abort_frame();  // the sink cannot hold it: same rule
    return false;
  }
  sink_.end_frame();
  return true;
}

}  // namespace o3sdk::eve
