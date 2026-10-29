// SPDX-License-Identifier: GPL-3.0-or-later
// The CARD page of the system Config screen (docs/M2C-PLAN.md section 4 and 7): how the files service sees the card, the volume, the job it runs, and, with a USB audio device, CARD AS DISK (the
// card shared to the computer, never at boot) and READ ONLY (the policy of the next share). New for o3-sdk; the disk's rules are track3r-os's OS-USBDISK.
// Only tokens of the contrast table are used (Text and TextDim on Pane, SelText on Sel).
#include "system_pages.h"

#include "sys_text.h"

#include <o3sdk/ui/components.h>

namespace o3sdk::app {

using ui::Color;
using ui::Face;

namespace {

using Text = core::FixedString<72>;

const char* caption(unsigned row) noexcept {
  switch (row) {
    case kCardState: return "CARD";
    case kCardVolume: return "VOLUME";
    case kCardFiles: return "FILES";
    case kCardDisk: return "CARD AS DISK";
    case kCardReadOnly: return "READ ONLY";
    default: return "";
  }
}

void value_text(Text& out, const CardView& v, unsigned row) noexcept {
  out.clear();
  switch (row) {
    case kCardState:
      out.assign(v.state);
      break;
    case kCardVolume:
      if (v.volume.kind == hal::storage::FsKind::None) {
        out.assign("-");
      } else {
        out.assign(hal::storage::fs_kind_name(v.volume.kind));
        if (v.volume.capacity_mb != 0u) {
          out.append("   ");
          append_uint(out, v.volume.capacity_mb);
          out.append(" MB");
        }
      }
      break;
    case kCardFiles:
      out.assign(v.files[0] != '\0' ? v.files : "IDLE");
      break;
    case kCardDisk:
      if (v.ask == CardAsk::Share) {
        out.assign(v.read_only ? "F1 AGAIN: SHARE READ ONLY, THE PORT RECONNECTS" : "F1 AGAIN: SHARE WRITABLE, THE PORT RECONNECTS");
      } else if (v.ask == CardAsk::Eject) {
        out.assign("F1 AGAIN: EJECT");
      } else if (v.disk.phase == hal::usb_audio::DiskPhase::Out) {
        out.assign(v.disk.writable ? "ON THE COMPUTER (WRITABLE)   F1 TWICE: EJECT" : "ON THE COMPUTER (READ ONLY)   F1 TWICE: EJECT");
      } else if (v.disk.phase == hal::usb_audio::DiskPhase::Working) {
        out.assign("WORKING...");
      } else {
        out.assign(v.disk.card_present ? (v.disk.busy ? "OFF  (THE CARD IS IN USE)" : "OFF   F1 TWICE: SHARE") : "OFF  (NO CARD)");
      }
      break;
    case kCardReadOnly:
      out.assign(v.read_only ? "ON" : "OFF");
      break;
    default: break;
  }
}

}  // namespace

void draw_card_page(ui::Renderer& r, const PageGrid& grid, const CardView& v) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int x = grid.x;
  const int value_x = x + 200;
  const int pitch = grid.pitch;
  const auto y_of = [&](int line) { return grid.y0 + line * pitch; };

  Text text;
  const unsigned rows = kCardBaseRows + (v.has_usb ? kCardUsbRows : 0u);
  for (unsigned i = 0; i < rows; ++i) {
    const bool on = i == v.row;
    if (on) {
      r.highlight_row({x - m.pad, y_of(static_cast<int>(i)), grid.w + 2 * m.pad, pitch});
    }
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(static_cast<int>(i)), pitch), caption(i), on ? Color::SelText : Color::TextDim, Face::Label);
    value_text(text, v, i);
    r.text(value_x, ui::caps_y(t, Face::Data, y_of(static_cast<int>(i)), pitch), text.c_str(), on ? Color::SelText : Color::Text, Face::Data);
  }
  int line = static_cast<int>(rows) + 1;
  if (v.has_usb) {
    r.text(x, ui::caps_y(t, Face::Label, y_of(line), pitch), "CARD AS DISK gives the card to the computer. The unit reads and writes nothing until the eject,", Color::TextDim, Face::Label);
    ++line;
    r.text(x, ui::caps_y(t, Face::Label, y_of(line), pitch), "and what it wanted to save waits. READ ONLY is the policy of the next share.", Color::TextDim, Face::Label);
    ++line;
  }
  r.text(x, ui::caps_y(t, Face::Label, y_of(line), pitch), "UP / DOWN choose a row. F1 changes it (twice for the disk).", Color::TextDim, Face::Label);
}

}  // namespace o3sdk::app
