// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/help_overlay.h>

#include <o3sdk/app/context.h>
#include <o3sdk/app/runtime.h>
#include <o3sdk/app/screen.h>
#include <o3sdk/ui/components.h>
#include <o3sdk/ui/plot/canvas.h>

#include <cstring>

namespace o3sdk::app {

void help_pads(Help& h, unsigned r0, unsigned c0, unsigned r1, unsigned c1, std::uint8_t group, const char* const* labels) noexcept {
  const unsigned width = c1 >= c0 ? c1 - c0 + 1u : 0u;
  for (unsigned r = r0; r <= r1 && r < kPadRows; ++r) {
    for (unsigned c = c0; c <= c1 && c < kPadCols; ++c) {
      const unsigned pad = pad_at(r, c);
      h.pad_group[pad] = group;
      h.pad_label[pad] = labels != nullptr ? labels[(r - r0) * width + (c - c0)] : nullptr;
    }
  }
}

namespace {

using ui::Color;
using ui::Face;
using ui::Rect;

constexpr std::uint8_t kNone = 0xFF;

// ---- The buttons: the printed order, in groups --------------------------------------------------------------------------------------------------------------------------------------------
struct Key {
  Button button;
  const char* name;
  std::uint8_t group;
};
constexpr unsigned kKeyCount = 25;
constexpr Key kKeys[kKeyCount] = {
    {Button::Params, "PARAMS", 0},          {Button::Performance, "PERFORMANCE", 0}, {Button::File, "FILE", 0},          {Button::Config, "CONFIG", 0},     {Button::Master, "MASTER", 0},
    {Button::SampleLoad, "SAMPLE LOAD", 1}, {Button::SamplePlay, "SAMPLE PLAY", 1},  {Button::SampleEdit, "SAMPLE EDIT", 1}, {Button::SampleRec, "SAMPLE REC", 1}, {Button::Song, "SONG", 1},
    {Button::Note, "NOTE", 2},              {Button::Instr, "INSTR", 2},             {Button::Vol, "VOL", 2},            {Button::Fx, "FX", 2},
    {Button::Pattern, "PATTERN", 3},
    {Button::Play, "PLAY", 4},              {Button::Rec, "REC", 4},                 {Button::Copy, "COPY", 4},          {Button::Insert, "INSERT", 4},     {Button::Delete, "DELETE", 4},      {Button::Shift, "SHIFT", 4},
    {Button::Up, "UP", 5},                  {Button::Down, "DOWN", 5},               {Button::Left, "LEFT", 5},          {Button::Right, "RIGHT", 5},
};
constexpr unsigned kConfigEntry = 3;
constexpr unsigned kShiftEntry = 20;
constexpr unsigned kEncoderEntry = 25;
constexpr std::uint8_t kEncoderGroup = 6;
static_assert(HelpOverlay::kEntries == kKeyCount + 1u, "the 25 buttons and the encoder");
static_assert(kKeys[kConfigEntry].button == Button::Config && kKeys[kShiftEntry].button == Button::Shift, "the entries the kit describes itself");

// The rows HelpOverlay::build_rows() makes of the entries (two to a row, a group begins on a new row), so rows_ is known to hold them.
constexpr unsigned list_rows() noexcept {
  unsigned rows = 0;
  unsigned col = 0;
  int last_group = -1;
  for (unsigned e = 0; e < HelpOverlay::kEntries; ++e) {
    const int group = e == kEncoderEntry ? kEncoderGroup : kKeys[e].group;
    if (group != last_group && col == 1u) {
      ++rows;
      col = 0;
    }
    if (++col == 2u) {
      ++rows;
      col = 0;
    }
    last_group = group;
  }
  return col == 1u ? rows + 1u : rows;
}
static_assert(list_rows() <= HelpOverlay::kMaxRows, "build_rows() stays inside rows_");

constexpr const char* kConfigText = "SYSTEM CONFIG";
constexpr const char* kConfigShift = "THIS HELP";
constexpr const char* kShiftText = "HOLD FOR THE SHIFT ROWS";
constexpr const char* kNoHelp = "NO HELP FOR THIS SCREEN";
constexpr const char* kNoPads = "NO PAD HELP ON THIS SCREEN";

// ---- Geometry (pixels; the content area is kContentArea: 800 x 426 under the header) -------------------------------------------------------------------------------------------------
constexpr int kSummaryY = 6;     // the summary's first line, below the top of the area; the pitch of its two lines
constexpr int kSummaryPitch = 18;
constexpr int kListTop = 46;
constexpr int kFCapH = 62;       // an F key cap: "Fn" and the label, two lines of text, the Shift label
constexpr int kFCapW = 92;
constexpr int kFSlot = 100;      // the soft keys' pitch: a cap sits over its key
constexpr int kFBottom = 4;
constexpr int kMoreH = 16;       // the line that says there is more, between the list and the F row
constexpr int kListH = kContentArea.h - kListTop - kFBottom - kFCapH - kMoreH;
constexpr int kCol0 = 8;
constexpr int kCol1 = 404;
constexpr int kCapW = 76;        // a button's cap: its name in the Micro face
constexpr int kCapH = 18;
constexpr int kTextX = 84;       // the texts, from the cap's left edge
constexpr int kRowGap = 2;
constexpr int kGroupGap = 4;     // extra, where a printed group begins
constexpr int kOneLineH = 18;    // a row with only a text; with a Shift line it is kTwoLineH
constexpr int kTwoLineH = 30;
constexpr int kCapRadius = 48;   // 3 px, in 1/16 px (Canvas)

constexpr int kPadW = 58;
constexpr int kPadH = 50;
constexpr int kPadPitchX = 64;
constexpr int kPadPitchY = 56;
constexpr int kPadsGridW = 11 * kPadPitchX + kPadW;
constexpr int kPadsGridH = 3 * kPadPitchY + kPadH;
constexpr int kLegendPitch = 18;
constexpr int kPadRadius = 80;   // 5 px

// ---- The word budget of the BUTTONS page ----------------------------------------------------------------------------------------------------------------------------------------------------
// What the runtime draws around the page: the header (about 60 words), the system strip when it is up (a Data line of at most 72 characters and its bar) and the soft keys (about 45). Held as a
// constant, not read off the frame, so that the list does not change its window as the header's number changes digits.
constexpr unsigned kChromeWords = 200;
// What the page itself spends beyond its glyphs: the caps (2 words each), the colour changes of the passes and the origin moves past x = 511, the "more" line.
constexpr unsigned kOverheadWords = 90;
constexpr unsigned kCellOverhead = 6;  // a cell's cap, its colour and its dim variant's second box

// ---- Text helpers -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool some(const char* s) noexcept { return s != nullptr && s[0] != '\0'; }

// The characters of `s` that make a word (a glyph that is not a space) among the first `max` and before a newline.
unsigned glyphs(const char* s, unsigned max) noexcept {
  unsigned n = 0;
  for (unsigned i = 0; s != nullptr && i < max && s[i] != '\0' && s[i] != '\n'; ++i) {
    n += s[i] != ' ' ? 1u : 0u;
  }
  return n;
}

// `s` cut at `max` characters (or at a newline) into `out`, which holds max + 1.
void cut(const char* s, unsigned max, char* out) noexcept {
  unsigned n = 0;
  while (s != nullptr && n < max && s[n] != '\0' && s[n] != '\n') {
    out[n] = s[n];
    ++n;
  }
  out[n] = '\0';
}

unsigned text_len(const char* s, unsigned max) noexcept {
  unsigned n = 0;
  while (s != nullptr && n < max && s[n] != '\0' && s[n] != '\n') {
    ++n;
  }
  return n;
}

unsigned wrapped_glyphs(const char* s, unsigned width, unsigned max_lines) noexcept {
  char buf[2 * (kHelpSummaryLine + 1)];
  const unsigned n = help_wrap(s, width, max_lines, buf, width + 1u);
  unsigned words = 0;
  for (unsigned i = 0; i < n; ++i) {
    words += glyphs(buf + i * (width + 1u), width);
  }
  return words;
}

// The text, cut, at (x, y).
void put(ui::Renderer& r, int x, int y, const char* s, unsigned max, Color c, Face face) noexcept {
  char buf[kHelpSummaryLine + 1];
  cut(s, max < kHelpSummaryLine ? max : kHelpSummaryLine, buf);
  r.text(x, y, buf, c, face);
}

// "SHIFT " and the text: a Shift line of a button.
void put_shift(ui::Renderer& r, int x, int y, const char* s, unsigned max, Color c) noexcept {
  char buf[8 + kHelpButtonText + 1] = "SHIFT ";
  cut(s, max, buf + 6);
  r.text(x, y, buf, c, Face::Micro);
}

// The ink that reads on a fill of this token (the pairs of ui::kContrastPairs).
Color ink_for(Color fill) noexcept {
  switch (fill) {
    case Color::Sel: return Color::SelText;
    case Color::Accent: return Color::AccentText;
    case Color::Play: return Color::PlayText;
    case Color::Rec: return Color::SelText;
    case Color::SelText: return Color::Sel;
    case Color::AccentText: return Color::Accent;
    case Color::PlayText: return Color::Play;
    case Color::Chrome: return Color::ChromeText;
    case Color::ChromeText:
    case Color::ChromeDim: return Color::Chrome;
    case Color::Pane:
    case Color::Empty: return Color::Text;
    case Color::Text:
    case Color::TextDim:
    case Color::Instr:
    case Color::Vol:
    case Color::Fx: return Color::Pane;
  }
  return Color::Pane;
}

int centred(int box_x, int box_w, unsigned chars, int cell_w) noexcept { return box_x + (box_w - static_cast<int>(chars) * cell_w) / 2; }

}  // namespace

// ---- State ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

HelpOverlay::Cell HelpOverlay::cell(unsigned entry) const noexcept {
  if (entry == kEncoderEntry) {
    return Cell{"ENCODER", some(help_.encoder) ? help_.encoder : nullptr, nullptr, true};
  }
  Cell c{kKeys[entry].name, nullptr, nullptr, false};
  const HelpButton* given = entry < kKeyCount ? given_[entry] : nullptr;
  if (given != nullptr) {
    c.text = some(given->text) ? given->text : nullptr;
    c.shift = some(given->shift) ? given->shift : nullptr;
  }
  if (entry == kConfigEntry) {  // the runtime's: the app never sees the button
    c.text = kConfigText;
    c.shift = kConfigShift;
  } else if (entry == kShiftEntry && c.text == nullptr) {
    c.text = kShiftText;
  }
  return c;
}

unsigned HelpOverlay::row_height(unsigned row) const noexcept {
  int h = kOneLineH;
  for (unsigned c = 0; c < 2u; ++c) {
    const unsigned e = rows_[row][c];
    if (e != kNone && some(cell(e).shift)) {
      h = kTwoLineH;
    }
  }
  return static_cast<unsigned>(h);
}

// The words a row of the list costs: the glyphs of its names and texts, the "SHIFT " of a Shift line, and each cell's boxes.
unsigned HelpOverlay::row_cost(unsigned row) const noexcept {
  unsigned words = 0;
  for (unsigned c = 0; c < 2u; ++c) {
    const unsigned e = rows_[row][c];
    if (e == kNone) {
      continue;
    }
    const Cell k = cell(e);
    words += glyphs(k.name, 12) + glyphs(k.text, kHelpButtonText) + kCellOverhead;
    if (some(k.shift)) {
      words += 5u + glyphs(k.shift, kHelpButtonText);
    }
  }
  return words;
}

bool HelpOverlay::group_start(unsigned row) const noexcept {
  if (row == 0u || row >= row_count_) {
    return false;
  }
  const auto group_of = [](unsigned e) { return e == kEncoderEntry ? kEncoderGroup : kKeys[e].group; };
  return group_of(rows_[row][0]) != group_of(rows_[row - 1u][0]);
}

// How many rows from `first` the list shows: while they fit the height and the word budget (the first row always shows).
unsigned HelpOverlay::window(unsigned first) const noexcept {
  int y = 0;
  unsigned words = 0;
  unsigned n = 0;
  for (unsigned row = first; row < row_count_; ++row) {
    const int gap = n != 0u && group_start(row) ? kGroupGap : 0;
    const int h = static_cast<int>(row_height(row));
    const unsigned cost = row_cost(row);
    if (n != 0u && (y + gap + h > kListH || words + cost > list_budget_)) {
      break;
    }
    y += gap + h + kRowGap;
    words += cost;
    ++n;
  }
  return n;
}

// The 25 buttons in the printed order and the encoder, two to a row; a group begins on a new row.
void HelpOverlay::build_rows() noexcept {
  unsigned rows = 0;
  unsigned col = 0;
  int last_group = -1;
  for (unsigned e = 0; e < kEntries; ++e) {
    const int group = e == kEncoderEntry ? kEncoderGroup : kKeys[e].group;
    if (group != last_group && col == 1u) {
      ++rows;
      col = 0;
    }
    if (col == 0u) {
      rows_[rows][0] = kNone;
      rows_[rows][1] = kNone;
    }
    rows_[rows][col] = static_cast<std::uint8_t>(e);
    ++col;
    if (col == 2u) {
      ++rows;
      col = 0;
    }
    last_group = group;
  }
  row_count_ = static_cast<std::uint8_t>(col == 1u ? rows + 1u : rows);
}

unsigned HelpOverlay::summary_words() const noexcept {
  return some(help_.summary) ? wrapped_glyphs(help_.summary, kHelpSummaryLine, 2) : (no_help_ ? glyphs(kNoHelp, 99) : 0u);
}

unsigned HelpOverlay::fkeys_words() const noexcept {
  unsigned words = 0;
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    words += 2u + glyphs(label_[i], 9) + wrapped_glyphs(help_.fkey[i], kHelpFKeyLine, 2) + 2u;
    if (some(help_.fkey_shift[i])) {
      words += 5u + glyphs(help_.fkey_shift[i], kHelpFKeyShift);
    }
  }
  return words;
}

void HelpOverlay::show(const Screen& screen, const Context& ctx, const SoftKeys& keys) noexcept {
  help_ = Help{};
  screen.help(ctx, help_);
  if (help_.buttons == nullptr) {
    help_.button_count = 0;
  }
  if (help_.groups == nullptr) {
    help_.group_count = 0;
  }
  if (help_.group_count > kHelpMaxGroups) {
    help_.group_count = kHelpMaxGroups;
  }
  for (unsigned i = 0; i < kEntries; ++i) {
    given_[i] = nullptr;
  }
  bool any = some(help_.summary) || some(help_.encoder) || some(help_.pads) || help_.group_count != 0u;
  for (unsigned b = 0; b < help_.button_count; ++b) {
    for (unsigned k = 0; k < kKeyCount; ++k) {
      if (kKeys[k].button == help_.buttons[b].button && given_[k] == nullptr) {  // (the first entry of a button counts)
        given_[k] = &help_.buttons[b];
        any = any || some(help_.buttons[b].text) || some(help_.buttons[b].shift);
      }
    }
  }
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    any = any || some(help_.fkey[i]) || some(help_.fkey_shift[i]);
    cut(keys.label[i], 12, label_[i]);
  }
  no_help_ = !any;
  label_dim_ = keys.dim;
  build_rows();
  const unsigned fixed = kChromeWords + summary_words() + fkeys_words() + kOverheadWords;
  list_budget_ = static_cast<std::uint16_t>(fixed < kWordBudget ? kWordBudget - fixed : 0u);
  max_first_ = 0;
  for (unsigned f = 0; f < row_count_; ++f) {
    if (f + window(f) >= row_count_) {
      max_first_ = static_cast<std::uint8_t>(f);
      break;
    }
  }
  first_row_ = 0;
  page_ = Page::Buttons;
  open_ = true;
}

void HelpOverlay::scroll_by(int rows) noexcept {
  int to = static_cast<int>(first_row_) + rows;
  to = to < 0 ? 0 : (to > static_cast<int>(max_first_) ? static_cast<int>(max_first_) : to);
  first_row_ = static_cast<std::uint8_t>(to);
}

void HelpOverlay::input(const Input& in) noexcept {
  if (!open_) {
    return;
  }
  if (in.turn()) {
    if (page_ == Page::Buttons) {
      const int d = in.delta();
      scroll_by(d < -static_cast<int>(kMaxRows) ? -static_cast<int>(kMaxRows) : (d > static_cast<int>(kMaxRows) ? static_cast<int>(kMaxRows) : d));
    }
    return;
  }
  if (in.type != Input::Type::Press || !in.button()) {
    return;
  }
  switch (in.which()) {
    case Button::F1:
    case Button::Left: page_ = Page::Buttons; break;
    case Button::F2:
    case Button::Right: page_ = Page::Pads; break;
    case Button::F8: open_ = false; break;
    case Button::Up:
      if (page_ == Page::Buttons) {
        scroll_by(-1);
      }
      break;
    case Button::Down:
      if (page_ == Page::Buttons) {
        scroll_by(1);
      }
      break;
    default: break;
  }
}

void HelpOverlay::soft_keys(SoftKeys& keys) const noexcept {
  keys.label[0] = "BUTTONS";
  keys.label[1] = "PADS";
  keys.label[7] = "CLOSE";
  keys.hot = static_cast<std::uint8_t>(1u << static_cast<unsigned>(page_));
}

// ---- Drawing --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void HelpOverlay::draw(ui::Renderer& r, const Rect& area) const noexcept {
  if (page_ == Page::Buttons) {
    draw_buttons(r, area);
  } else {
    draw_pads(r, area);
  }
}

namespace {

// A cell of the list as it is placed: its entry and where.
struct Placed {
  int x;
  int y;
  unsigned entry;
};

void disc(ui::plot::Canvas& cv, int cx, int cy, int radius, const ui::Rgb& rgb) noexcept { cv.point(cx * 16, cy * 16, static_cast<unsigned>(radius) * 16u, rgb); }

}  // namespace

void HelpOverlay::draw_buttons(ui::Renderer& r, const Rect& area) const noexcept {
  const ui::Theme& t = r.theme();
  const unsigned shown = window(first_row_);

  // Where everything goes.
  Placed placed[2 * kMaxRows];
  unsigned np = 0;
  const int list_top = area.y + kListTop;
  int y = list_top;
  for (unsigned k = 0; k < shown; ++k) {
    const unsigned row = first_row_ + k;
    y += k != 0u && group_start(row) ? kGroupGap : 0;
    for (unsigned c = 0; c < 2u; ++c) {
      const unsigned e = rows_[row][c];
      if (e != kNone) {
        placed[np++] = Placed{c == 0u ? kCol0 : kCol1, y, e};
      }
    }
    y += static_cast<int>(row_height(row)) + kRowGap;
  }
  const int fy = area.y + area.h - kFBottom - kFCapH;
  const auto dim = [&](const Placed& p) {
    const Cell c = cell(p.entry);
    return !some(c.text) && !some(c.shift);
  };

  // The boxes, in three colours: the caps, then the outlines of the dim ones and the encoder's centre, then the dim ones' insides.
  ui::plot::Canvas cv(r);
  cv.open();
  const ui::Rgb& chrome = t.rgb(Color::Chrome);
  const ui::Rgb& chrome_dim = t.rgb(Color::ChromeDim);
  const ui::Rgb& pane = t.rgb(Color::Pane);
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    cv.round_rect(Rect{static_cast<int>(i) * kFSlot + (kFSlot - kFCapW) / 2, fy, kFCapW, kFCapH}, kCapRadius, chrome);
  }
  for (unsigned i = 0; i < np; ++i) {
    if (dim(placed[i])) {
      continue;
    }
    if (placed[i].entry == kEncoderEntry) {
      disc(cv, placed[i].x + 10, placed[i].y + 9, 9, chrome);
    } else {
      cv.round_rect(Rect{placed[i].x, placed[i].y, kCapW, kCapH}, kCapRadius, chrome);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    const bool encoder = placed[i].entry == kEncoderEntry;
    if (dim(placed[i])) {
      if (encoder) {
        disc(cv, placed[i].x + 10, placed[i].y + 9, 9, chrome_dim);
      } else {
        cv.round_rect(Rect{placed[i].x, placed[i].y, kCapW, kCapH}, kCapRadius, chrome_dim);
      }
    } else if (encoder) {
      disc(cv, placed[i].x + 10, placed[i].y + 9, 4, chrome_dim);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    if (!dim(placed[i])) {
      continue;
    }
    if (placed[i].entry == kEncoderEntry) {
      disc(cv, placed[i].x + 10, placed[i].y + 9, 8, pane);
    } else {
      cv.round_rect(Rect{placed[i].x + 1, placed[i].y + 1, kCapW - 2, kCapH - 2}, kCapRadius - 16, pane);
    }
  }
  cv.close();

  // The text, a colour at a time. On a chrome cap: the label, the text and the cap names in ChromeText; the F numbers, a dim label and the Shift label in ChromeDim.
  const int num_y = ui::caps_y(t, Face::Micro, fy + 2, kCapH);
  const int label_y = ui::caps_y(t, Face::Label, fy + 2, kCapH);
  char lines[2 * (kHelpFKeyLine + 1)];
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    const int x = static_cast<int>(i) * kFSlot + (kFSlot - kFCapW) / 2;
    if (((label_dim_ >> i) & 1u) == 0u) {
      put(r, x + 22, label_y, label_[i], 9, Color::ChromeText, Face::Label);
    }
    const unsigned n = help_wrap(help_.fkey[i], kHelpFKeyLine, 2, lines, kHelpFKeyLine + 1u);
    for (unsigned k = 0; k < n; ++k) {
      r.text(x + 5, fy + 22 + static_cast<int>(k) * 12, lines + k * (kHelpFKeyLine + 1u), Color::ChromeText, Face::Micro);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    if (!dim(placed[i]) && placed[i].entry != kEncoderEntry) {
      const char* name = cell(placed[i].entry).name;
      r.text(centred(placed[i].x, kCapW, text_len(name, 12), 6), ui::caps_y(t, Face::Micro, placed[i].y, kCapH), name, Color::ChromeText, Face::Micro);
    }
  }
  for (unsigned i = 0; i < kFKeyCount; ++i) {
    const int x = static_cast<int>(i) * kFSlot + (kFSlot - kFCapW) / 2;
    char num[3] = {'F', static_cast<char>('1' + i), '\0'};
    r.text(x + 5, num_y, num, Color::ChromeDim, Face::Micro);
    if (((label_dim_ >> i) & 1u) != 0u) {
      put(r, x + 22, label_y, label_[i], 9, Color::ChromeDim, Face::Label);
    }
    if (some(help_.fkey_shift[i])) {
      put_shift(r, x + 5, fy + 47, help_.fkey_shift[i], kHelpFKeyShift, Color::ChromeDim);
    }
  }

  // On the pane: the summary and the texts (Text), the Shift lines (Instr), the dim names and the line that says there is more (TextDim).
  if (some(help_.summary) || no_help_) {
    char sum[2 * (kHelpSummaryLine + 1)];
    const unsigned n = some(help_.summary) ? help_wrap(help_.summary, kHelpSummaryLine, 2, sum, kHelpSummaryLine + 1u) : help_wrap(kNoHelp, kHelpSummaryLine, 2, sum, kHelpSummaryLine + 1u);
    for (unsigned k = 0; k < n; ++k) {
      r.text(16, ui::caps_y(t, Face::Label, area.y + kSummaryY + static_cast<int>(k) * kSummaryPitch, kSummaryPitch), sum + k * (kHelpSummaryLine + 1u), Color::Text, Face::Label);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    const Cell c = cell(placed[i].entry);
    if (c.encoder && !dim(placed[i])) {
      r.text(placed[i].x + 22, ui::caps_y(t, Face::Micro, placed[i].y, kCapH), c.name, Color::Text, Face::Micro);
    }
    if (some(c.text)) {
      put(r, placed[i].x + kTextX, ui::caps_y(t, Face::Label, placed[i].y, kCapH), c.text, kHelpButtonText, Color::Text, Face::Label);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    const Cell c = cell(placed[i].entry);
    if (some(c.shift)) {
      put_shift(r, placed[i].x + kTextX, placed[i].y + kCapH, c.shift, kHelpButtonText, Color::Instr);
    }
  }
  for (unsigned i = 0; i < np; ++i) {
    if (dim(placed[i])) {
      const Cell c = cell(placed[i].entry);
      const int x = c.encoder ? placed[i].x + 22 : centred(placed[i].x, kCapW, text_len(c.name, 12), 6);
      r.text(x, ui::caps_y(t, Face::Micro, placed[i].y, kCapH), c.name, Color::TextDim, Face::Micro);
    }
  }
  const bool above = first_row_ > 0u;
  const bool below = first_row_ + shown < row_count_;
  if (above || below) {
    const char* more = above && below ? "UP / DOWN FOR MORE" : (above ? "UP FOR MORE" : "DOWN FOR MORE");
    r.text(ui::kScreenW - 8 - r.text_width(more, Face::Micro), list_top + kListH + 2, more, Color::TextDim, Face::Micro);
  }
}

void HelpOverlay::draw_pads(ui::Renderer& r, const Rect& area) const noexcept {
  const ui::Theme& t = r.theme();
  const int x0 = area.x + (area.w - kPadsGridW) / 2;
  const int y0 = area.y + 8;
  const unsigned groups = help_.group_count;
  const auto group_of = [&](unsigned pad) { return help_.pad_group[pad] <= groups ? help_.pad_group[pad] : std::uint8_t{0}; };
  const auto pad_rect = [&](unsigned pad) { return Rect{x0 + static_cast<int>(pad_col(pad)) * kPadPitchX, y0 + static_cast<int>(pad_row(pad)) * kPadPitchY, kPadW, kPadH}; };
  const int legend_y = y0 + kPadsGridH + 10;

  // The boxes: the outlines of the pads in no group, then each group's pads and its legend swatch in its colour, then the dark insides.
  ui::plot::Canvas cv(r);
  cv.open();
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    if (group_of(pad) == 0u) {
      cv.round_rect(pad_rect(pad), kPadRadius, t.rgb(Color::Empty));
    }
  }
  for (unsigned g = 1; g <= groups; ++g) {
    const ui::Rgb& fill = t.rgb(help_.groups[g - 1u].color);
    for (unsigned pad = 0; pad < kPadCount; ++pad) {
      if (group_of(pad) == g) {
        cv.round_rect(pad_rect(pad), kPadRadius, fill);
      }
    }
    cv.round_rect(Rect{x0, legend_y + static_cast<int>(g - 1u) * kLegendPitch + 2, 14, 14}, 32, fill);
  }
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    if (group_of(pad) == 0u) {
      const Rect b = pad_rect(pad);
      cv.round_rect(Rect{b.x + 1, b.y + 1, b.w - 2, b.h - 2}, kPadRadius - 16, t.rgb(Color::Pane));
    }
  }
  cv.close();

  // The labels: each group's in the ink of its fill, the dark pads' dim.
  const auto label = [&](unsigned pad, Color ink) {
    const char* s = help_.pad_label[pad];
    if (some(s)) {
      const Rect b = pad_rect(pad);
      put(r, centred(b.x, b.w, text_len(s, kHelpPadLabel), 8), ui::caps_y(t, Face::Label, b.y, b.h), s, kHelpPadLabel, ink, Face::Label);
    }
  };
  for (unsigned g = 1; g <= groups; ++g) {
    for (unsigned pad = 0; pad < kPadCount; ++pad) {
      if (group_of(pad) == g) {
        label(pad, ink_for(help_.groups[g - 1u].color));
      }
    }
  }
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    if (group_of(pad) == 0u) {
      label(pad, Color::TextDim);
    }
  }

  // The legend: a swatch, the name and the line a group; or the words that there is none. Then the line about the pads as a whole.
  const int name_x = x0 + 14 + 8;
  const int text_x = name_x + static_cast<int>(kHelpGroupName) * 8 + 8;
  for (unsigned g = 1; g <= groups; ++g) {
    put(r, name_x, ui::caps_y(t, Face::Label, legend_y + static_cast<int>(g - 1u) * kLegendPitch, kLegendPitch), help_.groups[g - 1u].name, kHelpGroupName, Color::Text, Face::Label);
  }
  for (unsigned g = 1; g <= groups; ++g) {
    put(r, text_x, ui::caps_y(t, Face::Label, legend_y + static_cast<int>(g - 1u) * kLegendPitch, kLegendPitch), help_.groups[g - 1u].text, kHelpGroupText, Color::TextDim, Face::Label);
  }
  int below = legend_y + static_cast<int>(groups) * kLegendPitch + 6;
  if (groups == 0u) {
    r.text(x0, ui::caps_y(t, Face::Label, legend_y, kLegendPitch), kNoPads, Color::TextDim, Face::Label);
    below = legend_y + kLegendPitch + 6;
  }
  if (some(help_.pads)) {
    put(r, x0, ui::caps_y(t, Face::Label, below, kLegendPitch), help_.pads, kHelpPadsText, Color::Text, Face::Label);
  }
}

}  // namespace o3sdk::app
