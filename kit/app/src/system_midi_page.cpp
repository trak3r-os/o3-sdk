// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI page of the system Config screen (docs/M2B2-PLAN.md section 3): the rows of the user's hal::midi::Config, one value each, with
// the device's status under them. New for o3-sdk: track3r-os's Config has its own MIDI page for its own settings; the rows here are the kit's.
// Only tokens of the contrast table are used (Text and TextDim on Pane, SelText on Sel).
#include "system_pages.h"

#include "sys_text.h"

#include <o3sdk/ui/components.h>

namespace o3sdk::app {

using ui::Color;
using ui::Face;

namespace {

using Text = core::FixedString<40>;

enum Row : unsigned { kInChannel, kOutChannel, kOutPorts, kClockOut, kTransportOut, kFollow, kFollowPort, kTransportIn };
static_assert(kTransportIn + 1u == kMidiRowCount, "one caption per row");

const char* caption(unsigned row) noexcept {
  switch (row) {
    case kInChannel: return "IN CHANNEL";
    case kOutChannel: return "OUT CHANNEL";
    case kOutPorts: return "OUT PORTS";
    case kClockOut: return "CLOCK OUT";
    case kTransportOut: return "TRANSPORT OUT";
    case kFollow: return "FOLLOW CLOCK";
    case kFollowPort: return "FOLLOW PORT";
    case kTransportIn: return "TRANSPORT IN";
    default: return "";
  }
}

const char* on_off(bool on) noexcept { return on ? "ON" : "OFF"; }

// The value of a row as the page shows it.
void value_text(Text& out, const hal::midi::Config& c, unsigned row) noexcept {
  out.clear();
  switch (row) {
    case kInChannel:
      if (c.channel == 0u) {
        out.assign("OMNI");
      } else {
        append_uint(out, c.channel);
      }
      break;
    case kOutChannel: append_uint(out, c.out_channel); break;
    case kOutPorts:
      switch (c.out_ports & core::kMidiAllPorts) {
        case core::kMidiDin: out.assign("DIN"); break;
        case core::kMidiUsb: out.assign("USB"); break;
        case core::kMidiAllPorts: out.assign("DIN + USB"); break;
        default: out.assign("NONE"); break;
      }
      break;
    case kClockOut: out.assign(on_off(c.clock_out)); break;
    case kTransportOut: out.assign(on_off(c.transport_out)); break;
    case kFollow: out.assign(on_off(c.follow)); break;
    case kFollowPort: out.assign(c.follow_port == hal::midi::Port::Usb ? "USB" : "DIN"); break;
    case kTransportIn: out.assign(on_off(c.transport_in)); break;
    default: break;
  }
}

// "140.0" from milli-BPM.
void append_bpm(Text& s, std::uint32_t mbpm) noexcept {
  append_uint(s, mbpm / 1000u);
  s.append(".");
  append_uint(s, (mbpm % 1000u) / 100u);
}

}  // namespace

hal::midi::Config midi_step(hal::midi::Config c, unsigned row, int steps, bool wrap) noexcept {
  const auto walk = [&](int value, int lo, int hi) {
    int v = value + steps;
    if (wrap) {
      const int span = hi - lo + 1;
      v = lo + ((v - lo) % span + span) % span;
    }
    return static_cast<std::uint8_t>(v < lo ? lo : (v > hi ? hi : v));
  };
  switch (row) {
    case kInChannel: c.channel = walk(c.channel, 0, 16); break;
    case kOutChannel: c.out_channel = walk(c.out_channel, 1, 16); break;
    case kOutPorts: c.out_ports = walk(c.out_ports & core::kMidiAllPorts, 0, 3); break;  // (the ports are bits 0 and 1: 0, DIN, USB, both)
    case kClockOut: c.clock_out = steps != 0 ? !c.clock_out : c.clock_out; break;
    case kTransportOut: c.transport_out = steps != 0 ? !c.transport_out : c.transport_out; break;
    case kFollow: c.follow = steps != 0 ? !c.follow : c.follow; break;
    case kFollowPort:
      if (steps != 0) {
        c.follow_port = c.follow_port == hal::midi::Port::Din ? hal::midi::Port::Usb : hal::midi::Port::Din;
      }
      break;
    case kTransportIn: c.transport_in = steps != 0 ? !c.transport_in : c.transport_in; break;
    default: break;
  }
  return hal::midi::sanitized(c);
}

void draw_midi_page(ui::Renderer& r, const PageGrid& grid, const hal::midi::Config& c, const hal::midi::Status& s, unsigned row) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int x = grid.x;
  const int value_x = x + 200;
  const int pitch = grid.pitch;
  const auto y_of = [&](int line) { return grid.y0 + line * pitch; };

  Text text;
  for (unsigned i = 0; i < kMidiRowCount; ++i) {
    const bool on = i == row;
    if (on) {
      r.highlight_row({x - m.pad, y_of(static_cast<int>(i)), grid.w + 2 * m.pad, pitch});
    }
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(static_cast<int>(i)), pitch), caption(i), on ? Color::SelText : Color::TextDim, Face::Label);
    value_text(text, c, i);
    r.text(value_x, ui::caps_y(t, Face::Data, y_of(static_cast<int>(i)), pitch), text.c_str(), on ? Color::SelText : Color::Text, Face::Data);
  }

  // The device under the rows: what it makes of the settings, and the traffic.
  const int status_line = static_cast<int>(kMidiRowCount) + 1;
  text.clear();
  if (!c.follow) {
    text.assign("INTERNAL CLOCK");
  } else if (!s.following) {
    text.assign("WAITING FOR A CLOCK ON ");
    text.append(c.follow_port == hal::midi::Port::Usb ? "USB" : "DIN");
  } else {
    text.assign("FOLLOWING ");
    append_bpm(text, s.tempo_mbpm);
    text.append(s.locked ? " LOCKED" : " SETTLING");
  }
  r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(status_line), pitch), "CLOCK", Color::TextDim, Face::Label);
  r.text(value_x, ui::caps_y(t, Face::Data, y_of(status_line), pitch), text.c_str(), Color::Text, Face::Data);

  Text traffic;
  traffic.assign("RX ");
  append_uint(traffic, s.rx);
  traffic.append("   TX ");
  append_uint(traffic, s.tx);
  if (s.dropped != 0u) {
    traffic.append("   LOST ");
    append_uint(traffic, s.dropped);
  }
  r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(status_line + 1), pitch), "TRAFFIC", Color::TextDim, Face::Label);
  r.text(value_x, ui::caps_y(t, Face::Data, y_of(status_line + 1), pitch), traffic.c_str(), Color::Text, Face::Data);

  r.text(x, ui::caps_y(t, Face::Label, y_of(status_line + 3), pitch), "UP / DOWN choose a row. The encoder and F1 change it. Routed notes play the app's instrument.",
         Color::TextDim, Face::Label);
}

}  // namespace o3sdk::app
