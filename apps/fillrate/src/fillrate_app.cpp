// SPDX-License-Identifier: GPL-3.0-or-later
//
// FILLRATE: a test image for the Tracker, not an instrument. It measures how much the FT812 can draw on one scan line before lines
// break (band.h says what the band is; README.md is the procedure to run on the unit).
//
//   encoder       K, the number of stacked full-width layers (0..32); SHIFT: steps of 4
//   F1..F5        the kind of layer: L8 NEAREST, L8 BILINEAR (scaled 4/3), L4, RECT, MIX (bitmaps and rectangles)
//   F6            the cell width of a layer: 800, 400, 200, 100, 50 px (SHIFT: back); the pixels a line carries stay K x 800
//   F7            ballast: 0..1280 display-list words of invisible rectangles elsewhere (SHIFT: back)
//   F8            K = 0
//   PLAY          the reference grid on and off
//
// No unit, no sound: the engine runs empty.

#include <o3sdk/app/app.h>
#include <o3sdk/app/context.h>
#include <o3sdk/app/screen.h>

#include "band.h"

#include <cstddef>
#include <cstdint>

namespace {

using namespace o3sdk;
using namespace fillrate;
using app::Button;
using ui::Color;
using ui::Face;
using ui::Rect;

// ---- Text, without printf ---------------------------------------------------------------------------------------------------

class Line {
 public:
  Line& str(const char* s) noexcept {
    for (; *s != '\0' && n_ + 1u < sizeof buf_; ++s) {
      buf_[n_++] = *s;
    }
    buf_[n_] = '\0';
    return *this;
  }
  Line& num(unsigned v) noexcept {
    char digits[10];
    unsigned n = 0;
    do {
      digits[n++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u && n < sizeof digits);
    while (n != 0u && n_ + 1u < sizeof buf_) {
      buf_[n_++] = digits[--n];
    }
    buf_[n_] = '\0';
    return *this;
  }
  const char* c_str() const noexcept { return buf_; }

 private:
  char buf_[96] = {};
  unsigned n_ = 0;
};

const char* kind_label(Kind kind) noexcept {
  switch (kind) {
    case Kind::L8Nearest:
      return "L8 BITMAP NEAREST";
    case Kind::L8Bilinear:
      return kBilinearIsModelled ? "L8 BITMAP BILINEAR 0.75" : "L8 BILINEAR 0.75 (SIM: NEAREST)";
    case Kind::L4:
      return "L4 BITMAP NEAREST";
    case Kind::Rect:
      return "FILLED RECT";
    case Kind::Mix:
      return "MIX BITMAPS + RECTS";
  }
  return "";
}

// ---- Layout -----------------------------------------------------------------------------------------------------------------

constexpr int kBandTop = static_cast<int>(kBandY);
constexpr int kBandBottom = static_cast<int>(kBandY + kBandH);  // the first row under the band
constexpr int kRuleAbove = kBandTop - 4;                         // a full-width rule on a row of its own, above and below the band
constexpr int kRuleBelow = kBandBottom + 3;
constexpr int kGridStep = 40;                                    // vertical grid lines through the band: 1 px, every 40 px, and the last column
constexpr int kBallastTop = 384;                                 // the ballast rectangles: 1 px, background colour, rows 384 .. 443
constexpr int kBallastRows = 60;

constexpr unsigned kChunkCount = (kBitmapBytes + hal::kMaxUploadBytes - 1u) / hal::kMaxUploadBytes;
constexpr unsigned kChunksPerFrame = 2;  // 8 KB of the 115 KB a frame: the bitmaps are in place in about half a second

class FillrateScreen final : public app::Screen {
 public:
  const char* title() const noexcept override { return "FILL RATE"; }

  void input(app::Context& ctx, const app::Input& in) noexcept override {
    unsigned f = 0;
    if (in.turn()) {
      int k = static_cast<int>(k_) + in.delta() * (in.shift ? 4 : 1);
      k = k < 0 ? 0 : (k > static_cast<int>(kMaxLayers) ? static_cast<int>(kMaxLayers) : k);
      k_ = static_cast<unsigned>(k);
    } else if (in.fkey_press(f)) {
      if (f < kKindCount) {
        kind_ = static_cast<Kind>(f);
      } else if (f == 5) {
        cell_ = step(cell_, kCellWidthCount, in.shift);
      } else if (f == 6) {
        ballast_ = step(ballast_, kBallastCount, in.shift);
      } else {
        k_ = 0;
      }
    } else if (in.press(Button::Play)) {
      grid_ = !grid_;
    } else {
      return;
    }
    ctx.redraw();
  }

  void draw(app::Context&, ui::Renderer& r, const Rect&) noexcept override {
    upload_step(r.display());
    const unsigned cell_w = kCellWidths[cell_];
    Layers drawn;
    unsigned ballast_words = 0;
    if (chunk_ == kChunkCount) {
      // The band, clamped to what the list can still hold; then the ballast in what is left of that; then the references.
      const std::size_t head = r.words();
      const std::size_t reserve = kKeysRowWords + kTailWords;
      const unsigned avail = static_cast<unsigned>(head + reserve < kWordsLimit ? kWordsLimit - reserve - head : 0u);
      drawn = layers_for(kind_, fit_layers(kind_, k_, cell_w, avail));
      const unsigned used = band_words(drawn, cell_w);
      if (used != 0u) {
        r.begin_raw();
        RawSink sink{r};
        compose(sink, drawn, cell_w);
        r.end_raw();
      }
      const unsigned room = avail - used > 2u ? avail - used - 2u : 0u;  // minus the colour and BEGIN words the first rectangle brings
      ballast_words = (kBallastSteps[ballast_] < room ? kBallastSteps[ballast_] : room) & ~1u;
      draw_ballast(r, ballast_words);
      draw_references(r);
    }
    draw_text(r, drawn, cell_w, ballast_words);
    list_words_ = r.words();
  }

  void soft_keys(app::Context&, app::SoftKeys& out) noexcept override {
    for (unsigned i = 0; i < 8; ++i) {
      out.label[i] = kSoftKeyLabels[i];
    }
    out.hot = static_cast<std::uint8_t>(1u << static_cast<unsigned>(kind_));
  }

 private:
  struct RawSink {
    ui::Renderer& r;
    void operator()(std::uint32_t word) noexcept { r.raw(word); }
  };

  static unsigned step(unsigned v, unsigned count, bool back) noexcept { return back ? (v + count - 1u) % count : (v + 1u) % count; }

  // One chunk or two of the test bitmaps per frame, from an aligned staging buffer (hal/display.h). A Busy display is asked again at the
  // next frame; any other refusal is final and shown. Nothing is drawn from the bitmaps until the last chunk is in.
  void upload_step(hal::Display& display) noexcept {
    for (unsigned n = 0; n < kChunksPerFrame && chunk_ < kChunkCount && failed_ == hal::UploadResult::Ok; ++n) {
      const std::uint32_t offset = chunk_ * static_cast<std::uint32_t>(hal::kMaxUploadBytes);
      const std::size_t bytes = kBitmapBytes - offset < hal::kMaxUploadBytes ? kBitmapBytes - offset : hal::kMaxUploadBytes;
      generate(stage_, bytes, offset);
      const hal::UploadResult result = display.upload(ui::plot::kRamGApp + offset, stage_, bytes);
      if (result == hal::UploadResult::Busy) {
        return;
      }
      if (result != hal::UploadResult::Ok) {
        failed_ = result;
        return;
      }
      ++chunk_;
    }
  }

  // 1 px rectangles in the background colour, two words each, below the text: nothing to see, only list for the chip to walk.
  static void draw_ballast(ui::Renderer& r, unsigned words) noexcept {
    for (unsigned i = 0; i < words / 2u; ++i) {
      r.panel(Rect{static_cast<int>((i * 37u) % kBandW), kBallastTop + static_cast<int>(i % kBallastRows), 1, 1}, Color::Pane);
    }
  }

  // The references, over the layers: a rule above and below the band (rows that carry no layer, so always intact) and thin vertical
  // lines through it. A line the chip does not finish leaves gaps and jags in them.
  void draw_references(ui::Renderer& r) const noexcept {
    const int w = ui::kScreenW;
    r.rule(0, kRuleAbove, w, ui::Axis::Horizontal, Color::Text);
    r.rule(0, kRuleBelow, w, ui::Axis::Horizontal, Color::Text);
    if (grid_) {
      for (int x = 0; x < w; x += kGridStep) {
        r.rule(x, kBandTop, static_cast<int>(kBandH), ui::Axis::Vertical, Color::Text);
      }
      r.rule(w - 1, kBandTop, static_cast<int>(kBandH), ui::Axis::Vertical, Color::Text);
    }
  }

  void draw_text(ui::Renderer& r, const Layers& drawn, unsigned cell_w, unsigned ballast_words) const noexcept {
    const unsigned drawn_k = drawn.total();
    r.text(8, 38, kind_label(kind_), Color::Text, Face::Display);
    {
      Line l;
      l.str("K ").num(k_).str("   ").num(px_per_line(drawn)).str(" PX/LINE");
      r.text(8, 76, l.c_str(), Color::Accent, Face::Display);
    }
    {
      Line l;
      l.str("BMP ").num(bitmap_px(drawn)).str(" + FILL ").num(fill_px(drawn)).str(" PX   ").num(sprites_per_line(drawn, cell_w))
          .str(" SPRITES/LINE   CELL ").num(cell_w).str(" PX");
      r.text(8, 116, l.c_str(), Color::Text, Face::Data);
    }
    {
      Line l;
      // The list as of the last frame (this frame's text is still being drawn; constant once nothing changes): the words of the
      // screen, the runtime's soft-key row and the closing END and DISPLAY, as RAM_DL holds them.
      l.str("LIST ").num(static_cast<unsigned>(list_words_ + kKeysRowWords + kListClosingWords)).str("/").num(static_cast<unsigned>(eve::kMaxDisplayListWords))
          .str(" WORDS   BALLAST ").num(ballast_words).str(" WORDS   GRID ").str(grid_ ? "ON" : "OFF");
      r.text(8, 140, l.c_str(), Color::Text, Face::Data);
    }
    {
      Line l;
      Color c = Color::Rec;
      if (failed_ != hal::UploadResult::Ok) {
        l.str("BITMAP UPLOAD REFUSED, CODE ").num(static_cast<unsigned>(failed_));
      } else if (chunk_ < kChunkCount) {
        l.str("UPLOADING THE TEST BITMAPS ").num(chunk_).str("/").num(kChunkCount);
        c = Color::Accent;
      } else if (drawn_k < k_) {
        l.str("LIST FULL: DRAWING ").num(drawn_k).str(" OF ").num(k_).str(" LAYERS");
      } else if (ballast_words < kBallastSteps[ballast_]) {
        l.str("LIST FULL: BALLAST CUT TO ").num(ballast_words).str(" WORDS");
      }
      r.text(8, 164, l.c_str(), c, Face::Data);
    }
    r.text(8, 282, "F1 L8 NEAREST  F2 L8 BILINEAR  F3 L4  F4 RECT  F5 MIX (BITMAPS + RECTS)", Color::TextDim, Face::Data);
    r.text(8, 306, "F6 CELL WIDTH 800 400 200 100 50 PX  F7 BALLAST WORDS  (SHIFT: BACK)", Color::TextDim, Face::Data);
    r.text(8, 330, "F8 K = 0   PLAY GRID ON/OFF   ENCODER K  (SHIFT: STEPS OF 4)", Color::TextDim, Face::Data);
    r.text(8, 356, "BAND = ROWS 200-263, EVERY LAYER 800 PX WIDE", Color::TextDim, Face::Data);
  }

  Kind kind_ = Kind::L8Nearest;
  unsigned k_ = 0;
  unsigned cell_ = 0;     // index into kCellWidths
  unsigned ballast_ = 0;  // index into kBallastSteps
  bool grid_ = true;
  unsigned chunk_ = 0;    // chunks of the bitmaps uploaded so far
  hal::UploadResult failed_ = hal::UploadResult::Ok;
  std::size_t list_words_ = 0;  // the words this screen's last draw ended at (header and content, not the soft-key row)
  alignas(hal::kUploadAlign) std::uint8_t stage_[hal::kMaxUploadBytes] = {};
};
static_assert(alignof(FillrateScreen) >= hal::kUploadAlign, "the staging buffer is word aligned");

class FillrateApp final : public app::App {
 public:
  const app::AppInfo& info() const noexcept override {
    static constexpr app::AppInfo kInfo{"FILLRATE", "0.1.0"};
    return kInfo;
  }
  void setup(app::Context& ctx) noexcept override { ctx.show(screen_); }

 private:
  FillrateScreen screen_;
};

}  // namespace

o3sdk::app::App& o3sdk_app() noexcept {
  static FillrateApp app;
  return app;
}
