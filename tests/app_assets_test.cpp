// SPDX-License-Identifier: GPL-3.0-or-later
// The app's bitmaps in RAM_G (kit/app app_assets.h, App::assets()): the runtime uploads the table in chunks after the kit's own bitmaps, never fails start() over it,
// and Context::app_assets_ok() says whether it landed. The display here is a wrapper around the host fake that logs every upload and can turn busy after the n-th.
#include <doctest/doctest.h>

#include <o3sdk/app/app.h>
#include <o3sdk/app/app_assets.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/hal/display.h>
#include <o3sdk/platform/host/fakes.h>
#include <o3sdk/ui/assets.h>
#include <o3sdk/ui/plot/ram_g_map.h>

#include "support/aborts.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace o3sdk;
using test::aborts;
using app::AppAsset;
using app::AppAssetsResult;
using ui::plot::kRamGApp;
using ui::plot::kRamGAppBytes;

namespace {

// Two rows' bytes: 10,000 bytes (three chunks) and 4 bytes (one), each on a word.
struct Pattern {
  alignas(4) std::uint8_t big[10000];
  alignas(4) std::uint8_t small[4];
  Pattern() {
    for (std::size_t i = 0; i < sizeof big; ++i) {
      big[i] = static_cast<std::uint8_t>(i * 7u + 3u);
    }
    for (std::size_t i = 0; i < sizeof small; ++i) {
      small[i] = static_cast<std::uint8_t>(0xA0u + i);
    }
  }
};
const Pattern& pattern() {
  static const Pattern p;
  return p;
}

constexpr std::uint32_t kSmallOffset = 12288;  // after the 10,000 bytes, on a page

// hal::Display around the host fake: logs each upload (address, bytes, the source's alignment), refuses with Busy from the n-th one on.
class LogDisplay final : public hal::Display {
 public:
  struct Up {
    std::uint32_t addr;
    std::vector<std::uint8_t> bytes;
    bool source_aligned;
  };
  explicit LogDisplay(platform::host::FakeDisplay& inner) : inner_(inner) {}
  bool begin_frame() noexcept override { return inner_.begin_frame(); }
  bool write(const std::uint32_t* words, std::size_t count) noexcept override { return inner_.write(words, count); }
  void end_frame() noexcept override { inner_.end_frame(); }
  void abort_frame() noexcept override { inner_.abort_frame(); }
  hal::UploadResult upload(std::uint32_t addr, const std::uint8_t* bytes, std::size_t count) noexcept override {
    log.push_back({addr, std::vector<std::uint8_t>(bytes, bytes + count), reinterpret_cast<std::uintptr_t>(bytes) % hal::kUploadAlign == 0});
    if (busy_from_ >= 0 && log.size() > static_cast<std::size_t>(busy_from_)) {
      return hal::UploadResult::Busy;
    }
    return inner_.upload(addr, bytes, count);
  }
  // The uploads after the first `n` are refused (n = 0: the first one already is). Negative: never.
  void busy_after(int n) { busy_from_ = n; }
  std::vector<Up> log;

 private:
  platform::host::FakeDisplay& inner_;
  int busy_from_ = -1;
};

class AssetApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"ASSETS", "0"};
    return kInfo;
  }
  void setup(app::Context&) noexcept override {}
  std::size_t assets(const AppAsset*& out) const noexcept override {
    ++asked;
    out = rows;
    return count;
  }
  const AppAsset* rows = nullptr;
  std::size_t count = 0;
  mutable unsigned asked = 0;
};

struct Rig {
  platform::host::FakeDisplay inner;
  LogDisplay display{inner};
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  dsp::Engine engine;
  AssetApp app;
  app::Platform platform{display, input, nullptr, clock, false};
  app::Runtime runtime{app, engine, platform};

  explicit Rig(const AppAsset* rows = nullptr, std::size_t count = 0, int busy_after = -1) {
    app.rows = rows;
    app.count = count;
    display.busy_after(busy_after);
  }
};

// How many uploads the kit's own bitmaps take: a start() without a table.
std::size_t kit_uploads() {
  Rig rig;
  REQUIRE(rig.runtime.start());
  return rig.display.log.size();
}

// ---- Tables checked at compile time (6) -----------------------------------------------------------------------------------------------
alignas(4) constexpr std::uint8_t kA[16] = {};
alignas(4) constexpr std::uint8_t kB[16] = {};

constexpr AppAsset kGood[] = {{0, kA, 16}, {16, kB, 16}};
static_assert(app::app_assets_valid(kGood, 2), "adjacent rows are disjoint");
static_assert(app::app_assets_valid(nullptr, 0), "an empty table is valid, whatever the pointer");
static_assert(app::app_assets_valid(kGood, 0), "so is a count of zero");

constexpr AppAsset kLast[] = {{static_cast<std::uint32_t>(kRamGAppBytes) - 16, kA, 16}};
static_assert(app::app_assets_valid(kLast, 1), "a row may end exactly at the end of the region");
constexpr AppAsset kPast[] = {{static_cast<std::uint32_t>(kRamGAppBytes) - 12, kA, 16}};
static_assert(!app::app_assets_valid(kPast, 1), "four bytes past the region");
constexpr AppAsset kHuge[] = {{4, kA, 0xFFFFFFFCu}};
static_assert(!app::app_assets_valid(kHuge, 1), "offset + size does not wrap around");
constexpr AppAsset kOddOffset[] = {{2, kA, 16}};
static_assert(!app::app_assets_valid(kOddOffset, 1), "an offset off the word grid");
constexpr AppAsset kOddSize[] = {{0, kA, 10}};
static_assert(!app::app_assets_valid(kOddSize, 1), "a size off the word grid");
constexpr AppAsset kEmptyRow[] = {{0, kA, 0}};
static_assert(!app::app_assets_valid(kEmptyRow, 1), "a row of size zero");
constexpr AppAsset kNoBytes[] = {{0, nullptr, 16}};
static_assert(!app::app_assets_valid(kNoBytes, 1), "a row without bytes");
constexpr AppAsset kOverlap[] = {{0, kA, 16}, {12, kB, 16}};
static_assert(!app::app_assets_valid(kOverlap, 2), "rows overlapping by a word");
constexpr AppAsset kNested[] = {{0, kA, 16}, {4, kB, 4}};
static_assert(!app::app_assets_valid(kNested, 2), "a row inside another");
constexpr AppAsset kNestedBackwards[] = {{4, kB, 4}, {0, kA, 16}};
static_assert(!app::app_assets_valid(kNestedBackwards, 2), "in either order");
static_assert(!app::app_assets_valid(nullptr, 1), "a count without a table");

}  // namespace

TEST_CASE("app assets: a 10,000 byte row and a 4 byte row arrive at their offsets, in 3 + 1 chunks, each aligned and at most 4,096 bytes") {
  const AppAsset table[] = {{0, pattern().big, sizeof pattern().big}, {kSmallOffset, pattern().small, sizeof pattern().small}};
  REQUIRE(app::app_assets_valid(table, 2));
  const std::size_t kit = kit_uploads();
  Rig rig(table, 2);
  CHECK(rig.runtime.start());
  CHECK(rig.app.asked == 1u);
  CHECK(rig.runtime.context().app_assets_ok());
  REQUIRE(rig.display.log.size() == kit + 4);

  const std::uint32_t want_addr[4] = {kRamGApp, kRamGApp + 4096, kRamGApp + 8192, kRamGApp + kSmallOffset};
  const std::size_t want_size[4] = {4096, 4096, 1808, 4};
  for (unsigned i = 0; i < 4; ++i) {
    CAPTURE(i);
    const LogDisplay::Up& up = rig.display.log[kit + i];
    CHECK(up.addr == want_addr[i]);
    CHECK(up.addr % hal::kUploadAlign == 0);
    CHECK(up.bytes.size() == want_size[i]);
    CHECK(up.bytes.size() <= hal::kMaxUploadBytes);
    CHECK(up.source_aligned);
  }
  // The bytes are the table's, in order.
  std::vector<std::uint8_t> big;
  for (unsigned i = 0; i < 3; ++i) {
    const auto& b = rig.display.log[kit + i].bytes;
    big.insert(big.end(), b.begin(), b.end());
  }
  REQUIRE(big.size() == sizeof pattern().big);
  CHECK(std::vector<std::uint8_t>(pattern().big, pattern().big + sizeof pattern().big) == big);
  CHECK(rig.display.log[kit + 3].bytes == std::vector<std::uint8_t>(pattern().small, pattern().small + 4));
  // Nothing the kit uploaded came after, or before, the app's own region.
  for (std::size_t i = 0; i < kit; ++i) {
    CHECK(rig.display.log[i].addr < kRamGApp);
  }
}

TEST_CASE("app assets: the display busy after the kit's uploads: one app chunk tried and no more, the flag false, start() true") {
  const AppAsset table[] = {{0, pattern().big, sizeof pattern().big}, {kSmallOffset, pattern().small, sizeof pattern().small}};
  const std::size_t kit = kit_uploads();
  Rig rig(table, 2, static_cast<int>(kit));
  CHECK(rig.runtime.start());  // only the kit's result
  REQUIRE(rig.display.log.size() == kit + 1);
  CHECK(rig.display.log.back().addr == kRamGApp);
  CHECK_FALSE(rig.runtime.context().app_assets_ok());
}

TEST_CASE("app assets: the kit's upload failing: no app chunk tried and start() false, as today") {
  const AppAsset table[] = {{0, pattern().big, sizeof pattern().big}};
  Rig rig(table, 1, 0);
  CHECK_FALSE(rig.runtime.start());
  CHECK(rig.display.log.size() == 1u);  // the kit stops at its first refused chunk
  CHECK(rig.display.log[0].addr < kRamGApp);
  CHECK(rig.app.asked == 0u);
  CHECK_FALSE(rig.runtime.context().app_assets_ok());
}

TEST_CASE("app assets: a table out of the region, misaligned, overlapping or of size zero is refused and nothing is written") {
  const std::uint32_t last = static_cast<std::uint32_t>(kRamGAppBytes);
  const AppAsset out_of_region[] = {{last - 4, pattern().big, 8}};
  const AppAsset odd_offset[] = {{2, pattern().big, 8}};
  const AppAsset odd_size[] = {{0, pattern().big, 6}};
  const AppAsset overlap[] = {{0, pattern().big, 8}, {4, pattern().small, 4}};
  const AppAsset empty_row[] = {{0, pattern().big, 0}};
  const AppAsset misaligned_bytes[] = {{0, pattern().big + 1, 8}};  // the source is one byte off a word
  const struct {
    const AppAsset* rows;
    std::size_t count;
  } bad[] = {{out_of_region, 1}, {odd_offset, 1}, {odd_size, 1}, {overlap, 2}, {empty_row, 1}, {misaligned_bytes, 1}, {nullptr, 3}};
  for (const auto& t : bad) {
    platform::host::FakeDisplay inner;
    LogDisplay display(inner);
    CHECK(app::upload_app_assets(display, t.rows, t.count) == AppAssetsResult::Refused);
    CHECK(display.log.empty());
    CHECK(inner.accepted_uploads() == 0u);
  }
  CHECK_FALSE(app::app_assets_aligned(misaligned_bytes, 1));
  CHECK(app::app_assets_aligned(overlap, 2));

  // Through the runtime a refused table is a programming error: a check aborts (host and debug builds), after nothing was written.
  CHECK(aborts([&] {
    Rig rig(odd_offset, 1);
    (void)rig.runtime.start();
  }));
  CHECK(aborts([&] {
    Rig rig(misaligned_bytes, 1);
    (void)rig.runtime.start();
  }));
}

TEST_CASE("app assets: a chunk the display does not take stops the upload at that chunk") {
  const AppAsset table[] = {{0, pattern().big, sizeof pattern().big}, {kSmallOffset, pattern().small, sizeof pattern().small}};
  platform::host::FakeDisplay inner;
  LogDisplay display(inner);
  display.busy_after(2);  // the third chunk is refused
  CHECK(app::upload_app_assets(display, table, 2) == AppAssetsResult::Failed);
  CHECK(display.log.size() == 3u);  // the 4-byte row was not tried
  CHECK(inner.accepted_uploads() == 2u);
  platform::host::FakeDisplay inner2;
  LogDisplay ok(inner2);
  CHECK(app::upload_app_assets(ok, table, 2) == AppAssetsResult::Ok);
  CHECK(app::upload_app_assets(ok, table, 0) == AppAssetsResult::None);
}

TEST_CASE("app assets: an app without assets uploads what it did before, asked once, and the flag is false") {
  platform::host::FakeDisplay plain;
  LogDisplay reference(plain);
  REQUIRE(ui::upload_assets(reference) == hal::UploadResult::Ok);  // the kit's upload alone: today's start()

  Rig rig;
  CHECK(rig.runtime.start());
  CHECK(rig.app.asked == 1u);
  CHECK_FALSE(rig.runtime.context().app_assets_ok());
  REQUIRE(rig.display.log.size() == reference.log.size());
  for (std::size_t i = 0; i < reference.log.size(); ++i) {
    CHECK(rig.display.log[i].addr == reference.log[i].addr);
    CHECK(rig.display.log[i].bytes == reference.log[i].bytes);
  }
  // The default of App::assets().
  struct Bare final : app::App {
    const app::AppInfo& info() const noexcept override {
      static constexpr app::AppInfo kInfo{"BARE", "0"};
      return kInfo;
    }
    void setup(app::Context&) noexcept override {}
  } bare;
  const AppAsset* rows = &kGood[0];
  CHECK(bare.assets(rows) == 0u);
  CHECK(rows == nullptr);
}
