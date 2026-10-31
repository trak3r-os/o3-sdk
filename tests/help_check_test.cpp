// SPDX-License-Identifier: GPL-3.0-or-later
//
// The kit's help-side helpers (kit/app help.h, src/help.cpp; docs/FRAMEWORK.md, "Help"): help_check() finds each limit the overlay draws with (field, index, what), a text exactly at a limit passes and one
// character over fails, the wrap rule the overlay and the check share, and the note names of a keyboard's pads. HELLO's own Help passes help_check() in sim_hello_test.cpp, and the overlay's agreement with
// the check's line count is pinned in help_overlay_test.cpp.
#include <doctest/doctest.h>

#include <o3sdk/app/help.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

using namespace o3sdk;
using namespace o3sdk::app;

namespace {

// `n` characters of letters (no space).
std::string word(unsigned n) { return std::string(n, 'A'); }

// The first problem of `h`, or none.
struct Found {
  bool ok;
  HelpProblem p;
};
Found check(const Help& h) {
  Found f{};
  f.ok = help_check(h, &f.p);
  return f;
}

#define CHECK_PROBLEM(h, fld, idx, text)                                \
  do {                                                                  \
    const Found f_ = check(h);                                          \
    CHECK_FALSE(f_.ok);                                                 \
    CHECK_MESSAGE(f_.p.field == (fld), help_field_name(f_.p.field));    \
    CHECK(f_.p.index == (idx));                                         \
    CHECK_MESSAGE(std::strcmp(f_.p.what, (text)) == 0, f_.p.what);      \
  } while (0)

constexpr HelpButton kGood[] = {{Button::Play, "START / STOP", "RESTART"}, {Button::Left, "OCTAVE DOWN", nullptr}};
constexpr HelpPadGroup kGroups[] = {{"KEYS", "PLAY THE VOICE", ui::Color::Sel}, {"MODS", "HOLD ONE", ui::Color::Play}};

Help good() {
  Help h;
  h.summary = "THE PADS PLAY THE VOICE. F1-F3 AND THE ENCODER EDIT ITS VALUES.";
  h.fkey[0] = "PICK THE WAVE";
  h.fkey_shift[0] = "MORE";
  h.buttons = kGood;
  h.button_count = 2;
  h.encoder = "CHANGE THE VALUE";
  h.groups = kGroups;
  h.group_count = 2;
  help_pads(h, 0, 0, kPadRows - 1, kPadCols - 1, 1);
  h.pad_group[0] = 2;
  h.pad_label[0] = "C3";
  h.pads = "ONE OCTAVE A ROW.";
  return h;
}

}  // namespace

TEST_CASE("help_check: an empty Help and a filled one pass, and a pass leaves the problem untouched") {
  CHECK(help_check(Help{}));
  HelpProblem p;
  p.what = "UNTOUCHED";
  CHECK(help_check(good(), &p));
  CHECK(std::strcmp(p.what, "UNTOUCHED") == 0);
  CHECK(help_check(good()));  // no problem asked for
}

TEST_CASE("help_check: the summary and the soft keys' texts: at most two lines, no word longer than a line, upper case") {
  Help h = good();
  const std::string line = word(kHelpSummaryLine);
  const std::string two = line + " " + line;
  h.summary = two.c_str();
  CHECK(help_check(h));  // two full lines: at the limit
  const std::string three = two + " X";
  h.summary = three.c_str();
  CHECK_PROBLEM(h, HelpField::Summary, 0, "TOO MANY LINES");
  const std::string newlines = "ONE\nTWO\nTHREE";  // a newline ends a line too
  h.summary = newlines.c_str();
  CHECK_PROBLEM(h, HelpField::Summary, 0, "TOO MANY LINES");
  const std::string tall = word(kHelpSummaryLine + 1);
  h.summary = tall.c_str();
  CHECK_PROBLEM(h, HelpField::Summary, 0, "WORD LONGER THAN A LINE");
  h.summary = "THE PADS play";
  CHECK_PROBLEM(h, HelpField::Summary, 0, "LOWER CASE");

  h = good();
  const std::string f2 = word(kHelpFKeyLine) + " " + word(kHelpFKeyLine);
  h.fkey[4] = f2.c_str();
  CHECK(help_check(h));
  const std::string f3 = f2 + " X";
  h.fkey[4] = f3.c_str();
  CHECK_PROBLEM(h, HelpField::FKey, 4, "TOO MANY LINES");
  const std::string fw = word(kHelpFKeyLine + 1);
  h.fkey[6] = fw.c_str();
  h.fkey[4] = nullptr;
  CHECK_PROBLEM(h, HelpField::FKey, 6, "WORD LONGER THAN A LINE");
  h.fkey[6] = "Fine";
  CHECK_PROBLEM(h, HelpField::FKey, 6, "LOWER CASE");
}

TEST_CASE("help_check: the one-line texts hold to their limits, a character over fails") {
  struct Case {
    HelpField field;
    unsigned limit;
  };
  // fkey_shift 9, encoder 28, pads 96, pad label 5, group name 12, group text 48, button text and shift 28.
  const std::string fs_ok = word(kHelpFKeyShift), fs_bad = word(kHelpFKeyShift + 1);
  const std::string en_ok = word(kHelpButtonText), en_bad = word(kHelpButtonText + 1);
  const std::string pads_ok = word(kHelpPadsText), pads_bad = word(kHelpPadsText + 1);
  const std::string pl_ok = word(kHelpPadLabel), pl_bad = word(kHelpPadLabel + 1);
  const std::string gn_ok = word(kHelpGroupName), gn_bad = word(kHelpGroupName + 1);
  const std::string gt_ok = word(kHelpGroupText), gt_bad = word(kHelpGroupText + 1);

  Help h = good();
  h.fkey_shift[7] = fs_ok.c_str();
  h.encoder = en_ok.c_str();
  h.pads = pads_ok.c_str();
  h.pad_label[47] = pl_ok.c_str();
  HelpPadGroup groups[2] = {{gn_ok.c_str(), gt_ok.c_str(), ui::Color::Sel}, {"B", "B", ui::Color::Play}};
  h.groups = groups;
  HelpButton buttons[2] = {{Button::Play, en_ok.c_str(), en_ok.c_str()}, {Button::Left, "L", nullptr}};
  h.buttons = buttons;
  CHECK(help_check(h));  // every text exactly at its limit

  h.fkey_shift[7] = fs_bad.c_str();
  CHECK_PROBLEM(h, HelpField::FKeyShift, 7, "TOO LONG");
  h.fkey_shift[7] = fs_ok.c_str();
  h.encoder = en_bad.c_str();
  CHECK_PROBLEM(h, HelpField::Encoder, 0, "TOO LONG");
  h.encoder = en_ok.c_str();
  h.pads = pads_bad.c_str();
  CHECK_PROBLEM(h, HelpField::Pads, 0, "TOO LONG");
  h.pads = pads_ok.c_str();
  h.pad_label[47] = pl_bad.c_str();
  CHECK_PROBLEM(h, HelpField::PadLabel, 47, "TOO LONG");
  h.pad_label[47] = pl_ok.c_str();
  groups[0].name = gn_bad.c_str();
  CHECK_PROBLEM(h, HelpField::GroupName, 0, "TOO LONG");
  groups[0].name = gn_ok.c_str();
  groups[1].text = gt_bad.c_str();
  CHECK_PROBLEM(h, HelpField::GroupText, 1, "TOO LONG");
  groups[1].text = "B";
  buttons[0].text = en_bad.c_str();
  CHECK_PROBLEM(h, HelpField::ButtonText, 0, "TOO LONG");
  buttons[0].text = en_ok.c_str();
  buttons[1].shift = en_bad.c_str();
  CHECK_PROBLEM(h, HelpField::ButtonShift, 1, "TOO LONG");
  buttons[1].shift = nullptr;
  CHECK(help_check(h));
}

TEST_CASE("help_check: lower case is found in every text, and a newline in a one-line text") {
  Help h = good();
  h.encoder = "Turn";
  CHECK_PROBLEM(h, HelpField::Encoder, 0, "LOWER CASE");
  h.encoder = "ONE\nTWO";
  CHECK_PROBLEM(h, HelpField::Encoder, 0, "NEWLINE IN A ONE-LINE TEXT");
  h = good();
  h.pad_label[9] = "c3";
  CHECK_PROBLEM(h, HelpField::PadLabel, 9, "LOWER CASE");
  h = good();
  h.pads = "pads";
  CHECK_PROBLEM(h, HelpField::Pads, 0, "LOWER CASE");
  h = good();
  h.fkey_shift[2] = "a";
  CHECK_PROBLEM(h, HelpField::FKeyShift, 2, "LOWER CASE");
  constexpr HelpButton lower[] = {{Button::Play, "OK", nullptr}, {Button::Left, "OK", "no"}};
  h = good();
  h.buttons = lower;
  CHECK_PROBLEM(h, HelpField::ButtonShift, 1, "LOWER CASE");
  constexpr HelpPadGroup lg[] = {{"KEYS", "OK", ui::Color::Sel}, {"mods", "OK", ui::Color::Play}};
  h = good();
  h.groups = lg;
  CHECK_PROBLEM(h, HelpField::GroupName, 1, "LOWER CASE");
  // A character that is not a letter is not a case.
  h = good();
  h.encoder = "1-9 + (SHIFT) / #";
  CHECK(help_check(h));
}

TEST_CASE("help_check: the groups and the pads' groups") {
  Help h = good();
  h.group_count = kHelpMaxGroups;
  HelpPadGroup eight[kHelpMaxGroups];
  for (auto& g : eight) {
    g = HelpPadGroup{"G", "T", ui::Color::Sel};
  }
  h.groups = eight;
  CHECK(help_check(h));  // eight: at the limit
  h.group_count = kHelpMaxGroups + 1u;
  CHECK_PROBLEM(h, HelpField::Groups, 0, "TOO MANY GROUPS");

  h = good();
  h.groups = nullptr;
  CHECK_PROBLEM(h, HelpField::Groups, 0, "NO GROUP TABLE");

  h = good();
  h.pad_group[30] = 3;  // two groups: 3 is past them
  CHECK_PROBLEM(h, HelpField::PadGroup, 30, "GROUP PAST GROUP COUNT");
  h.pad_group[30] = 2;
  CHECK(help_check(h));
  h.pad_group[0] = 0;  // no group: dark, fine
  CHECK(help_check(h));
}

TEST_CASE("help_check: the buttons table") {
  Help h = good();
  h.buttons = nullptr;
  CHECK_PROBLEM(h, HelpField::Buttons, 0, "NO BUTTON TABLE");

  constexpr HelpButton twice[] = {{Button::Play, "A", nullptr}, {Button::Left, "B", nullptr}, {Button::Play, "C", nullptr}};
  h = good();
  h.buttons = twice;
  h.button_count = 3;
  CHECK_PROBLEM(h, HelpField::Buttons, 2, "BUTTON LISTED TWICE");
  h.button_count = 2;  // the repeat is past the count: not read
  CHECK(help_check(h));

  constexpr HelpButton soft[] = {{Button::Play, "A", nullptr}, {Button::F3, "B", nullptr}};
  h = good();
  h.buttons = soft;
  CHECK_PROBLEM(h, HelpField::Buttons, 1, "SOFT KEY IN BUTTONS (USE FKEY)");
  constexpr HelpButton f1[] = {{Button::F1, "A", nullptr}};
  h.buttons = f1;
  h.button_count = 1;
  CHECK_PROBLEM(h, HelpField::Buttons, 0, "SOFT KEY IN BUTTONS (USE FKEY)");
  constexpr HelpButton f8[] = {{Button::F8, "A", nullptr}};
  h.buttons = f8;
  CHECK_PROBLEM(h, HelpField::Buttons, 0, "SOFT KEY IN BUTTONS (USE FKEY)");

  constexpr HelpButton cfg[] = {{Button::Left, "A", nullptr}, {Button::Config, "B", nullptr}};
  h = good();
  h.buttons = cfg;
  CHECK_PROBLEM(h, HelpField::Buttons, 1, "CONFIG IN BUTTONS (THE KIT DESCRIBES IT)");

  // Shift is allowed (the kit gives it a default text; an app may give its own), and so is every other printed button.
  constexpr HelpButton all[] = {{Button::Shift, "A", nullptr}, {Button::Right, "B", nullptr}, {Button::SampleRec, "C", nullptr}, {Button::Song, "D", nullptr}};
  h = good();
  h.buttons = all;
  h.button_count = 4;
  CHECK(help_check(h));

  constexpr HelpButton unknown[] = {{static_cast<Button>(kButtonCount), "A", nullptr}};
  h = good();
  h.buttons = unknown;
  h.button_count = 1;
  CHECK_PROBLEM(h, HelpField::Buttons, 0, "NOT A BUTTON");
}

TEST_CASE("help_check: the first problem is reported, in the order summary, F keys, buttons, encoder, groups, pads") {
  Help h = good();
  h.pads = "bad";
  h.encoder = "bad";
  h.fkey[1] = "bad";
  CHECK_PROBLEM(h, HelpField::FKey, 1, "LOWER CASE");
  h.fkey[1] = nullptr;
  CHECK_PROBLEM(h, HelpField::Encoder, 0, "LOWER CASE");
  h.encoder = nullptr;
  CHECK_PROBLEM(h, HelpField::Pads, 0, "LOWER CASE");
  h.summary = "bad";
  CHECK_PROBLEM(h, HelpField::Summary, 0, "LOWER CASE");
}

TEST_CASE("help_wrap: the rule of the lines, counted and written") {
  char out[3 * 11];
  // A line ends at a newline, else after the last space that lets it fit, else (a long word) at the width.
  CHECK(help_wrap("ONE TWO THREE", 7, 3, out, 8) == 2);
  CHECK(std::strcmp(out, "ONE TWO") == 0);
  CHECK(std::strcmp(out + 8, "THREE") == 0);
  CHECK(help_wrap("ABCDEFGHIJKLMNO", 10, 3, out, 11) == 2);  // the long word is cut in the middle
  CHECK(std::strcmp(out, "ABCDEFGHIJ") == 0);
  CHECK(std::strcmp(out + 11, "KLMNO") == 0);
  CHECK(help_wrap("A\nB", 10, 3, out, 11) == 2);
  CHECK(help_wrap("  LEAD   ", 10, 3, out, 11) == 1);  // the spaces at the ends of a line are dropped
  CHECK(std::strcmp(out, "LEAD") == 0);
  CHECK(help_wrap("", 10, 3) == 0);
  CHECK(help_wrap(nullptr, 10, 3) == 0);
  CHECK(help_wrap("ANYTHING", 0, 3) == 0);
  CHECK(help_wrap("A B C D E F", 3, 2) == 2);  // the text past the last line is lost
  CHECK(help_lines("A B C D E F", 3) == 3);
  CHECK(help_lines("A B C D E F", 11) == 1);
  CHECK(help_lines("A\n\nB", 11) == 3);  // an empty line is a line
  CHECK(help_lines("", 11) == 0);
  // Counting and writing give the same number.
  const char* texts[] = {"THE PADS PLAY THE VOICE", "ONE\nTWO", "AAAAAAAAAAAAAAAAAAAAAAAAAAA", "A B", "  ", "WORD " };
  for (const char* t : texts) {
    for (unsigned w : {1u, 2u, 5u, 14u}) {
      char big[40 * 15];
      CHECK(help_lines(t, w) == help_wrap(t, w, 40, big, 15));
      CHECK(help_wrap(t, w, 2) == (help_lines(t, w) < 2u ? help_lines(t, w) : 2u));
    }
  }
}

TEST_CASE("help_note_label: 60 is C4, C-1 to G9, and nothing outside 0..127") {
  char s[6];
  const struct {
    int note;
    const char* name;
  } cases[] = {{0, "C-1"}, {1, "C#-1"}, {11, "B-1"}, {12, "C0"}, {36, "C2"}, {48, "C3"}, {49, "C#3"}, {54, "F#3"}, {59, "B3"}, {60, "C4"}, {69, "A4"}, {70, "A#4"}, {103, "G7"}, {108, "C8"}, {120, "C9"}, {127, "G9"}};
  for (const auto& c : cases) {
    CHECK(help_note_label(s, c.note));
    CHECK_MESSAGE(std::strcmp(s, c.name) == 0, c.note << " is " << s << ", not " << c.name);
  }
  // The label always fits a pad (kHelpPadLabel characters), and the longest is "C#-1" in 5 bytes.
  std::size_t longest = 0;
  for (int n = 0; n <= 127; ++n) {
    REQUIRE(help_note_label(s, n));
    longest = std::strlen(s) > longest ? std::strlen(s) : longest;
    CHECK(std::strlen(s) <= kHelpPadLabel);
    CHECK(s[0] >= 'A');
    CHECK(s[0] <= 'G');
  }
  CHECK(longest == 4);
  // Outside the range: no label, an empty string.
  for (const int n : {-1, -12, -1000, 128, 129, 1000, INT_MIN, INT_MAX}) {
    std::strcpy(s, "XXXXX");
    CHECK_FALSE(help_note_label(s, n));
    CHECK(s[0] == '\0');
  }
  CHECK_FALSE(help_note_label(nullptr, 60));
}

TEST_CASE("help_pad_note: a pad's label is in the names and the Help points at it") {
  Help h;
  HelpPadNames names = {};
  CHECK(help_pad_note(h, names, 0, 84));
  CHECK(h.pad_label[0] == names[0]);
  CHECK(std::strcmp(h.pad_label[0], "C6") == 0);
  CHECK(help_pad_note(h, names, kPadCount - 1u, 59));
  CHECK(std::strcmp(h.pad_label[kPadCount - 1u], "B3") == 0);
  CHECK(std::strcmp(h.pad_label[0], "C6") == 0);  // the others are not touched
  CHECK_FALSE(help_pad_note(h, names, 5, -1));   // no label, and the pointer is cleared
  CHECK(h.pad_label[5] == nullptr);
  CHECK(h.pad_group[0] == 0);  // the group is the caller's
  CHECK_FALSE(help_pad_note(h, names, kPadCount, 60));
  // A Help built from the notes of a keyboard passes the check.
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    (void)help_pad_note(h, names, pad, 24 + static_cast<int>(pad));
  }
  CHECK(help_check(h));
}

TEST_CASE("help_field_name: every field has a name") {
  for (unsigned f = 0; f <= static_cast<unsigned>(HelpField::Pads); ++f) {
    CHECK(std::strlen(help_field_name(static_cast<HelpField>(f))) > 2u);
  }
}
