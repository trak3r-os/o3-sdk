// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/eve/display_list_builder.h>
#include <o3sdk/eve/dl.h>
#include <o3sdk/platform/host/fakes.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dl = o3sdk::eve::dl;
using o3sdk::eve::DisplayListBuilder;
using o3sdk::eve::Primitive;
using o3sdk::platform::host::FakeDisplay;

namespace {
using Words = std::vector<std::uint32_t>;
constexpr std::uint32_t kDisplayWord = 0x00000000u;  // DISPLAY, §4.29
}  // namespace

TEST_CASE("builder: a frame is its words followed by DISPLAY, delivered in one commit") {
  FakeDisplay sink;
  std::uint32_t storage[16];
  DisplayListBuilder b(sink, storage, 16);

  CHECK(b.begin_frame());
  CHECK(b.in_frame());
  CHECK(b.emit(dl::clear_color_rgb(0, 0, 255)));
  CHECK(b.emit(dl::clear(true, true, true)));
  CHECK(b.emit(dl::begin(Primitive::Rects)));
  CHECK(b.emit(dl::vertex2f(160, 480)));
  CHECK(b.emit(dl::end()));
  CHECK(b.size() == 5u);
  CHECK(sink.frames() == 0u);  // nothing is committed before end_frame()
  CHECK(b.end_frame());
  CHECK_FALSE(b.in_frame());

  CHECK(sink.frames() == 1u);
  CHECK(sink.aborted() == 0u);
  CHECK(sink.last_frame() == Words{0x020000FFu, 0x26000007u, 0x1F000009u, 0x405001E0u, 0x21000000u, kDisplayWord});
}

TEST_CASE("builder: an empty frame is just DISPLAY") {
  FakeDisplay sink;
  std::uint32_t storage[4];
  DisplayListBuilder b(sink, storage, 4);
  CHECK(b.begin_frame());
  CHECK(b.end_frame());
  CHECK(sink.last_frame() == Words{kDisplayWord});
}

TEST_CASE("builder: consecutive frames do not leak into each other") {
  FakeDisplay sink;
  std::uint32_t storage[8];
  DisplayListBuilder b(sink, storage, 8);
  CHECK(b.begin_frame());
  CHECK(b.emit(dl::color_a(1)));
  CHECK(b.end_frame());
  CHECK(b.begin_frame());
  CHECK(b.emit(dl::color_a(2)));
  CHECK(b.end_frame());
  CHECK(sink.frames() == 2u);
  CHECK(sink.last_frame() == Words{dl::color_a(2), kDisplayWord});
}

TEST_CASE("builder: a full buffer keeps capacity-1 words, drops the rest and never commits the truncated list") {
  FakeDisplay sink;
  std::uint32_t storage[4];  // 3 words + DISPLAY
  DisplayListBuilder b(sink, storage, 4);
  CHECK(b.begin_frame());
  CHECK(b.emit(dl::color_a(9)));  // a complete frame first, so there is something to stay on screen
  CHECK(b.end_frame());
  CHECK(sink.frames() == 1u);

  CHECK(b.begin_frame());
  CHECK(b.emit(1));
  CHECK(b.emit(2));
  CHECK(b.emit(3));
  CHECK(b.ok());
  CHECK_FALSE(b.emit(4));  // dropped
  CHECK_FALSE(b.ok());
  CHECK(b.size() == 3u);
  CHECK_FALSE(b.end_frame());  // reports the loss...
  CHECK_FALSE(b.in_frame());
  CHECK_FALSE(sink.in_frame());
  CHECK(sink.frames() == 1u);   // ...and aborts: nothing new was committed
  CHECK(sink.aborted() == 1u);
  CHECK(sink.last_frame() == Words{dl::color_a(9), kDisplayWord});  // the previous frame is still the visible one

  CHECK(b.begin_frame());  // the failure does not stick to the next frame
  CHECK(b.ok());
  CHECK(b.emit(5));
  CHECK(b.end_frame());
  CHECK(sink.last_frame() == Words{5, kDisplayWord});
}

TEST_CASE("builder: exactly capacity-1 words is not an overflow") {
  FakeDisplay sink;
  std::uint32_t storage[3];
  DisplayListBuilder b(sink, storage, 3);
  CHECK(b.begin_frame());
  CHECK(b.emit(7));
  CHECK(b.emit(8));
  CHECK(b.ok());
  CHECK(b.end_frame());
  CHECK(sink.last_frame() == Words{7, 8, kDisplayWord});
}

TEST_CASE("builder: a busy sink skips the frame and nothing is sent") {
  FakeDisplay sink;
  std::uint32_t storage[8];
  DisplayListBuilder b(sink, storage, 8);
  sink.set_busy(true);
  CHECK_FALSE(b.begin_frame());
  CHECK_FALSE(b.in_frame());
  CHECK_FALSE(b.emit(1));        // no frame open: dropped
  CHECK_FALSE(b.end_frame());    // nothing to end
  CHECK(sink.begun() == 0u);
  CHECK(sink.frames() == 0u);

  sink.set_busy(false);
  CHECK(b.begin_frame());
  CHECK(b.ok());  // the earlier failure does not stick
  CHECK(b.emit(5));
  CHECK(b.end_frame());
  CHECK(sink.last_frame() == Words{5, kDisplayWord});
}

TEST_CASE("builder: emit and end_frame outside a frame do nothing") {
  FakeDisplay sink;
  std::uint32_t storage[8];
  DisplayListBuilder b(sink, storage, 8);
  CHECK_FALSE(b.emit(1));
  CHECK_FALSE(b.end_frame());
  CHECK(sink.frames() == 0u);
  CHECK(sink.begun() == 0u);
}

TEST_CASE("builder: a buffer too small for DISPLAY never opens a frame") {
  FakeDisplay sink;
  std::uint32_t storage[1];
  DisplayListBuilder tiny(sink, storage, 1);
  CHECK_FALSE(tiny.begin_frame());
  DisplayListBuilder none(sink, nullptr, 0);
  CHECK_FALSE(none.begin_frame());
  CHECK(sink.begun() == 0u);
}

TEST_CASE("builder: begin_frame abandons a frame that was never ended") {
  FakeDisplay sink;
  std::uint32_t storage[8];
  DisplayListBuilder b(sink, storage, 8);
  CHECK(b.begin_frame());
  CHECK(b.emit(1));
  CHECK(b.emit(2));
  CHECK(b.begin_frame());  // the forgotten end_frame must not wedge the loop
  CHECK(b.size() == 0u);
  CHECK(b.emit(3));
  CHECK(b.end_frame());
  CHECK(sink.frames() == 1u);
  CHECK(sink.last_frame() == Words{3, kDisplayWord});
}

TEST_CASE("builder: a list the sink cannot hold is aborted, never committed, and the last frame stays") {
  FakeDisplay sink(3);  // RAM_DL of 3 words
  std::uint32_t storage[8];
  DisplayListBuilder b(sink, storage, 8);

  CHECK(b.begin_frame());  // a good frame first: 1 word + DISPLAY fits
  CHECK(b.emit(dl::color_a(7)));
  CHECK(b.end_frame());
  CHECK(sink.frames() == 1u);

  CHECK(b.begin_frame());
  CHECK(b.emit(1));
  CHECK(b.emit(2));
  CHECK(b.emit(3));  // 3 words + DISPLAY = 4 > 3
  CHECK(b.ok());
  CHECK_FALSE(b.end_frame());
  CHECK_FALSE(b.in_frame());
  CHECK_FALSE(sink.in_frame());  // the sink's frame is closed...
  CHECK(sink.frames() == 1u);    // ...without a commit
  CHECK(sink.aborted() == 1u);
  CHECK(sink.last_frame() == Words{dl::color_a(7), kDisplayWord});  // the previous frame is still shown
}

TEST_CASE("builder: a full-size frame fits the device's RAM_DL exactly") {
  FakeDisplay sink;  // 2048 words, like the FT81x
  static std::uint32_t storage[o3sdk::eve::kMaxDisplayListWords];
  DisplayListBuilder b(sink, storage, o3sdk::eve::kMaxDisplayListWords);
  CHECK(b.begin_frame());
  for (std::size_t i = 0; i + 1 < o3sdk::eve::kMaxDisplayListWords; ++i) {
    CHECK(b.emit(dl::color_a(static_cast<std::uint8_t>(i))));
  }
  CHECK(b.ok());
  CHECK(b.end_frame());  // 2047 words + DISPLAY: the whole RAM_DL, committed
  CHECK(sink.last_frame().size() == o3sdk::eve::kMaxDisplayListWords);
  CHECK(sink.last_frame().back() == kDisplayWord);

  CHECK(b.begin_frame());  // one word more is one too many: the frame is dropped, not truncated
  for (std::size_t i = 0; i + 1 < o3sdk::eve::kMaxDisplayListWords; ++i) {
    CHECK(b.emit(dl::color_a(static_cast<std::uint8_t>(i))));
  }
  CHECK_FALSE(b.emit(dl::end()));
  CHECK_FALSE(b.end_frame());
  CHECK(sink.aborted() == 1u);
  CHECK(sink.frames() == 1u);
  CHECK(sink.last_frame().size() == o3sdk::eve::kMaxDisplayListWords);
}
