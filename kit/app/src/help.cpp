// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of HELP an app calls on its own data: the text rule shared by the overlay and the check, help_check(), and the note names of a keyboard's pads (help.h).
#include <o3sdk/app/help.h>

#include <climits>
#include <cstring>

namespace o3sdk::app {

unsigned help_wrap(const char* s, unsigned width, unsigned max_lines, char* out, unsigned stride) noexcept {
  if (width == 0u) {
    return 0;
  }
  unsigned lines = 0;
  while (s != nullptr && lines < max_lines) {
    while (*s == ' ') {
      ++s;
    }
    if (*s == '\0') {
      break;
    }
    unsigned i = 0;
    unsigned last_space = 0;
    while (s[i] != '\0' && s[i] != '\n' && i < width) {
      if (s[i] == ' ') {
        last_space = i;
      }
      ++i;
    }
    unsigned take = i;
    unsigned next = i;
    if (s[i] == '\n') {
      next = i + 1u;
    } else if (s[i] != '\0') {  // full, and there is more
      if (s[i] == ' ') {
        next = i + 1u;
      } else if (last_space > 0u) {
        take = last_space;
        next = last_space + 1u;
      }
    }
    while (take > 0u && s[take - 1u] == ' ') {
      --take;
    }
    if (out != nullptr) {
      char* line = out + lines * stride;
      for (unsigned k = 0; k < take; ++k) {
        line[k] = s[k];
      }
      line[take] = '\0';
    }
    s += next;
    ++lines;
  }
  return lines;
}

unsigned help_lines(const char* s, unsigned width) noexcept { return help_wrap(s, width, UINT_MAX, nullptr, 0); }

const char* help_field_name(HelpField field) noexcept {
  switch (field) {
    case HelpField::Summary: return "SUMMARY";
    case HelpField::FKey: return "FKEY";
    case HelpField::FKeyShift: return "FKEY SHIFT";
    case HelpField::Buttons: return "BUTTONS";
    case HelpField::ButtonText: return "BUTTON TEXT";
    case HelpField::ButtonShift: return "BUTTON SHIFT";
    case HelpField::Encoder: return "ENCODER";
    case HelpField::Groups: return "GROUPS";
    case HelpField::GroupName: return "GROUP NAME";
    case HelpField::GroupText: return "GROUP TEXT";
    case HelpField::PadLabel: return "PAD LABEL";
    case HelpField::PadGroup: return "PAD GROUP";
    case HelpField::Pads: return "PADS";
  }
  return "";
}

namespace {

bool lower_case(const char* s) noexcept {
  for (; *s != '\0'; ++s) {
    if (*s >= 'a' && *s <= 'z') {
      return true;
    }
  }
  return false;
}

bool has_newline(const char* s) noexcept { return std::strchr(s, '\n') != nullptr; }

// A word (a run of characters that are neither a space nor a newline) longer than `width`: help_wrap() would cut it in the middle.
bool long_word(const char* s, unsigned width) noexcept {
  unsigned run = 0;
  for (; *s != '\0'; ++s) {
    run = (*s == ' ' || *s == '\n') ? 0u : run + 1u;
    if (run > width) {
      return true;
    }
  }
  return false;
}

struct Check {
  HelpProblem* out;
  bool bad(HelpField field, unsigned index, const char* what) const noexcept {
    if (out != nullptr) {
      out->field = field;
      out->index = static_cast<std::uint8_t>(index);
      out->what = what;
    }
    return false;
  }
  // Null is fine (no text); else upper case, in at most `max` characters, with no newline.
  bool line(const char* s, unsigned max, HelpField field, unsigned index) const noexcept {
    if (s == nullptr) {
      return true;
    }
    if (std::strlen(s) > max) {
      return bad(field, index, "TOO LONG");
    }
    if (has_newline(s)) {
      return bad(field, index, "NEWLINE IN A ONE-LINE TEXT");
    }
    return lower_case(s) ? bad(field, index, "LOWER CASE") : true;
  }
  // Null is fine; else upper case, at most 2 lines of `width`, no word longer than a line.
  bool wrapped(const char* s, unsigned width, HelpField field, unsigned index) const noexcept {
    if (s == nullptr) {
      return true;
    }
    if (long_word(s, width)) {
      return bad(field, index, "WORD LONGER THAN A LINE");
    }
    if (help_lines(s, width) > 2u) {
      return bad(field, index, "TOO MANY LINES");
    }
    return lower_case(s) ? bad(field, index, "LOWER CASE") : true;
  }
};

}  // namespace

bool help_check(const Help& h, HelpProblem* first) noexcept {
  const Check c{first};
  if (!c.wrapped(h.summary, kHelpSummaryLine, HelpField::Summary, 0)) {
    return false;
  }
  for (unsigned k = 0; k < kFKeyCount; ++k) {
    if (!c.wrapped(h.fkey[k], kHelpFKeyLine, HelpField::FKey, k) || !c.line(h.fkey_shift[k], kHelpFKeyShift, HelpField::FKeyShift, k)) {
      return false;
    }
  }
  if (h.button_count != 0u && h.buttons == nullptr) {
    return c.bad(HelpField::Buttons, 0, "NO BUTTON TABLE");
  }
  std::uint64_t seen = 0;  // bit n: Button n is listed
  static_assert(kButtonCount <= 64u, "seen holds a bit per button");
  for (unsigned b = 0; b < h.button_count; ++b) {
    const HelpButton& hb = h.buttons[b];
    const unsigned id = static_cast<unsigned>(hb.button);
    if (id >= kButtonCount) {
      return c.bad(HelpField::Buttons, b, "NOT A BUTTON");
    }
    if (is_fkey(hb.button)) {
      return c.bad(HelpField::Buttons, b, "SOFT KEY IN BUTTONS (USE FKEY)");
    }
    if (hb.button == Button::Config) {
      return c.bad(HelpField::Buttons, b, "CONFIG IN BUTTONS (THE KIT DESCRIBES IT)");
    }
    if (((seen >> id) & 1u) != 0u) {
      return c.bad(HelpField::Buttons, b, "BUTTON LISTED TWICE");
    }
    seen |= std::uint64_t{1} << id;
    if (!c.line(hb.text, kHelpButtonText, HelpField::ButtonText, b) || !c.line(hb.shift, kHelpButtonText, HelpField::ButtonShift, b)) {
      return false;
    }
  }
  if (!c.line(h.encoder, kHelpButtonText, HelpField::Encoder, 0)) {
    return false;
  }
  if (h.group_count > kHelpMaxGroups) {
    return c.bad(HelpField::Groups, 0, "TOO MANY GROUPS");
  }
  if (h.group_count != 0u && h.groups == nullptr) {
    return c.bad(HelpField::Groups, 0, "NO GROUP TABLE");
  }
  for (unsigned g = 0; g < h.group_count; ++g) {
    if (!c.line(h.groups[g].name, kHelpGroupName, HelpField::GroupName, g) || !c.line(h.groups[g].text, kHelpGroupText, HelpField::GroupText, g)) {
      return false;
    }
  }
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    if (!c.line(h.pad_label[pad], kHelpPadLabel, HelpField::PadLabel, pad)) {
      return false;
    }
    if (h.pad_group[pad] > h.group_count) {
      return c.bad(HelpField::PadGroup, pad, "GROUP PAST GROUP COUNT");
    }
  }
  return c.line(h.pads, kHelpPadsText, HelpField::Pads, 0);
}

bool help_note_label(char* out, int note) noexcept {
  if (out == nullptr) {
    return false;
  }
  if (note < 0 || note > 127) {
    out[0] = '\0';
    return false;
  }
  static constexpr char kNames[] = "C C#D D#E F F#G G#A A#B ";  // two characters a pitch class: the letter and a '#' or a space
  const unsigned pc = static_cast<unsigned>(note) % 12u;
  unsigned n = 0;
  out[n++] = kNames[pc * 2u];
  if (kNames[pc * 2u + 1u] != ' ') {
    out[n++] = kNames[pc * 2u + 1u];
  }
  const int octave = note / 12 - 1;  // -1..9
  if (octave < 0) {
    out[n++] = '-';
  }
  out[n++] = static_cast<char>('0' + (octave < 0 ? -octave : octave));
  out[n] = '\0';
  return true;
}

bool help_pad_note(Help& h, HelpPadNames& names, unsigned pad, int note) noexcept {
  if (pad >= kPadCount) {
    return false;
  }
  const bool ok = help_note_label(names[pad], note);
  h.pad_label[pad] = ok ? names[pad] : nullptr;
  return ok;
}

}  // namespace o3sdk::app
