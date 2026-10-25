// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Widgets the kit's apps share, on top of ui/components.h. Stateless: each draws from what it is given.

#include <o3sdk/app/param_keys.h>
#include <o3sdk/dsp/engine.h>
#include <o3sdk/ui/renderer.h>

#include <cstdint>

namespace o3sdk::app {

// Height the knob strip wants: a caption row, a value row and a bar.
inline constexpr int kKnobStripH = 64;

// Eight parameter cells across `area`, one above each F key: name, value with its unit, and a bar for where the value sits in its
// range. The focused cell is highlighted; held keys (a macro) are marked too. The page's name sits in the first cell's corner when
// there is more than one page.
void knob_strip(ui::Renderer& r, const ui::Rect& area, const dsp::Engine& engine, const ParamKeys& keys) noexcept;

// A horizontal level meter of `peak` (1.0 = full scale), green up to full scale; the last pixels in Accent when it went over.
void level_meter(ui::Renderer& r, const ui::Rect& area, float peak) noexcept;

// A row of `count` step cells across `area`: a cell is filled when `on` has its bit, the cell `playhead` (or none: -1) is in Play
// green, `cursor` (or -1) is framed in Accent. Up to 32 steps.
void step_row(ui::Renderer& r, const ui::Rect& area, unsigned count, std::uint32_t on, int playhead, int cursor) noexcept;

}  // namespace o3sdk::app
