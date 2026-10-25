// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal::Display on the FT812 (display/display.h) against the chip model and a DMA engine fake (mk66_display_fakes.h): the
// frame and upload contracts of hal/display.h, the swap and its pacing, the staging buffers' alignment, the fall-back when
// the DMA path refuses, fails or hangs, the DMA self-test, the backlight rule. Runs on the host.
#include <doctest/doctest.h>

#include "mk66_display_fakes.h"

#include <o3sdk/hal/display.h>
#include <o3sdk/platform/mk66/display/chip.h>
#include <o3sdk/platform/mk66/display/display.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace display = o3sdk::platform::mk66::display;
namespace ft = o3sdk::platform::mk66::display::ft812;
using o3sdk::hal::UploadResult;
using o3sdk::platform::mk66::display::fake::FakeBulk;
using o3sdk::platform::mk66::display::fake::FakeChip;

namespace {

// A chip brought up by the real init, a DMA fake, and the display on them.
struct Rig {
  FakeChip chip;
  FakeBulk bulk{chip};
  display::Chip driver{chip};
  display::Ft812Display disp{driver, bulk};

  explicit Rig(bool dma = true, std::uint8_t backlight = 127) {
    const display::InitReport report = driver.init(display::InitConfig{});
    CHECK(report.status == display::InitStatus::Ok);
    disp.start(dma, backlight);
  }
  // Whatever a test did, the driver never touched the SPI bus while a DMA transfer owned it.
  ~Rig() { CHECK(chip.bus_violations() == 0u); }
  Rig(const Rig&) = delete;
  Rig& operator=(const Rig&) = delete;

  // Lets `ms` milliseconds of the chip's time pass, the main loop calling service() each one.
  void settle(std::uint32_t ms) {
    for (std::uint32_t i = 0; i < ms; ++i) {
      chip.advance_ms(1);
      disp.service();
    }
  }
};

struct alignas(8) Bytes {
  std::uint8_t b[4096 + 16];
};
Bytes make_bytes(std::uint8_t seed) {
  Bytes x{};
  for (std::size_t i = 0; i < sizeof x.b; ++i) {
    x.b[i] = static_cast<std::uint8_t>(seed + i * 7u);
  }
  return x;
}

std::uint32_t frame_word(unsigned i) { return 0x04000000u | i; }  // distinct, none of them DISPLAY (0)

// A frame of `words` words, closed by DISPLAY when `terminated`. false: the display refused at some step.
bool frame_of(display::Ft812Display& d, unsigned words, bool terminated) {
  if (!d.begin_frame()) {
    return false;
  }
  std::uint32_t buf[64];
  unsigned left = words;
  unsigned n = 0;
  while (left > 0) {
    const unsigned chunk = left < 64 ? left : 64;
    for (unsigned i = 0; i < chunk; ++i) {
      buf[i] = frame_word(n + i);
    }
    if (!d.write(buf, chunk)) {
      d.abort_frame();
      return false;
    }
    n += chunk;
    left -= chunk;
  }
  if (terminated) {
    const std::uint32_t end = ft::kDlDisplay;
    if (!d.write(&end, 1)) {
      d.abort_frame();
      return false;
    }
  }
  d.end_frame();
  return true;
}

}  // namespace

TEST_CASE("display driver: the staging buffers are word aligned by declaration, header and payload contiguous") {
  static_assert(alignof(display::Ft812Display) >= 8, "the staging buffers carry alignas(8)");
  Rig rig;
  CHECK(frame_of(rig.disp, 8, true));
  const std::uint8_t* src = rig.bulk.source();
  CHECK(src != nullptr);
  // The DMA source is the 3-byte header and the payload right behind it: the header at offset 1 of an 8-aligned buffer, so the
  // payload (the display-list words) starts on a word.
  CHECK(reinterpret_cast<std::uintptr_t>(src) % 8u == 1u);
  CHECK((reinterpret_cast<std::uintptr_t>(src) + ft::kWriteHeaderBytes) % 4u == 0u);
  rig.settle(40);
  const Bytes data = make_bytes(1);
  CHECK(rig.disp.upload(0x1000, data.b, 64) == UploadResult::Ok);
  const std::uint8_t* up = rig.bulk.source();
  CHECK(up != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(up) % 8u == 1u);
  CHECK((reinterpret_cast<std::uintptr_t>(up) + ft::kWriteHeaderBytes) % 4u == 0u);
}

TEST_CASE("display driver: upload() refuses in the contract's order and takes what it should") {
  Rig rig;
  const Bytes data = make_bytes(3);
  display::Ft812Display& d = rig.disp;
  // TooLarge before anything else, even with a null source and a bad address
  CHECK(d.upload(2, nullptr, o3sdk::hal::kMaxUploadBytes + 1) == UploadResult::TooLarge);
  // NoData before Misaligned and BadRange
  CHECK(d.upload(2, nullptr, 8) == UploadResult::NoData);
  // Misaligned before BadRange: address, then source
  CHECK(d.upload(2, data.b, 8) == UploadResult::Misaligned);
  CHECK(d.upload(0x1000, data.b + 1, 8) == UploadResult::Misaligned);
  CHECK(d.upload(o3sdk::hal::kRamGBytes + 4, data.b, 8) == UploadResult::BadRange);
  CHECK(d.upload(o3sdk::hal::kRamGBytes - 4, data.b, 8) == UploadResult::BadRange);
  CHECK(d.upload(o3sdk::hal::kRamGBytes - 4, data.b, 4) == UploadResult::Ok);   // the last word is in range
  CHECK(d.upload(o3sdk::hal::kRamGBytes, data.b, 0) == UploadResult::Ok);        // empty at the end: valid, a no-op
  CHECK(d.upload(0x2000, nullptr, 0) == UploadResult::Ok);                      // null only for an empty upload
  CHECK(d.upload(0x2000, data.b, 0) == UploadResult::Ok);
  // a tail of 1..3 bytes is allowed, the cap itself is allowed
  CHECK(d.upload(0x3000, data.b, 3) == UploadResult::Ok);
  CHECK(d.upload(0x4000, data.b, o3sdk::hal::kMaxUploadBytes) == UploadResult::Ok);
  rig.settle(20);
  CHECK(rig.chip.ram_g(0x3000) == data.b[0]);
  CHECK(rig.chip.ram_g(0x3002) == data.b[2]);
  CHECK(rig.chip.ram_g(0x3003) == 0);  // nothing past the count
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x4000], data.b, o3sdk::hal::kMaxUploadBytes) == 0);
  CHECK(rig.chip.ram_g(0x4000 + o3sdk::hal::kMaxUploadBytes) == 0);
  // refusals wrote nothing
  CHECK(rig.chip.ram_g(0x1000) == 0);
  CHECK(rig.chip.ram_g(2) == 0);
  CHECK(rig.chip.ram_g(0xFFFFC) == data.b[0]);
}

TEST_CASE("display driver: before start() nothing is taken: uploads are Busy, frames are refused") {
  FakeChip chip;
  FakeBulk bulk(chip);
  display::Chip driver(chip);
  display::Ft812Display disp(driver, bulk);
  const Bytes data = make_bytes(5);
  CHECK(disp.upload(0, data.b, 16) == UploadResult::Busy);
  CHECK_FALSE(disp.begin_frame());
  CHECK(disp.stats().upload_busy == 1u);
  CHECK(disp.stats().begin_refused == 1u);
  CHECK_FALSE(disp.ready());
  CHECK(bulk.starts() == 0u);
  disp.service();  // does nothing, touches nothing
  CHECK(chip.selects() == 0u);
}

TEST_CASE("display driver: an upload is on the wire by DMA, copied first: the caller's buffer is free at once") {
  Rig rig;
  Bytes data = make_bytes(9);
  Bytes copy = data;
  CHECK(rig.disp.upload(0x8000, data.b, 1000) == UploadResult::Ok);
  data = make_bytes(200);  // the caller reuses its buffer immediately
  CHECK(rig.bulk.busy());
  rig.settle(5);
  CHECK_FALSE(rig.bulk.busy());
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x8000], copy.b, 1000) == 0);
  CHECK(rig.disp.stats().uploads == 1u);
  CHECK(rig.disp.stats().upload_bytes == 1000u);
  CHECK(rig.disp.stats().dma_sends == 1u);
  CHECK(rig.disp.stats().polled_sends == 0u);
}

TEST_CASE("display driver: back-to-back uploads each land: the second waits for the first, never Busy") {
  Rig rig;
  const Bytes a = make_bytes(11);
  const Bytes b = make_bytes(22);
  const Bytes c = make_bytes(33);
  CHECK(rig.disp.upload(0x0000, a.b, 1920) == UploadResult::Ok);
  CHECK(rig.disp.upload(0x0800, b.b, 1920) == UploadResult::Ok);  // while the first is still in flight
  CHECK(rig.disp.upload(0x1000, c.b, 1920) == UploadResult::Ok);
  rig.settle(10);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x0000], a.b, 1920) == 0);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x0800], b.b, 1920) == 0);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x1000], c.b, 1920) == 0);
  CHECK(rig.disp.stats().upload_busy == 0u);
  CHECK(rig.bulk.completions() == 3u);
}

TEST_CASE("display driver: a frame is written to RAM_DL, then swapped in; begin_frame waits for both") {
  Rig rig;
  CHECK(rig.disp.begin_frame());
  const std::uint32_t words[] = {0x02112233u, 0x04AABBCCu, 0x26000007u, ft::kDlDisplay};
  CHECK(rig.disp.write(words, 4));
  CHECK(rig.chip.swaps() == 1u);  // only the init's black list so far
  rig.disp.end_frame();
  CHECK(rig.bulk.busy());                      // in flight on the DMA engine
  CHECK_FALSE(rig.disp.begin_frame());         // the frame buffer is on the wire
  CHECK(rig.chip.swaps() == 1u);               // not swapped before the data is in
  rig.settle(3);                               // the transfer completes, the swap is issued
  CHECK(rig.chip.swaps() == 2u);
  for (unsigned i = 0; i < 4; ++i) {
    CHECK(rig.chip.ram_dl_word(i) == words[i]);
  }
  CHECK_FALSE(rig.disp.begin_frame());         // the swap has not happened yet: the list memory is not safe to write
  rig.settle(30);                              // a frame start (18.5 ms) passes
  CHECK(rig.disp.stats().swaps_done == 1u);
  CHECK(rig.disp.begin_frame());               // now it is
  rig.disp.abort_frame();
  CHECK(rig.disp.stats().frames == 1u);
  CHECK(rig.disp.stats().frames_aborted == 1u);
  CHECK(rig.disp.stats().begin_refused >= 2u);
  // what the chip is scanning is the new list
  std::uint32_t first = 0;
  std::memcpy(&first, rig.chip.displayed().data(), 4);
  CHECK(first == 0x02112233u);
}

TEST_CASE("display driver: bus_idle (OS-HIL `diag shot`) is false while a transfer owns the SPI and true as soon as it ends, though the swap has not completed") {
  Rig rig;
  CHECK(rig.disp.bus_idle());
  CHECK(rig.disp.idle());
  CHECK(rig.disp.begin_frame());
  const std::uint32_t end = ft::kDlDisplay;
  CHECK(rig.disp.write(&end, 1));
  rig.disp.end_frame();
  CHECK_FALSE(rig.disp.bus_idle());  // the frame is on the wire
  CHECK_FALSE(rig.disp.idle());
  rig.settle(3);                     // the transfer completes, the swap is issued
  CHECK(rig.chip.swaps() == 2u);
  CHECK(rig.disp.bus_idle());        // no transfer owns the bus ...
  CHECK_FALSE(rig.disp.idle());      // ... though the swap has not completed
  rig.settle(30);
  CHECK(rig.disp.idle());
  CHECK(rig.disp.bus_idle());
}

TEST_CASE("display driver: frames pace at the display's refresh, never faster") {
  Rig rig;
  unsigned committed = 0;
  const std::uint64_t t0 = rig.chip.now_us();
  for (unsigned ms = 0; ms < 400; ++ms) {
    rig.chip.advance_ms(1);
    rig.disp.service();
    if (frame_of(rig.disp, 40, true)) {
      ++committed;
    }
  }
  const std::uint64_t spent_ms = (rig.chip.now_us() - t0) / 1000u;
  // one frame per swap: at most one per 18.5 ms, and at least one per 2 frame times
  CHECK(committed <= spent_ms / 18u + 1u);
  CHECK(committed >= spent_ms / 40u);
}

TEST_CASE("display driver: the word budget: 2048 words with DISPLAY, nothing more, and a list is always terminated") {
  static_assert(display::Ft812Display::kMaxFrameWords == 2048, "RAM_DL");
  {
    Rig rig;
    CHECK(frame_of(rig.disp, 2047, true));  // 2047 + DISPLAY = 2048
    rig.settle(10);
    CHECK(rig.chip.ram_dl_word(2046) == frame_word(2046));
    CHECK(rig.chip.ram_dl_word(2047) == ft::kDlDisplay);
    CHECK(rig.disp.stats().frames == 1u);
  }
  {
    Rig rig;
    CHECK_FALSE(frame_of(rig.disp, 2048, true));  // the 2049th word does not fit: refused, and the frame is abandoned
    rig.settle(10);
    CHECK(rig.disp.stats().frames == 0u);
    CHECK(rig.chip.swaps() == 1u);  // nothing was swapped in beyond the init's list
    CHECK(rig.disp.begin_frame());  // a refused write left the display usable
    rig.disp.abort_frame();
  }
  {
    Rig rig;  // a list without DISPLAY gets one if there is room
    CHECK(frame_of(rig.disp, 10, false));
    rig.settle(10);
    CHECK(rig.chip.ram_dl_word(9) == frame_word(9));
    CHECK(rig.chip.ram_dl_word(10) == ft::kDlDisplay);
  }
  {
    Rig rig;  // a full list without DISPLAY cannot be terminated: dropped, never sent
    CHECK(frame_of(rig.disp, 2048, false));
    rig.settle(10);
    CHECK(rig.disp.stats().frames == 0u);
    CHECK(rig.disp.stats().frames_aborted == 1u);
    CHECK(rig.chip.swaps() == 1u);
    CHECK(rig.chip.ram_dl_word(0) == ft::kDlClearColorBlack);  // RAM_DL still holds the init's list
  }
  {
    Rig rig;  // an empty frame is not a frame
    CHECK(rig.disp.begin_frame());
    rig.disp.end_frame();
    CHECK(rig.disp.stats().frames == 0u);
    CHECK(rig.disp.stats().frames_aborted == 1u);
  }
}

TEST_CASE("display driver: the frame contract: abort keeps the old picture, write needs a frame, begin abandons an open frame") {
  Rig rig;
  const std::uint32_t w[2] = {frame_word(1), ft::kDlDisplay};
  CHECK_FALSE(rig.disp.write(w, 2));  // no frame open
  rig.disp.end_frame();               // without a frame: nothing
  rig.disp.abort_frame();
  CHECK(rig.disp.stats().frames == 0u);
  CHECK(rig.disp.begin_frame());
  CHECK(rig.disp.write(w, 2));
  rig.disp.abort_frame();
  rig.settle(30);
  CHECK(rig.chip.swaps() == 1u);
  CHECK(rig.chip.ram_dl_word(0) == ft::kDlClearColorBlack);  // the abort changed nothing
  CHECK(rig.disp.begin_frame());
  CHECK(rig.disp.write(w, 1));
  CHECK(rig.disp.begin_frame());  // abandons the first one: starts empty again
  CHECK(rig.disp.write(w, 2));
  rig.disp.end_frame();
  rig.settle(5);
  CHECK(rig.chip.ram_dl_word(0) == frame_word(1));
  CHECK(rig.chip.ram_dl_word(1) == ft::kDlDisplay);
}

TEST_CASE("display driver: an upload inside an open frame does not touch the frame; a frame behind an upload is queued") {
  Rig rig;
  const Bytes data = make_bytes(44);
  CHECK(rig.disp.begin_frame());
  std::uint32_t words[100];
  for (unsigned i = 0; i < 100; ++i) {
    words[i] = frame_word(i);
  }
  CHECK(rig.disp.write(words, 100));
  CHECK(rig.disp.upload(0x2000, data.b, 512) == UploadResult::Ok);  // inside the frame: allowed
  CHECK(rig.disp.write(words, 100));                                 // the frame goes on being built
  rig.disp.end_frame();                                              // behind an upload still in flight
  CHECK(rig.disp.stats().frames == 1u);
  rig.settle(10);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x2000], data.b, 512) == 0);
  for (unsigned i = 0; i < 200; ++i) {
    CHECK(rig.chip.ram_dl_word(i) == frame_word(i % 100));
  }
  CHECK(rig.chip.ram_dl_word(200) == ft::kDlDisplay);
  CHECK(rig.chip.swaps() == 2u);
  // and an upload after a committed frame waits for the frame's transfer: call order is wire order
  rig.settle(40);
  CHECK(frame_of(rig.disp, 50, true));
  CHECK(rig.disp.upload(0x6000, data.b, 256) == UploadResult::Ok);
  CHECK(rig.bulk.completions() >= 3u);  // the frame had completed before the upload started
  rig.settle(10);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x6000], data.b, 256) == 0);
}

TEST_CASE("display driver: polled mode (no DMA): the same results by the byte-wise path") {
  Rig rig(/*dma=*/false);
  CHECK_FALSE(rig.disp.using_dma());
  const Bytes data = make_bytes(7);
  CHECK(rig.disp.upload(0x500, data.b, 300) == UploadResult::Ok);
  CHECK(std::memcmp(&rig.chip.ram_g_all()[0x500], data.b, 300) == 0);  // synchronous
  CHECK(frame_of(rig.disp, 20, true));
  CHECK(rig.chip.ram_dl_word(0) == frame_word(0));
  CHECK(rig.chip.swaps() == 2u);
  CHECK(rig.bulk.starts() == 0u);
  CHECK(rig.disp.stats().dma_sends == 0u);
  CHECK(rig.disp.stats().polled_sends == 2u);
}

TEST_CASE("display driver: an engine that refuses is bypassed for that transfer, one that fails or hangs for good") {
  const Bytes data = make_bytes(13);
  {
    Rig rig;
    rig.bulk.refuse = true;  // start() says no: this transfer goes by the polled path, the next may try DMA again
    CHECK(rig.disp.upload(0x100, data.b, 64) == UploadResult::Ok);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x100], data.b, 64) == 0);
    CHECK(rig.disp.stats().polled_sends == 1u);
    CHECK(rig.disp.using_dma());
    rig.bulk.refuse = false;
    CHECK(rig.disp.upload(0x200, data.b, 64) == UploadResult::Ok);
    CHECK(rig.disp.stats().dma_sends == 1u);
  }
  {
    Rig rig;
    rig.bulk.fail = true;  // the engine reports Failed: sent again, polled, and polled from then on
    CHECK(rig.disp.upload(0x100, data.b, 64) == UploadResult::Ok);
    rig.settle(5);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x100], data.b, 64) == 0);
    CHECK(rig.disp.stats().bulk_failures == 1u);
    CHECK_FALSE(rig.disp.using_dma());
    rig.bulk.fail = false;
    CHECK(frame_of(rig.disp, 10, true));  // a frame after the failure: polled, complete, swapped
    CHECK(rig.chip.ram_dl_word(0) == frame_word(0));
    CHECK(rig.chip.swaps() == 2u);
    CHECK(rig.bulk.starts() == 1u);
  }
  {
    Rig rig;
    rig.bulk.hang = true;  // never completes: abandoned after the timeout, data resent
    CHECK(rig.disp.upload(0x300, data.b, 64) == UploadResult::Ok);
    rig.settle(display::kBulkTimeoutMs + 5);
    CHECK(rig.bulk.aborts() == 1u);
    CHECK(rig.disp.stats().bulk_timeouts == 1u);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x300], data.b, 64) == 0);
    CHECK_FALSE(rig.disp.using_dma());
  }
  {
    Rig rig;  // a hang noticed by the next upload, which waits for it, instead of reporting Busy
    rig.bulk.hang = true;
    CHECK(rig.disp.upload(0x300, data.b, 64) == UploadResult::Ok);
    CHECK(rig.disp.upload(0x400, data.b, 64) == UploadResult::Ok);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x300], data.b, 64) == 0);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x400], data.b, 64) == 0);
    CHECK(rig.disp.stats().bulk_timeouts == 1u);
  }
}

TEST_CASE("display driver: a frame survives the engine failing under it") {
  Rig rig;
  rig.bulk.fail = true;
  CHECK(frame_of(rig.disp, 30, true));
  rig.settle(40);
  CHECK(rig.disp.stats().bulk_failures == 1u);
  CHECK(rig.chip.ram_dl_word(0) == frame_word(0));
  CHECK(rig.chip.ram_dl_word(29) == frame_word(29));
  CHECK(rig.chip.ram_dl_word(30) == ft::kDlDisplay);
  CHECK(rig.chip.swaps() == 2u);
  CHECK(rig.disp.stats().swaps_done == 1u);
}

TEST_CASE("display driver: the DMA self-test passes a good engine and switches a bad one off") {
  {
    Rig rig;
    CHECK(rig.disp.self_test());
    CHECK(rig.disp.using_dma());
    CHECK(rig.disp.stats().dma_sends == 1u);
    bool all = true;
    for (std::size_t i = 0; i < display::kSelfTestBytes; ++i) {
      if (rig.chip.ram_g(display::kSelfTestAddr + static_cast<std::uint32_t>(i)) == 0 && i != 0) {
        all = false;  // the pattern has exactly one zero byte, at index 0
      }
    }
    CHECK(all);
    CHECK(rig.chip.ram_g(display::kSelfTestAddr + 1) == 0xFF);
    CHECK(rig.chip.ram_g(display::kSelfTestAddr + 2) == 0x80);
  }
  {
    Rig rig;
    rig.bulk.corrupt = true;  // data arrives wrong: a PUSHR command half gone bad
    CHECK_FALSE(rig.disp.self_test());
    CHECK_FALSE(rig.disp.using_dma());
    const Bytes data = make_bytes(2);
    CHECK(rig.disp.upload(0x100, data.b, 32) == UploadResult::Ok);
    CHECK(std::memcmp(&rig.chip.ram_g_all()[0x100], data.b, 32) == 0);  // everything goes the slow way from now on
    CHECK(rig.bulk.starts() == 1u);
  }
  {
    Rig rig;
    rig.bulk.refuse = true;  // an engine that will not start proves nothing
    CHECK_FALSE(rig.disp.self_test());
    CHECK_FALSE(rig.disp.using_dma());
  }
  {
    Rig rig;
    rig.bulk.hang = true;
    CHECK_FALSE(rig.disp.self_test());
    CHECK_FALSE(rig.disp.using_dma());
    CHECK(rig.bulk.aborts() == 1u);
  }
  {
    Rig rig(/*dma=*/false);  // nothing to test without DMA
    CHECK_FALSE(rig.disp.self_test());
  }
}

TEST_CASE("display driver: the backlight stays off until the first frame is on screen, then ramps in steps") {
  Rig rig;
  const std::size_t base = rig.chip.writes().size();  // the init's own writes (backlight 0) are before this
  rig.settle(500);  // nothing drawn: still dark
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  CHECK(rig.disp.backlight() == 0);
  CHECK_FALSE(rig.disp.backlight_on());
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(2);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);  // committed and sent, but the swap has not happened yet
  rig.settle(30);                                // a frame start: the list is on screen
  CHECK(rig.disp.stats().swaps_done == 1u);
  rig.settle(300);
  CHECK(rig.disp.backlight_on());
  // every duty written since the start: strictly rising, in steps of at most kBacklightStep, ending exactly on the target
  std::uint32_t last = 0;
  unsigned steps = 0;
  const auto& w = rig.chip.writes();
  for (std::size_t i = base; i < w.size(); ++i) {
    if (w[i].first != ft::reg::kPwmDuty) {
      continue;
    }
    CHECK(w[i].second > last);
    CHECK(w[i].second - last <= display::kBacklightStep);
    CHECK(w[i].second <= 127u);
    last = w[i].second;
    ++steps;
  }
  CHECK(last == 127u);
  CHECK(steps == (127u + display::kBacklightStep - 1u) / display::kBacklightStep);
}

TEST_CASE("display driver: if the app draws nothing the backlight still comes up, after the timeout") {
  Rig rig;
  rig.settle(display::kBacklightFallbackMs - 100);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  rig.settle(400);
  CHECK(rig.disp.backlight_on());
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 127u);
}

TEST_CASE("display driver: set_backlight_target (the DISPLAY setting, OS-CFGWIRE) takes a light that is on to the new level in the ramp's steps, up or down; clamped, idempotent, held dark stays dark") {
  Rig rig;
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(500);
  CHECK(rig.disp.backlight_on());
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 127u);
  // Down to Medium (50): falling, steps of at most kBacklightStep, ending exactly on it.
  const std::size_t base = rig.chip.writes().size();
  rig.disp.set_backlight_target(50);
  rig.disp.set_backlight_target(50);  // again: nothing new
  rig.settle(300);
  CHECK(rig.disp.backlight_on());
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 50u);
  std::uint32_t last = 127;
  const auto& w = rig.chip.writes();
  for (std::size_t i = base; i < w.size(); ++i) {
    if (w[i].first != ft::reg::kPwmDuty) {
      continue;
    }
    CHECK(w[i].second < last);
    CHECK(last - w[i].second <= display::kBacklightStep);
    last = w[i].second;
  }
  CHECK(last == 50u);
  // Up again, past the maximum: clamped to 128.
  rig.disp.set_backlight_target(250);
  rig.settle(300);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == ft::kPwmDutyMax);
  CHECK(rig.disp.backlight() == ft::kPwmDutyMax);
  // To 0 (a light that is off but on): ends dark, the light is still "on" (nothing to wait for).
  rig.disp.set_backlight_target(0);
  rig.settle(400);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  CHECK(rig.disp.backlight_on());
  // Held dark (the safe state) stays dark whatever the setting says.
  rig.disp.hold_dark();
  rig.disp.set_backlight_target(100);
  rig.settle(400);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  CHECK_FALSE(rig.disp.backlight_on());
}

TEST_CASE("display driver: a target set before the first frame is the one the ramp rises to") {
  Rig rig;
  rig.disp.set_backlight_target(20);
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(500);
  CHECK(rig.disp.backlight_on());
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 20u);
}

TEST_CASE("display driver: backlight target 0 is on at once and writes nothing; a target above 128 is clamped") {
  {
    Rig rig(true, 0);
    CHECK(rig.disp.backlight_on());
    CHECK(frame_of(rig.disp, 5, true));
    rig.settle(60);
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  }
  {
    Rig rig(true, 250);
    rig.settle(display::kBacklightFallbackMs + 300);
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == ft::kPwmDutyMax);
  }
}

TEST_CASE("display driver: every transfer is bounded by its own buffer: the engine is told the room behind its source") {
  Rig rig;
  // a full frame: 3 + 2048 x 4 bytes, exactly the room behind the frame buffer's header
  CHECK(frame_of(rig.disp, 2047, true));
  CHECK(rig.bulk.last_len() == 8195u);
  CHECK(rig.bulk.last_capacity() == 8195u);
  rig.settle(40);
  // a full upload chunk: 3 + 4096 bytes, exactly the room behind the (smaller) upload buffer's header
  const Bytes data = make_bytes(21);
  CHECK(rig.disp.upload(0x10000, data.b, o3sdk::hal::kMaxUploadBytes) == UploadResult::Ok);
  CHECK(rig.bulk.last_len() == 4099u);
  CHECK(rig.bulk.last_capacity() == 4099u);
  rig.settle(10);
  // smaller ones leave room, never less than they need
  CHECK(rig.disp.upload(0x20000, data.b, 100) == UploadResult::Ok);
  CHECK(rig.bulk.last_len() == 103u);
  CHECK(rig.bulk.last_capacity() == 4099u);
  CHECK(rig.bulk.last_len() <= rig.bulk.last_capacity());
  rig.settle(10);
  // the engine refuses a length beyond its room (the second line of defence behind the static_asserts)
  std::uint8_t probe[16] = {};
  CHECK_FALSE(rig.bulk.start(probe, 16, 15));
  CHECK_FALSE(rig.bulk.busy());  // a refusal takes nothing
}

TEST_CASE("display driver: a swap that never completes: one re-init, then the display is given up and held dark") {
  Rig rig;
  rig.chip.swap_stuck = true;  // the scan-out is dead: REG_DLSWAP never reads 0 again, whatever is done
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(display::kSwapTimeoutMs - 50);
  CHECK_FALSE(rig.disp.failed());                   // not yet: the swap may still be on its way
  CHECK(rig.disp.stats().swap_stalls == 0u);
  rig.settle(400);                                  // past the timeout: re-init once (it fails: the scan-out is still dead)
  CHECK(rig.disp.stats().swap_stalls == 1u);
  CHECK(rig.disp.stats().reinits == 1u);
  CHECK(rig.disp.failed());
  CHECK_FALSE(rig.disp.begin_frame());              // refused for good
  const Bytes data = make_bytes(2);
  CHECK(rig.disp.upload(0x100, data.b, 16) == UploadResult::Busy);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);     // held dark: the backlight never comes up
  rig.settle(display::kBacklightFallbackMs + 500);
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  CHECK_FALSE(rig.disp.backlight_on());
  CHECK(rig.disp.stats().reinits == 1u);            // and it does not keep trying
  CHECK(rig.chip.bus_violations() == 0u);
}

TEST_CASE("display driver: a stall that a re-init cures: bitmaps are asked for again, the backlight starts over") {
  Rig rig;
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(60);
  CHECK(rig.disp.stats().swaps_done == 1u);
  rig.settle(300);
  CHECK(rig.disp.backlight_on());
  rig.chip.swap_stuck_until_reset = true;  // the next swap hangs, until the chip is reset
  rig.settle(40);
  CHECK(frame_of(rig.disp, 30, true));
  rig.settle(display::kSwapTimeoutMs + 100);
  CHECK(rig.disp.stats().swap_stalls == 1u);
  CHECK(rig.disp.stats().reinits == 1u);
  CHECK_FALSE(rig.disp.failed());
  CHECK(rig.disp.reinit_count() == 1u);
  CHECK(rig.chip.speed() == display::SpiSpeed::Data);  // the re-init ended at the data speed
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);         // dark again until a frame is on screen
  CHECK_FALSE(rig.disp.backlight_on());
  // its RAM_G is presumed gone: frames are refused until the bitmaps are up again, uploads are not
  CHECK(rig.disp.needs_assets());
  CHECK_FALSE(rig.disp.begin_frame());
  const Bytes data = make_bytes(7);
  CHECK(rig.disp.upload(0x400, data.b, 64) == UploadResult::Ok);
  rig.settle(5);
  CHECK_FALSE(rig.disp.begin_frame());
  rig.disp.assets_loaded();
  CHECK(frame_of(rig.disp, 25, true));
  rig.settle(40);
  CHECK(rig.disp.stats().swaps_done >= 2u);
  rig.settle(300);
  CHECK(rig.disp.backlight_on());  // back up, after a frame of the new generation was on screen
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 127u);
  CHECK(rig.chip.bus_violations() == 0u);
}

TEST_CASE("display driver: a second stall right after the re-init gives the display up without another re-init") {
  Rig rig;
  CHECK(frame_of(rig.disp, 20, true));
  rig.settle(60);
  rig.chip.swap_stuck_until_reset = true;
  CHECK(frame_of(rig.disp, 30, true));
  rig.settle(display::kSwapTimeoutMs + 100);
  CHECK(rig.disp.stats().reinits == 1u);
  CHECK_FALSE(rig.disp.failed());
  rig.disp.assets_loaded();
  rig.chip.swap_stuck = true;  // it stalls again
  CHECK(frame_of(rig.disp, 10, true));
  rig.settle(display::kSwapTimeoutMs + 100);
  CHECK(rig.disp.stats().swap_stalls == 2u);
  CHECK(rig.disp.stats().reinits == 1u);  // one re-init only
  CHECK(rig.disp.failed());
  CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
}

TEST_CASE("display driver: hold_dark is the safe state: the backlight is off now and never ramps again") {
  {
    Rig rig;
    CHECK(frame_of(rig.disp, 20, true));
    rig.settle(400);
    CHECK(rig.disp.backlight_on());
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 127u);
    rig.disp.hold_dark();
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
    CHECK_FALSE(rig.disp.backlight_on());
    rig.settle(display::kBacklightFallbackMs + 500);
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
  }
  {
    Rig rig;  // before any frame: the fallback timeout does not light it either
    rig.disp.hold_dark();
    rig.settle(display::kBacklightFallbackMs + 500);
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);
    CHECK(frame_of(rig.disp, 20, true));
    rig.settle(500);
    CHECK(rig.chip.reg(ft::reg::kPwmDuty) == 0u);  // frames still go to the chip; only the light is held
    CHECK(rig.disp.stats().swaps_done == 1u);
  }
}
