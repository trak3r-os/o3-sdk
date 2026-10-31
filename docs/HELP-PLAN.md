# HELP: Shift + Config explains the controls (plan)

Status: HELP-K built, 2026-10-31 (the "As built" notes below say where it differs from the plan); HELP-A next, in o3-303. HELP-K2 (section 5) built the same day: help_check(), the note-label helpers and the template's HELP. The maintainer asked: "mete el shift+config en el sdk y que sirva para explicar los botones, y si puedes que
tenga imagenes de los pads explicandolos tambien". Shift + Config opens a help overlay that the kit draws for any app. It has two pages:
the buttons as a drawing of key caps with what each does on this screen, and the 48 pads as a drawn 4 x 12 grid with a label on each pad
and a legend. The app supplies only data, one `Help` per screen. Two slices: **HELP-K** in o3-sdk (this repository) and **HELP-A** in
o3-303 (its screens' texts).

## 1. What the user sees

- **Opening:** Shift held plus Config pressed, on any app screen, opens HELP over that screen.
  - The sound keeps playing.
  - The app's input is suspended, as while Config is open. A release of something the app saw pressed still reaches it, so the Shift and a held pad are released.
  - Config alone opens the system Config, as today.
- **Header and soft keys:** the header says `HELP` and the screen's title, for example `HELP  HOME`. The soft keys are F1 BUTTONS, F2 PADS and F8 CLOSE, with the current page's key hot.
- **Moving around:** Left and Right also switch the page.
  - Up, Down and the encoder scroll the BUTTONS page when its texts do not fit.
  - Config (with or without Shift) or F8 closes HELP and the screen is back as it was.
  - The power-off lock closes nothing; a locked input reaches nobody, as today.
- **BUTTONS page:** a drawing of the controls, not a photograph and not to scale. `panel.h` documents the codes, not the positions; a measured layout is out of scope.
  - **The F row:** eight key caps along the bottom, as the soft keys are. Each cap shows its label (from `soft_keys()`) and, under it, the screen's longer text in two lines of the 6 x 12 face, and the Shift row's label when the screen gives one.
  - **The other 25 buttons:** key caps in their printed groups, each cap with the printed name and, beside it, what the button does on this screen (one line, plus a Shift line). The groups are the screen buttons (PARAMS, PERFORMANCE, FILE, CONFIG, MASTER), the sample buttons and SONG, the column buttons (NOTE, INSTR, VOL, FX), PATTERN, the edit and transport buttons (PLAY, REC, COPY, INSERT, DELETE, SHIFT), and the arrows.
  - A button the screen gives no text is drawn dimmed, with its name only.
  - **The encoder:** a round cap with its text.
  - **Summary:** one or two lines at the top about the screen.
- **PADS page:** the grid drawn as 48 pads (rounded boxes), 12 across and 4 down, as on the unit.
  - Each pad shows its short label (at most 5 characters, 6 x 12 face) and is filled with its group's colour.
  - A pad in no group is drawn dark.
  - Under the grid, the legend: per group a swatch, the name and one line of text, then one line about the pads as a whole.
  - A screen with no pad help says `NO PAD HELP ON THIS SCREEN` under an empty grid.
- **A screen with no `help()` at all:** BUTTONS shows its soft-key labels, every other cap dimmed.

## 2. The kit contract (HELP-K, o3-sdk, size M)

**Files**
- New `kit/app/include/o3sdk/app/help.h`, and `help_overlay.h` (a public header, `kit/app/include/o3sdk/app/`, because `runtime.h` holds the `HelpOverlay`) with `kit/app/src/help_overlay.cpp`; `kit/app/CMakeLists.txt` lists the source.
- `screen.h`: one virtual.
- `runtime.{h,cpp}`: the route and the draw.
- `docs/FRAMEWORK.md`, "Help".
- HELLO's screen gets a `help()`, with a new scenario `apps/hello/scenarios/help.txt` whose frames are pinned (new keys in `tests/digests/hello.txt`; every existing pin unchanged).
- New `tests/help_overlay_test.cpp`.
- `docs/NEW-APP.md` gets one paragraph.
- **Every file is o3sdk's own.** Before editing any existing file, diff it against `/home/anortes/Code/track3r-os` after the rename, as every kit step does.

```cpp
// help.h
struct HelpButton {          // one of the 25 buttons that are not soft keys
  Button button;
  const char* text;          // what it does here, one line (at most 28 characters)
  const char* shift;         // with Shift held (null: nothing), one line (at most 28)
};
struct HelpPadGroup {
  const char* name;          // at most 12 characters, upper case
  const char* text;          // one line, at most 48 characters
  ui::Color color;           // a theme colour token (style.h), so the colours follow the screen's theme
};
struct Help {
  const char* summary = nullptr;          // what the screen is for, at most 2 lines of 96 characters
  const char* fkey[kFKeyCount] = {};      // the soft keys' longer texts (null: the label only), at most 2 x 14 characters
  const char* fkey_shift[kFKeyCount] = {};  // the Shift row's labels (null: none), at most 9 characters (a soft-key label)
  const HelpButton* buttons = nullptr;    // a static table
  std::uint8_t button_count = 0;
  const char* encoder = nullptr;          // one line
  const char* pad_label[kPadCount] = {};  // at most 5 characters (null: none)
  std::uint8_t pad_group[kPadCount] = {}; // 0: no group (dark), n: groups[n - 1]
  const HelpPadGroup* groups = nullptr;   // at most 8, a static table
  std::uint8_t group_count = 0;
  const char* pads = nullptr;             // one line about the pads as a whole
};
// Fills pad_group (and pad_label from `labels`, which may be null) over the rectangle of rows r0..r1 and columns c0..c1.
void help_pads(Help& h, unsigned r0, unsigned c0, unsigned r1, unsigned c1, std::uint8_t group, const char* const* labels = nullptr) noexcept;

// screen.h
// What the controls do on this screen, for HELP (Shift + Config). Asked once when HELP opens; `help` starts empty. Pointers must stay
// valid while HELP is open: static tables, or the screen's own members. Default: nothing (HELP shows the soft keys only).
virtual void help(const Context&, Help&) const noexcept {}
```

**Runtime**
- `route()`: a Config press with `panel.shift()` held opens HELP (`HelpOverlay::show(ctx, screen)`); otherwise Config works as today.
- While HELP is open:
  - every press and turn goes to the overlay and its presses are "taken", with the same rule as the system screen;
  - Config closes HELP;
  - the system Config never opens over HELP.
- Opening HELP while Config is open is not possible: the Config press closes Config.
- The `Help` lives in the `HelpOverlay` the Runtime owns (about 0.5 KB with the soft-key labels) and is filled once at `show()`. No heap.
- **As built:**
  - `show()` and the overlay's soft keys are asked with Shift let go (as Panel copy/apply allows), so a screen describes itself as it is without Shift.
  - Config alone still opens the system Config; a Config press with Shift opens HELP, and a screen switch closes HELP.
  - The kit describes Config itself (SYSTEM CONFIG / THIS HELP) and the Shift default (HOLD FOR THE SHIFT ROWS); the app lists the other 24 buttons.
  - The pad LEDs stay dark while HELP is open.

**Drawing**
- With the Renderer and `ui::plot::Canvas` only: rects and text, no bitmaps, so every app gets it without assets. The pads and the encoder are `Canvas` rounded boxes and a circle in raw sections; pad labels use the Label face.
- The theme is the current screen's `theme()`, or else the context's.
- The chrome is always Full: the kit header and the overlay's soft keys.
- Texts longer than their limit are cut, never wrapped past their box.
- **Budget:** every HELP frame under 1,400 display-list words (the scenarios bound 1,800). A glyph is one word, so the BUTTONS page draws a button's texts only for buttons that have one, and scrolls when the total does not fit. The test measures a worst case: every text at its limit.
- **As built:** the list scrolls by rows (two buttons to a row, the printed groups kept), and the rows shown are a dynamic window: as many as the budget leaves after the summary and the F row. The measured worst case is 1,344 words on BUTTONS and 1,251 on PADS.

**Tests** (`help_overlay_test.cpp`)
- Routing:
  - Shift + Config opens HELP, and Config alone opens Config.
  - Config or F8 closes HELP.
  - F1, F2, Left and Right change the page.
  - The app gets no press while HELP is open, but gets the Shift release.
  - The screen's state is the same before and after.
- Drawing:
  - the 48 pads in a 12 x 4 grid;
  - a pad label cut at 5 characters;
  - group colours from the theme;
  - a screen without `help()`;
  - the worst-case word count under 1,400.
- Every pinned digest unchanged, and HELLO's new help frames pinned.
- ASan clean.

**Acceptance**
- Host ctest green with 0 warnings; MK66 `-L mk66` green with no `__aeabi_d`.
- The kit apps' PNGs and WAVs unchanged, except HELLO's new `help.txt` pictures.
- o3-303 builds and passes its ctest against the tree.
- FRAMEWORK.md says all of the above.

## 3. o3-303 (HELP-A, size M, after HELP-K lands)

- **Which screens:** every screen fills `help()`: HOME, PATTERN SETUP, PERFORM, PERFORM SETUP, INSTRUMENT, MASTER, FILE PROJECTS, FILE LIBRARY, RECORD and HOOKS. SONG gets its help with SG2.
- **Where the texts come from:**
  - `docs/UX-303.md` sections 2 and 5;
  - the PADS line strings;
  - the pad maps (`pad_map.h`, `perform.h`);
  - the Shift rows.
- **Pad groups:**
  - HOME and SETUP: steps, modifiers and keyboard;
  - PERFORM: its rows of live actions, with each pad's short action name;
  - FILE LIBRARY: the slots.
- **Where they live:** the texts are constexpr tables in a new `app/src/help_texts.{h,cpp}`.
- **Test** (`help_texts_test`): every limit of section 2 holds, every pad a screen uses has a group, and every `fkey` text belongs to a key the screen labels.
- **Scenario** `app/scenarios/help.txt`: Shift + Config on each screen, with a snap of BUTTONS and of PADS.
- **UX-303:** a new section 5.13 "HELP".
- **The skin:** HELP is drawn with the screen's theme, so it follows OBSIDIAN as soon as SK2 sets it. SKIN-PLAN needs no other change.
- **Unit:** checks go with the next unit image (SU1):
  - the 6 x 12 pad labels legible at arm's length;
  - Shift + Config by hand;
  - nothing lost after closing: the playing pattern, the held encoder owner.

## 4. Order

The single lane runs:
1. SK1 (running): review, then commit.
2. HELP-K: review, then commit and push to o3-sdk.
3. HELP-A: review, then commit and push to o3-303.
4. SK2, SK3, the SU1 image, SG2, SK4 to SK8.

The maintainer asked for HELP now, so it goes ahead of SK2.

## 5. HELP-K2 (as built)

The maintainer asked: "make this available through the sdk, for future engines, the shift config and pads diagrams". The overlay was already in the kit; K2 moves into it the pieces every app would otherwise rewrite
(o3-303's `help_texts_test.cpp` re-implemented the wrap rule to check limits, and HELLO and o3-303 each had a note-name function), and makes a new app start with HELP filled in.

- **`help_check(const Help&, HelpProblem* first = nullptr)`** (`help.h`, `kit/app/src/help.cpp`): true when a filled Help holds to every limit of section 2. It reports the first problem as `HelpProblem{field, index, what}`
  (`help_field_name()` names the field). The wrap rule is **one function**, `help_wrap(s, width, max_lines, out, stride)` (with `out` null it only counts; `help_lines(s, width)` is the unbounded count): the overlay's
  drawing calls it, and so does the check, so they cannot drift. Moving it out of `help_overlay.cpp` changed no pixel: every pin of `tests/digests/*.txt`, HELLO's `help/*` included, is as it was.
  - Checked: the summary and each `fkey` in at most 2 lines of their width and with no word longer than a line; every one-line text within its limit (button text and shift 28, encoder 28, group name 12, group text 48, pad
    label 5, Shift label 9, `pads` 96) and without a newline; no lower-case letter in any text; `group_count` at most 8 and with a table; `pad_group` within `group_count`; a buttons table when `button_count > 0`; no button
    twice; no soft key and no Config in the table; no value that is not a button.
  - Beyond the brief: the newline in a one-line text (the overlay cuts there), `NO GROUP TABLE`, `NOT A BUTTON`, the word-length rule on `fkey` too (the overlay splits a long word mid-word on any wrapped text).
  - Not checked: that a button has a text (a dim cap is allowed), that each pad of a map has a group, that the texts are true: the app's tests.
- **Keyboards**: `help_note_label(char* out, int note)` (true; `""` and false outside 0..127), `HelpPadNames` (`char[kPadCount][kHelpPadLabel + 1]`) and `help_pad_note(h, names, pad, note)`.
  - **Octave convention: `note / 12 - 1`, so 60 is C4, 48 is C3, 0 is C-1.** HELLO's code and o3-303's `note_name` were already the same formula; they differed only below note 12 (HELLO clamped the octave to 0, o3-303
    wrote `C-1`), which HELLO never reaches (its lowest key is note 12). An app that counts 60 as C3 passes `note - 12`.
- **HELLO** uses `help_pad_note()` and a `HelpPadNames` member in place of its own spelling: its `help/*` digests are unchanged.
- **The template** (`templates/app`): `PlayScreen::help()` is real and describes its own controls (FILE, SAMPLE LOAD, DELETE, LEFT / RIGHT, F1-F3 and the encoder, the white and black keys with their note names, which follow
  the octave); `app/scenarios/help.txt` (Shift + Config, BUTTONS, PADS, close) is pinned in `tests/digests/template.txt` (`help/*`, run by `templates/check_template.py`); `tests/app_test.cpp` opens HELP at three octaves and
  checks the Help with `help_check()`.
- **Tests**: `tests/help_check_test.cpp` (every problem with field and index, every limit exactly and one character over, `help_wrap`/`help_lines`, `help_note_label` over 0..127 and outside it, `help_pad_note`);
  `help_overlay_test.cpp` pins that the overlay draws as many lines as `help_lines()` counts (up to two) and that the check calls the third line too long; `sim_hello_test.cpp` checks HELLO's own Help at eight octaves.
- **Docs**: FRAMEWORK.md "Help" (the check, the keyboard helpers, "Give your app HELP"), NEW-APP.md (the template line and a per-screen checklist line).
- **For o3-303 (HELP-A follow-up, its own change)**: `tests/help_texts_test.cpp` can drop `wrapped_lines`, `check_wrapped`, `check_line` and `upper_case` and call `help_check()` on each screen's Help; `note_name()` in
  `app/src/help_texts.cpp` can become `help_note_label()` (its `KeyNames` type, 5 bytes, is enough: the longest label is `C#-1`); its labels stay pixel-identical (same text).
