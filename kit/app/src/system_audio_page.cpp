// SPDX-License-Identifier: GPL-3.0-or-later
// The AUDIO page of the system Config screen (docs/M2C-PLAN.md section 4): the audio in's source, gain and monitor, a level meter of the input, and,
// while the radio is the input, its frequency, region and seek with what the chip says. New for o3-sdk (track3r-os has the same controls on its Record screen).
// Only tokens of the contrast table are used (Text and TextDim on Pane, SelText on Sel).
#include "system_pages.h"

#include "sys_text.h"

#include <o3sdk/app/widgets.h>
#include <o3sdk/ui/components.h>

namespace o3sdk::app {

using hal::capture::RadioStatus;
using hal::capture::Source;
using ui::Color;
using ui::Face;

namespace {

using Text = core::FixedString<48>;

const char* caption(unsigned row) noexcept {
  switch (row) {
    case kAudioInput: return "INPUT";
    case kAudioGain: return "GAIN";
    case kAudioMonitor: return "MONITOR";
    case kAudioFreq: return "FREQ";
    case kAudioRegion: return "REGION";
    case kAudioSeek: return "SEEK";
    case kAudioUsb: return "USB AUDIO";
    case kAudioProfile: return "PROFILE";
    case kAudioReturn: return "RETURN";
    case kAudioReturnLevel: return "RETURN LEVEL";
    default: return "";
  }
}

const char* source_label(Source s) noexcept {
  switch (s) {
    case Source::LineMix: return "LINE";
    case Source::LineLeft: return "LINE LEFT";
    case Source::LineRight: return "LINE RIGHT";
    case Source::MicLow: return "MIC LOW";
    case Source::MicHigh: return "MIC HIGH";
    case Source::Radio: return "RADIO";
    case Source::Usb: return "USB";
    case Source::Count: break;
  }
  return "";
}

// "93.70 MHZ" from 10 kHz units.
void append_freq(Text& s, unsigned freq) noexcept {
  append_uint(s, freq / 100u);
  s.append(".");
  append_dec(s, freq % 100u, 2);
  s.append(" MHZ");
}

bool radio_dead(const RadioStatus& st) noexcept { return st.state == RadioStatus::State::Absent || st.state == RadioStatus::State::Failed; }

// "8 CHANNELS, MASTER ON 7-8".
void append_profile(Text& s, hal::usb_audio::Profile p) noexcept {
  const unsigned n = hal::usb_audio::profile_channels(p);
  append_uint(s, n);
  s.append(n == 2u ? " CHANNELS, MASTER ONLY" : " CHANNELS, MASTER ON ");
  if (n != 2u) {
    append_uint(s, n - 1u);
    s.append("-");
    append_uint(s, n);
  }
}

void value_text(Text& out, const AudioView& v, unsigned row) noexcept {
  out.clear();
  switch (row) {
    case kAudioUsb:
      out.assign(v.usb.audio ? "ON" : "OFF  (CONSOLE AND MIDI ONLY)");
      break;
    case kAudioProfile:
      append_profile(out, v.usb.profile);
      break;
    case kAudioReturn:
      out.assign(v.usb.duplex ? "ON  (THE COMPUTER CAN PLAY TO THE UNIT)" : "OFF  (CAPTURE ONLY)");
      break;
    case kAudioReturnLevel:
      append_uint(out, v.usb.level);
      out.append(" %");
      if (v.usb.return_open) {
        out.append("   (PLAYING)");
      }
      break;
    case kAudioInput:
      out.assign(source_label(v.source));
      if (v.source == Source::Radio && v.radio.state == RadioStatus::State::Absent) {
        out.append("  (NO RADIO)");
      } else if (v.failed) {
        out.append("  (CODEC NOT ANSWERING)");
      } else if (v.settling) {
        out.append("  (SETTLING)");
      }
      break;
    case kAudioGain:
      append_uint(out, v.gain);
      out.append(" %");
      break;
    case kAudioMonitor:
      if (v.monitor == 0u) {
        out.assign("OFF");
      } else {
        append_uint(out, v.monitor);
        out.append(" %");
      }
      break;
    case kAudioFreq: append_freq(out, v.radio.freq); break;
    case kAudioRegion: out.assign(hal::capture::region_info(v.radio.region).name); break;
    case kAudioSeek:
      switch (v.radio.state) {
        case RadioStatus::State::Seeking: out.assign("SEEKING"); break;
        case RadioStatus::State::Tuning: out.assign("TUNING"); break;
        case RadioStatus::State::Starting: out.assign("STARTING"); break;
        case RadioStatus::State::Ready: out.assign(v.radio.band_limit ? "NO STATION FOUND   F5 / F6 seek" : "F5 / F6 seek down / up"); break;
        default: out.assign("-"); break;
      }
      break;
    default: break;
  }
}

}  // namespace

unsigned audio_rows(AudioRow (&kinds)[kAudioMaxRows], Source source, bool has_input, bool has_usb) noexcept {
  unsigned n = 0;
  if (has_input) {
    const unsigned input = source == Source::Radio ? kAudioBaseRows + kAudioRadioRows : kAudioBaseRows;
    for (unsigned i = 0; i < input; ++i) {
      kinds[n++] = static_cast<AudioRow>(i);
    }
  }
  if (has_usb) {
    for (unsigned i = 0; i < kAudioUsbRows; ++i) {
      kinds[n++] = static_cast<AudioRow>(kAudioUsb + i);
    }
  }
  return n;
}

hal::usb_audio::Profile audio_step_profile(hal::usb_audio::Profile p, int steps, bool wrap) noexcept {
  const int count = static_cast<int>(hal::usb_audio::kProfileCount);
  int to = static_cast<int>(p) + steps;
  to = wrap ? ((to % count) + count) % count : (to < 0 ? 0 : (to >= count ? count - 1 : to));
  return static_cast<hal::usb_audio::Profile>(to);
}

Source audio_step_source(Source source, int steps, bool wrap, bool radio_ok, bool usb_ok) noexcept {
  // The choices that can be reached: every source of the row, but the radio while nobody answers there (it stays when the move starts on it) and the USB return without a USB device.
  Source list[kAudioSourceChoices];
  int count = 0;
  int at = 0;
  for (unsigned i = 0; i < kAudioSourceChoices; ++i) {
    const Source s = static_cast<Source>(i);
    if ((s == Source::Radio && !radio_ok && source != Source::Radio) || (s == Source::Usb && !usb_ok && source != Source::Usb)) {
      continue;
    }
    if (s == source) {
      at = count;
    }
    list[count++] = s;
  }
  int to = at + steps;
  if (wrap) {
    to = ((to % count) + count) % count;
  }
  to = to < 0 ? 0 : (to >= count ? count - 1 : to);
  return list[to];
}

unsigned audio_step_percent(unsigned value, int steps, bool wrap) noexcept {
  int v = static_cast<int>(value > 100u ? 100u : value) + steps;
  if (wrap) {
    v = ((v % 101) + 101) % 101;
  }
  return static_cast<unsigned>(v < 0 ? 0 : (v > 100 ? 100 : v));
}

void draw_audio_page(ui::Renderer& r, const PageGrid& grid, const AudioView& v) noexcept {
  const ui::Theme& t = r.theme();
  const ui::Metrics& m = t.metrics;
  const int x = grid.x;
  const int value_x = x + 200;
  const int pitch = grid.pitch;
  const auto y_of = [&](int line) { return grid.y0 + line * pitch; };

  Text text;
  AudioRow kinds[kAudioMaxRows];
  const unsigned rows = audio_rows(kinds, v.source, v.has_input, v.has_usb);
  for (unsigned i = 0; i < rows; ++i) {
    const bool on = i == v.row;
    const unsigned kind = kinds[i];
    const bool grey = (kind >= kAudioFreq && kind <= kAudioSeek && radio_dead(v.radio)) || (kind >= kAudioUsb && v.usb.changing);  // the radio's rows of a receiver that is not there; the port while it re-enumerates
    if (on) {
      r.highlight_row({x - m.pad, y_of(static_cast<int>(i)), grid.w + 2 * m.pad, pitch});
    }
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(static_cast<int>(i)), pitch), caption(kind), on ? Color::SelText : Color::TextDim, Face::Label);
    value_text(text, v, kind);
    r.text(value_x, ui::caps_y(t, Face::Data, y_of(static_cast<int>(i)), pitch), text.c_str(), on ? Color::SelText : (grey ? Color::TextDim : Color::Text), Face::Data);
  }

  // The input's level, with the gain and the gate: what the units and the monitor hear.
  int line = static_cast<int>(rows) + 1;
  if (v.has_input) {
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "LEVEL L", Color::TextDim, Face::Label);
    level_meter(r, {value_x, y_of(line) + 6, 300, pitch - 12}, v.meter.l);
    ++line;
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "LEVEL R", Color::TextDim, Face::Label);
    level_meter(r, {value_x, y_of(line) + 6, 300, pitch - 12}, v.meter.r);
    ++line;
  }
  if (v.has_usb) {
    ++line;
    text.clear();
    text.assign(v.usb.changing ? "THE PORT IS RECONNECTING" : (!v.usb.audio ? "STANDARD" : (v.usb.return_open ? "CAPTURE AND RETURN OPEN" : (v.usb.streaming ? "CAPTURE OPEN" : "NO STREAM OPEN"))));
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "USB", Color::TextDim, Face::Label);
    r.text(value_x, ui::caps_y(t, Face::Data, y_of(line), pitch), text.c_str(), Color::Text, Face::Data);
    ++line;
    if (v.usb.proposed) {
      r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "F1 AGAIN APPLIES IT: THE PORT RECONNECTS, THE COMPUTER SEES THE UNIT GO AND COME BACK.", Color::Text, Face::Label);
      ++line;
    }
  }

  if (v.has_input && v.source == Source::Radio) {
    ++line;
    text.clear();
    switch (v.radio.state) {
      case RadioStatus::State::Off: text.assign("OFF"); break;
      case RadioStatus::State::Starting: text.assign("STARTING"); break;
      case RadioStatus::State::Ready: text.assign("READY"); break;
      case RadioStatus::State::Tuning: text.assign("TUNING"); break;
      case RadioStatus::State::Seeking: text.assign("SEEKING"); break;
      case RadioStatus::State::Absent: text.assign("NOT ON THIS UNIT"); break;
      case RadioStatus::State::Failed:
        text.assign("FAILED ");
        text.append(v.radio.fail);
        break;
    }
    if (v.radio.state == RadioStatus::State::Ready || v.radio.state == RadioStatus::State::Tuning || v.radio.state == RadioStatus::State::Seeking) {
      text.append("   SIGNAL ");
      append_uint(text, v.radio.rssi);
      text.append(v.radio.stereo ? "   STEREO" : "   MONO");
    }
    r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "RADIO", Color::TextDim, Face::Label);
    r.text(value_x, ui::caps_y(t, Face::Data, y_of(line), pitch), text.c_str(), Color::Text, Face::Data);
    ++line;
    if (radio_dead(v.radio)) {
      r.text(x + m.pad, ui::caps_y(t, Face::Label, y_of(line), pitch), "F5 tries the receiver again. The chip did not answer: this board may have no FM radio.", Color::TextDim, Face::Label);
      ++line;
    }
  }
  ++line;
  r.text(x, ui::caps_y(t, Face::Label, y_of(line), pitch), "UP / DOWN choose a row. The encoder and F1 change it (SHIFT: coarser).",
         Color::TextDim, Face::Label);
}

}  // namespace o3sdk::app
