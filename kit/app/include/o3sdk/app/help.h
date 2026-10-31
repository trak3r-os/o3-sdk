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

// ---- The note names of a keyboard's pads -------------------------------------------------------------------------------------------------------------------

// The storage of the pad labels a screen builds when HELP opens (Help::pad_label points into it: a member of the screen, `mutable` because Screen::help() is const).
using HelpPadNames = char[kPadCount][kHelpPadLabel + 1];

// The name of a MIDI note into `out` ("C3", "C#3", "F#4", "C-1"), upper case, letter, an optional sharp, the octave. The octave is note / 12 - 1: **60 is C4, 48 is C3, 0 is C-1, 127 is G9** (the MIDI
// convention of o3-sdk's HELLO and o3-303; an app whose docs count 60 as C3 passes note - 12). Writes at most 5 bytes with the terminator ("C#-1" is the longest) and returns true; a note outside 0..127 writes ""
// (no label) and returns false.
bool help_note_label(char* out, int note) noexcept;

// help_note_label() into names[pad] and Help::pad_label[pad] pointing at it (null when the note is outside 0..127, or `pad` is past the grid: false). The group is the caller's (help_pads() or pad_group[]).
bool help_pad_note(Help& h, HelpPadNames& names, unsigned pad, int note) noexcept;

// ---- The text rule, and the check of a filled Help ---------------------------------------------------------------------------------------------------------

// The one rule of the summary's and the soft keys' wrapping, the overlay draws with it and help_check() measures with it, so the two cannot drift: `s` as lines of at most `width` characters (a line ends at a '\n', else
// after the last space that lets it fit, else, for a word longer than the line, at `width`). Leading and trailing spaces of a line are dropped. At most `max_lines` lines are made and the text past them is lost. With `out`
// not null each line is written to out + line * stride (`stride` = width + 1 or more: the line and its terminator); with null only the lines are counted. Returns the lines made. Not for `width` 0 (returns 0).
unsigned help_wrap(const char* s, unsigned width, unsigned max_lines, char* out = nullptr, unsigned stride = 0) noexcept;
// The lines `s` needs at `width`, with no limit of lines (help_wrap() counting): what a text must not exceed.
unsigned help_lines(const char* s, unsigned width) noexcept;

// The fields of a Help, for the first problem help_check() finds.
enum class HelpField : std::uint8_t {
  Summary,     // index 0
  FKey,        // Help::fkey[index]
  FKeyShift,   // Help::fkey_shift[index]
  Buttons,     // the table as a whole (index 0), or the entry `index` (a button listed twice, a soft key or Config in it)
  ButtonText,  // buttons[index].text
  ButtonShift, // buttons[index].shift
  Encoder,     // index 0
  Groups,      // the table as a whole (group_count, a null table), index 0
  GroupName,   // groups[index].name
  GroupText,   // groups[index].text
  PadLabel,    // pad_label[index]
  PadGroup,    // pad_group[index]
  Pads,        // index 0
};
// "SUMMARY", "FKEY", "FKEY SHIFT", "BUTTONS", ...: the field in a test's message.
const char* help_field_name(HelpField field) noexcept;

struct HelpProblem {
  HelpField field = HelpField::Summary;
  std::uint8_t index = 0;    // the f key, the table entry, the group or the pad
  const char* what = "";     // a short text in capitals ("TOO LONG", "TOO MANY LINES", "LOWER CASE", ...), static
};

// Whether a filled Help holds to every limit above and to what the overlay draws with, exactly as the overlay draws it (the same help_wrap()): the summary and the soft keys' texts in at most 2 lines with no word
// longer than a line; the one-line texts (button text and shift, encoder, group name and text, pad label, the Shift row's labels, the pads line) within their limits and without a newline; group_count within
// kHelpMaxGroups and with a table, every pad_group within group_count; a buttons table that exists when button_count is not 0; no button listed twice (the overlay takes the first), no soft key and no Config in the
// table (the soft keys go in fkey, the kit describes Config); no lower-case letter in any text. The first problem found goes to `first` (when not null; untouched when the Help is right). For a test of the app's
// screens, and for a debug build; no heap, no double, a few hundred steps.
bool help_check(const Help& h, HelpProblem* first = nullptr) noexcept;

}  // namespace o3sdk::app
