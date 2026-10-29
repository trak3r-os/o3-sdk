// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pages an app adds to the system Config screen (docs/M2C-PLAN.md section 4, decision D18): after the kit's FIRMWARE, AUDIO, MIDI, CARD, DISPLAY and INFO, one tab a page, in the look
// and with the keys of the kit's own (UP / DOWN choose a row, the encoder and F1 change a value, F1 on an action asks and F1 again, within kAskMs, runs it).
//
// A page is a constexpr table the app returns from App::config_page(); an item is
//   a value   `value` is a core::ParamDesc (range, steps, display); App::config_get(key) and config_set(key, v) are the app's, called in the main loop only. The kit keeps the value in
//             the settings file (`APPV`, keyed by `key`) and gives it back through config_set() at boot, after App::setup() has set the defaults; a saved value outside its descriptor is dropped.
//   an action `value` is null; App::config_run(key) runs after the confirming second F1, in the main loop (a firmware update is refused meanwhile exactly as for any job: an action that
//             saves goes through Context::files()). Nothing is kept for it.
// `key` is the item's stable identity, 1 .. 65535, unique in the app: append, never reuse one (an older settings file keeps meaning what it meant), like a unit's parameter id.
// At most kMaxConfigPages pages of kMaxConfigItems items; more are not shown. A table that breaks a rule (a zero or repeated key, a descriptor param_desc_valid refuses, no label) has
// that item left out, not the page.
//
//   static constexpr core::ParamDesc kLevel = {"LEVEL", "%", nullptr, nullptr, 0, 100, 80, 100, 10, 0, core::ParamUnit::Int, core::ParamCurve::Linear, 0, 0};
//   static constexpr ConfigItem kItems[] = {{1, "LEVEL", &kLevel}, {2, "CLEAR PATTERN", nullptr}};
//   unsigned config_pages() const noexcept override { return 1; }
//   bool config_page(unsigned, ConfigPage& out) const noexcept override { out = {"HELLO", kItems, 2}; return true; }
//
// Pure data and the rules that read it: no heap, no allocation.

#include <o3sdk/core/param.h>

#include <cstdint>

namespace o3sdk::app {

class App;

inline constexpr unsigned kMaxConfigPages = 4;
inline constexpr unsigned kMaxConfigItems = 8;
inline constexpr unsigned kConfigLabelChars = 20;  // the longest caption a row makes room for (a longer one is cut)

struct ConfigItem {
  std::uint16_t key;
  const char* label;                 // upper case
  const core::ParamDesc* value;      // null: an action
};

struct ConfigPage {
  const char* title;                 // the tab, upper case
  const ConfigItem* items;
  unsigned count;
};

// What the kit makes of an app's pages: for each valid page (one with a valid item) its table and the mask of its valid items; the app's tables stay the app's and must outlive the view
// (constexpr data). Built by the runtime once the app has set up; public for the tests.
struct ConfigView {
  struct Page {
    const char* title;
    const ConfigItem* items;
    std::uint8_t count;  // entries of `items` looked at (at most kMaxConfigItems)
    std::uint8_t mask;   // bit i: items[i] is valid
    // The `n`-th valid item (n < valid()), or null.
    const ConfigItem* nth(unsigned n) const noexcept;
    unsigned valid() const noexcept;
  };
  Page page[kMaxConfigPages] = {};
  std::uint8_t pages = 0;
  std::uint8_t values = 0;  // items with a value, in all

  // Reads `app`'s pages: those config_page() gives, with at least one valid item. A key that an earlier item has is left out.
  void build(const App& app) noexcept;
  // The item with `key` that has a value (null: none).
  const ConfigItem* find_value(std::uint16_t key) const noexcept;
};

}  // namespace o3sdk::app
