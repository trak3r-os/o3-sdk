// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The kit's convention for F1..F8 and the encoder (docs/FRAMEWORK.md, "Controls"), as a helper a screen owns:
//
//   tap an F key           the encoder drives that key's parameter from now on (latched)
//   hold an F key + turn   the encoder drives that one while it is held; at the release the latched one comes back
//   hold two F keys + turn both move together (an instant macro)
//   turn                   fine steps; Shift + turn: coarse steps (the descriptor's `coarse`)
//   next_page()            the next page of eight (a screen binds it to a button: Params on the kit's apps)
//
// Pages are tables of up to eight parameter ids of one unit. The helper changes values through the engine (set_param), so the unit
// hears every step and the screen shows the stored value.

#include <o3sdk/app/context.h>
#include <o3sdk/app/input.h>
#include <o3sdk/dsp/engine.h>

#include <cstdint>

namespace o3sdk::app {

struct ParamPage {
  const char* name;          // shown in the knob strip's corner ("SOUND", "MODS")
  dsp::UnitId unit;
  std::uint16_t ids[kFKeyCount];
  std::uint8_t count;        // keys used, from F1; the rest are blank
};

class ParamKeys {
 public:
  static constexpr unsigned kMaxPages = 8;

  // The pages (copied by pointer: the table must outlive the helper; a constexpr table is the usual case). Resets the focus to F1.
  void set_pages(const ParamPage* pages, unsigned count) noexcept;
  // Consumes F key presses and releases and encoder turns; true when it did.
  bool input(Context& ctx, const Input& in) noexcept;

  void next_page() noexcept;
  void prev_page() noexcept;
  void set_page(unsigned index) noexcept;
  unsigned page_index() const noexcept { return page_; }
  unsigned page_count() const noexcept { return count_; }
  const ParamPage* page() const noexcept { return count_ != 0u ? &pages_[page_] : nullptr; }

  // The key the encoder drives now (a held one, else the latched one), 0..7.
  unsigned focus() const noexcept;
  // F keys held right now that will move together, bit n = F(n+1).
  unsigned held() const noexcept { return held_; }

 private:
  void turn(Context& ctx, int detents, bool coarse) noexcept;

  const ParamPage* pages_ = nullptr;
  unsigned count_ = 0;
  unsigned page_ = 0;
  unsigned latched_ = 0;
  unsigned before_hold_ = 0;  // the latched key when the first held key went down
  unsigned held_ = 0;         // bit mask of held F keys
  unsigned last_held_ = 0;    // the most recent held key
  bool turned_ = false;       // the encoder moved while keys were held
};

}  // namespace o3sdk::app
