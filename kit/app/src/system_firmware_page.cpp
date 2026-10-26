// SPDX-License-Identifier: GPL-3.0-or-later
// The FIRMWARE page of the system Config screen: the list of Firmware/*.ptf, the two questions, the check's bar and the hand-over's "do not
// power off". Class N of docs/M2B2-PLAN.md: drawn with kit/ui on the data pane instead of track3r-os's modal dialog (app/src/screens/
// firmware_overlay.cpp); the texts, the 8 rows, the size and RESET TARGET tags, the clipped name and the "more files" note are copied from it. The keys
// are the soft-key row's (system_config.cpp); the dialog's own key hints are not drawn. Only tokens of the contrast table are used (Text and
// TextDim on Pane, SelText on Sel, AccentText on Accent, Play on Pane).
#include "system_pages.h"

#include "sys_text.h"

#include <o3sdk/ui/components.h>

namespace o3sdk::app {

using ui::Color;
using ui::Face;

namespace {

constexpr int kLine = 2;                                         // rows of the message / question area above the list
constexpr int kRows = static_cast<int>(FirmwareView::kRows);

using Text = FirmwareView::Text;

// "12345 KB" for a file's size (a .ptf is HEX text of some hundreds of KB).
void append_kib(Text& out, std::uint32_t bytes) noexcept {
  append_uint(out, (bytes + 1023u) / 1024u);
  out.append(" KB");
}

// The rows of the body: `put` writes text on row `row` from `x`, in the face and ink given (one place that does the vertical centring).
struct Body {
  ui::Renderer& r;
  int y0;  // the first row's top
  int pitch;
  void put(int row, int x, const char* text, Face face, Color ink) const { r.text(x, ui::caps_y(r.theme(), face, y0 + row * pitch, pitch), text, ink, face); }
};

std::uint16_t permille_of(std::uint32_t done, std::uint32_t total) noexcept {
  const std::uint32_t p = ((done >> 8) * 1000u) / ((total >> 8) | 1u);  // (the file is at most 4 MiB: no overflow; a total under 256 B reads as 0)
  return static_cast<std::uint16_t>(p > 1000u ? 1000u : p);
}

}  // namespace

void draw_firmware_page(ui::Renderer& r, const PageGrid& grid, const FirmwareView& v) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int x = grid.x;
  const int w = grid.w;
  const int right = x + w;
  const int pitch = grid.pitch;
  const int cell = t.geometry(Face::Data).cell_w;
  const Body body{r, grid.y0, pitch};
  const auto data_line = [&](int row, const char* text) { body.put(row, x, text, Face::Data, Color::Text); };
  const auto label_line = [&](int row, const char* text, Color ink = Color::TextDim) { body.put(row, x, text, Face::Label, ink); };
  const auto bar = [&](int row) {
    ui::bar(r, {x, grid.y0 + row * pitch + m.gap, w, pitch - 2 * m.gap}, permille_of(v.done, v.total), 1000u, Color::Empty, Color::Play);
  };

  using Phase = FirmwareView::Phase;
  Text line;
  switch (v.phase) {
    case Phase::Closed:
      label_line(0, "Switch this unit to another image from the card's Firmware folder.", Color::Text);
      label_line(kLine, "F1 reads the folder. The list is read and nothing is written to the card.");
      label_line(kLine + 1, "Every image carries this screen, so the unit can always change again.");
      break;
    case Phase::List: {
      if (!v.msg.empty()) {  // (one line: kMsgChars is what the page is wide enough for)
        if (v.bad) {
          const int chip_w = r.text_width(v.msg.c_str(), Face::Label) + 4 * m.pad;
          r.panel({x, grid.y0 + m.gap, chip_w, pitch - 2 * m.gap}, Color::Accent);
          body.put(0, x + 2 * m.pad, v.msg.c_str(), Face::Label, Color::AccentText);
        } else {
          label_line(0, v.msg.c_str(), Color::Text);
        }
      } else {
        label_line(0, "Choose a file of the Firmware folder.", Color::Text);
      }
      if (v.n == 0) {
        data_line(kLine, "NO .PTF IN FIRMWARE/");
      }
      const int cursor = v.cursor < v.n ? v.cursor : 0;
      const int first = cursor < kRows ? 0 : cursor - kRows + 1;
      for (int k = 0; k < kRows && first + k < static_cast<int>(v.n); ++k) {
        const hal::firmware::File& f = v.files[first + k];
        const int row = kLine + k;
        const bool on = first + k == cursor;
        const Color ink = on ? Color::SelText : Color::Text;
        const Color dim = on ? Color::SelText : Color::TextDim;
        if (on) {
          r.highlight_row({x - m.pad, grid.y0 + row * pitch, w + 2 * m.pad, pitch});
        }
        const int name_x = x + m.pad;
        int name_end = right - m.pad;  // where the next text of the row begins: the size, or on the first file the tag before it
        line.clear();
        append_kib(line, f.size);
        const int size_w = r.text_width(line.c_str(), Face::Label);
        name_end -= size_w;
        body.put(row, right - m.pad - size_w, line.c_str(), Face::Label, dim);
        if (f.reset_target) {  // the reset button flashes the first .ptf of the folder
          const char* tag = "RESET TARGET";
          name_end -= 2 * cell + r.text_width(tag, Face::Label);
          body.put(row, name_end, tag, Face::Label, dim);
        }
        // The name has the room up to `name_end`, one cell free: a name of the full 52 characters is cut to "..." on the first file, whose tag takes the room (the next step shows the whole name).
        const int room_cells = (name_end - name_x) / cell - 1;
        const std::size_t room = room_cells > 0 ? static_cast<std::size_t>(room_cells) : 0u;
        std::size_t len = 0;
        while (len < sizeof f.name && f.name[len] != '\0') {
          ++len;
        }
        if (len <= room) {
          body.put(row, name_x, f.name, Face::Data, ink);
        } else {
          line.clear();
          append_clipped(line, f.name, room > 3 ? room - 3 : 0);
          line.append("...");
          body.put(row, name_x, line.c_str(), Face::Data, ink);
        }
      }
      if (v.more) {
        const char* note = "MORE FILES ON THE CARD ARE NOT LISTED";
        body.put(kLine + kRows, right - r.text_width(note, Face::Label), note, Face::Label, Color::TextDim);
      }
      label_line(kLine + kRows + 1, "UP / DOWN or the encoder choose. F1 asks first; nothing is written before INSTALL.");
      break;
    }
    case Phase::AskUpdate:
      line.assign("UPDATE TO ");
      line.append(v.chosen());
      line.append("?");
      data_line(0, line.c_str());
      label_line(1, "It is checked first. Nothing is written.");
      label_line(kLine, "F1 YES. Any other key takes the question back.");
      break;
    case Phase::Checking:
      line.assign("CHECKING ");
      line.append(v.chosen());
      data_line(0, line.c_str());
      bar(kLine);
      label_line(kLine + 1, "Reading the card.");
      break;
    case Phase::AskInstall: {
      line.assign("IMAGE OK, ");
      append_uint(line, v.image);
      line.append(" B, CRC ");
      char crc[9];  // the CRC-32 of the whole file, as `fw arm` prints it: 8 upper-case digits
      *put_hex2(put_hex2(put_hex2(put_hex2(crc, v.crc >> 24), v.crc >> 16), v.crc >> 8), v.crc) = '\0';
      line.append(crc);
      data_line(0, line.c_str());
      label_line(1, v.chosen(), Color::Text);
      label_line(kLine, "Good for 30 s. F1 INSTALL. Any other key cancels.");
      break;
    }
    case Phase::Installing: {
      const char* warn = "UPDATING: DO NOT POWER OFF";
      const int box_h = pitch - 2 * m.gap;
      r.panel({x, grid.y0 + m.gap, r.text_width(warn, Face::Data) + 4 * m.pad, box_h}, Color::Accent);  // the one thing on the screen that matters now
      body.put(0, x + 2 * m.pad, warn, Face::Data, Color::AccentText);
      label_line(1, "The unit restarts by itself.");
      bar(kLine);
      break;
    }
  }
}

}  // namespace o3sdk::app
