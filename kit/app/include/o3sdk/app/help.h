// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a screen tells HELP (Shift + Config, help_overlay.h; docs/FRAMEWORK.md, "Help"): what each button does on this screen, what each of the
// 48 pads is, and a line about the screen. The kit draws it (two pages: the buttons as key caps, the pads as a 4 x 12 grid with a legend); the app
// supplies only this data from Screen::help(), once when HELP opens. Every pointer must stay valid while HELP is open: static tables, or the
// screen's own members. Texts are upper case, like every label on the screen. The summary and the soft keys' texts wrap at a word into their lines;
// every text is cut where its box (its lines) ends, never drawn past it.

#include <o3sdk/app/panel.h>
#include <o3sdk/ui/style.h>

#include <cstdint>

namespace o3sdk::app {

// The limits of the texts, in characters (what the drawing keeps; the rest is cut).
inline constexpr unsigned kHelpSummaryLine = 96;   // Help::summary: two lines of this
inline constexpr unsigned kHelpFKeyLine = 14;      // Help::fkey: two lines of this
inline constexpr unsigned kHelpFKeyShift = 9;      // Help::fkey_shift: a soft-key label
inline constexpr unsigned kHelpButtonText = 28;    // HelpButton::text and ::shift, and Help::encoder
inline constexpr unsigned kHelpPadLabel = 5;       // Help::pad_label
inline constexpr unsigned kHelpGroupName = 12;     // HelpPadGroup::name
inline constexpr unsigned kHelpGroupText = 48;     // HelpPadGroup::text
inline constexpr unsigned kHelpPadsText = 96;      // Help::pads
inline constexpr unsigned kHelpMaxGroups = 8;      // Help::groups

// One of the 25 buttons that are not soft keys. Config is the runtime's (the app never sees it) and is described by the kit; a Shift entry is optional (the kit says
// what it does when the screen gives none). A button the screen lists no text for is drawn dimmed, with its name only.
struct HelpButton {
  Button button;
  const char* text;   // what it does here, one line (at most kHelpButtonText characters)
  const char* shift;  // with Shift held (null: nothing), one line (at most kHelpButtonText)
};

struct HelpPadGroup {
  const char* name;  // at most kHelpGroupName characters, upper case
  const char* text;  // one line, at most kHelpGroupText characters
  ui::Color color;   // a theme colour token (style.h), so the colours follow the screen's theme
};

struct Help {
  const char* summary = nullptr;            // what the screen is for: at most 2 lines of kHelpSummaryLine characters (wrapped at a space, or at a '\n')
  const char* fkey[kFKeyCount] = {};        // the soft keys' longer texts (null: the label only): at most 2 lines of kHelpFKeyLine characters, wrapped like the summary
  const char* fkey_shift[kFKeyCount] = {};  // the Shift row's labels (null: none), at most kHelpFKeyShift characters
  const HelpButton* buttons = nullptr;      // a static table
  std::uint8_t button_count = 0;
  const char* encoder = nullptr;            // what turning it does, one line (at most kHelpButtonText)
  const char* pad_label[kPadCount] = {};    // at most kHelpPadLabel characters (null: none)
  std::uint8_t pad_group[kPadCount] = {};   // 0: no group (drawn dark), n: groups[n - 1]
  const HelpPadGroup* groups = nullptr;     // at most kHelpMaxGroups, a static table
  std::uint8_t group_count = 0;
  const char* pads = nullptr;               // one line about the pads as a whole (at most kHelpPadsText)
};

// Puts `group` (1..) on the pads of rows r0..r1 and columns c0..c1 (inclusive, row 0 on top; a range past the grid is cut to it) and, when `labels` is not null, their labels from it, row by row
// from the rectangle's top left ((r1 - r0 + 1) * (c1 - c0 + 1) entries; a null entry: no label).
void help_pads(Help& h, unsigned r0, unsigned c0, unsigned r1, unsigned c1, std::uint8_t group, const char* const* labels = nullptr) noexcept;

}  // namespace o3sdk::app
