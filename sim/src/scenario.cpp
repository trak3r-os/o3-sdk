// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/sim/scenario.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace o3sdk::sim {

namespace {

bool to_int(const std::string& s, long& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  out = std::strtol(s.c_str(), &end, 10);
  return end != nullptr && *end == '\0';
}

bool to_double(const std::string& s, double& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0';
}

// "140" or "140.5" as milli-BPM; false for anything else or outside 20 .. 300 BPM.
bool to_mbpm(const std::string& s, std::uint32_t& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  const double bpm = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0' || !(bpm >= 20.0 && bpm <= 300.0)) {
    return false;
  }
  out = static_cast<std::uint32_t>(bpm * 1000.0 + 0.5);
  return true;
}

// Two hex digits a byte, "3c" or "0x3c".
bool to_byte(const std::string& s, std::uint8_t& out) {
  const std::size_t skip = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 2u : 0u;
  if (s.size() == skip || s.size() - skip > 2u) {
    return false;
  }
  unsigned v = 0;
  for (std::size_t i = skip; i < s.size(); ++i) {
    const char c = s[i];
    const int d = (c >= '0' && c <= '9') ? c - '0' : ((c >= 'a' && c <= 'f') ? c - 'a' + 10 : ((c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1));
    if (d < 0) {
      return false;
    }
    v = v * 16u + static_cast<unsigned>(d);
  }
  out = static_cast<std::uint8_t>(v);
  return true;
}

// A station: "93.7" (MHz) or "9370" (10 kHz units) as 10 kHz units; false outside the FM bands (76 .. 108 MHz).
bool to_station(const std::string& s, std::uint16_t& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == nullptr || *end != '\0' || !(v > 0.0)) {
    return false;
  }
  const double units = v < 200.0 ? v * 100.0 : v;
  if (!(units >= 7600.0 && units <= 10800.0)) {
    return false;
  }
  out = static_cast<std::uint16_t>(units + 0.5);
  return true;
}

}  // namespace

ScenarioResult run_scenario(Harness& h, const std::string& text, const std::string& out_dir) {
  ScenarioResult result;
  std::istringstream lines(text);
  std::string raw;
  int number = 0;
  auto fail = [&](const std::string& why) {
    result.ok = false;
    result.line = number;
    result.error = why;
    return result;
  };

  while (std::getline(lines, raw)) {
    ++number;
    const std::size_t hash = raw.find('#');
    std::istringstream words(hash == std::string::npos ? raw : raw.substr(0, hash));
    std::vector<std::string> w;
    for (std::string t; words >> t;) {
      w.push_back(t);
    }
    if (w.empty()) {
      continue;
    }
    const std::string& cmd = w[0];
    long n = 0;
    app::Button b{};

    if (cmd == "wait") {
      if (w.size() != 2 || !to_int(w[1], n) || n < 0) {
        return fail("wait <ms>");
      }
      h.run_ms(static_cast<std::uint32_t>(n));
    } else if (cmd == "press" || cmd == "release" || cmd == "tap") {
      if (w.size() < 2 || !app::find_button(w[1].c_str(), b)) {
        return fail(cmd + ": unknown button '" + (w.size() > 1 ? w[1] : "") + "'");
      }
      long hold = 60;
      if (w.size() == 3 && (!to_int(w[2], hold) || hold < 1)) {
        return fail("tap <button> [<ms>]");
      }
      if (cmd != "release") {
        h.press(b);
        h.run_ms(1);
      }
      if (cmd == "tap") {
        h.run_ms(static_cast<std::uint32_t>(hold));
      }
      if (cmd != "press") {
        h.release(b);
        h.run_ms(1);
      }
    } else if (cmd == "pad" || cmd == "paddown" || cmd == "padup") {
      if (w.size() < 2 || !to_int(w[1], n) || n < 0 || n >= static_cast<long>(app::kPadCount)) {
        return fail(cmd + " <0..47>");
      }
      long hold = 150;
      if (cmd == "pad" && w.size() == 3 && (!to_int(w[2], hold) || hold < 1)) {
        return fail("pad <n> [<ms>]");
      }
      const auto pad = static_cast<unsigned>(n);
      if (cmd != "padup") {
        h.pad_down(pad);
        h.run_ms(1);
      }
      if (cmd == "pad") {
        h.run_ms(static_cast<std::uint32_t>(hold));
      }
      if (cmd != "paddown") {
        h.pad_up(pad);
        h.run_ms(1);
      }
    } else if (cmd == "turn") {
      if (w.size() != 2 || !to_int(w[1], n) || n == 0 || n < -100 || n > 100) {
        return fail("turn <detents>, -100..100, not 0");
      }
      h.turn(static_cast<int>(n));
      h.run_ms(1);
    } else if (cmd == "snap") {
      if (w.size() != 2) {
        return fail("snap <name>");
      }
      h.runtime().draw();  // the picture as of now, not as of the last frame tick
      const std::string path = out_dir + "/" + w[1] + ".png";
      if (!h.snapshot(path)) {
        return fail("cannot write " + path);
      }
      std::printf("snap %s\n", path.c_str());
      ++result.snapshots;
    } else if (cmd == "boot") {
      if (w.size() != 2) {
        return fail("boot <name>");
      }
      if (!h.show_boot_screen()) {
        return fail("the display took no boot frame");
      }
      const std::string path = out_dir + "/" + w[1] + ".png";
      if (!h.snapshot(path)) {
        return fail("cannot write " + path);
      }
      std::printf("boot %s\n", path.c_str());
      ++result.snapshots;
    } else if (cmd == "record") {
      if (w.size() != 2 || (w[1] != "on" && w[1] != "off")) {
        return fail("record on|off");
      }
      h.record(w[1] == "on");
    } else if (cmd == "wav") {
      if (w.size() != 2) {
        return fail("wav <name>");
      }
      const std::string path = out_dir + "/" + w[1] + ".wav";
      if (!h.write_wav(path)) {
        return fail("cannot write " + path);
      }
      std::printf("wav %s (%zu frames)\n", path.c_str(), h.audio().size() / 2u);
    } else if (cmd == "midi") {
      if (h.midi() == nullptr) {
        return fail("midi: this harness has no MIDI device (<app>_sim makes one when the scenario has a midi or clock line, or with --midi)");
      }
      platform::host::FakeMidi::Port port{};
      if (w.size() < 3 || (w[1] != "din" && w[1] != "usb")) {
        return fail("midi din|usb <hex bytes>");
      }
      port = w[1] == "din" ? platform::host::FakeMidi::Port::Din : platform::host::FakeMidi::Port::Usb;
      std::vector<std::uint8_t> bytes;
      for (std::size_t i = 2; i < w.size(); ++i) {
        std::uint8_t byte = 0;
        if (!to_byte(w[i], byte)) {
          return fail("midi: '" + w[i] + "' is not a hex byte");
        }
        bytes.push_back(byte);
      }
      if (h.midi()->in_bytes(port, bytes.data(), bytes.size(), h.due_samples()) == 0u) {
        return fail("midi: no complete channel message in those bytes");
      }
    } else if (cmd == "clock") {
      if (h.midi() == nullptr) {
        return fail("clock: this harness has no MIDI device (<app>_sim makes one when the scenario has a midi or clock line, or with --midi)");
      }
      std::uint32_t mbpm = 0;
      if (w.size() == 2 && w[1] == "off") {
        h.midi()->clock_off();
      } else if (w.size() == 3 && to_mbpm(w[1], mbpm) && (w[2] == "start" || w[2] == "stop")) {
        if (w[2] == "start") {
          h.midi()->clock_start(mbpm);
        } else {
          h.midi()->clock_stop(mbpm);
        }
      } else if (w.size() == 4 && to_mbpm(w[1], mbpm) && w[2] == "continue" && to_int(w[3], n) && n >= 0 && n < 16384) {
        h.midi()->clock_continue(mbpm, static_cast<unsigned>(n));
      } else {
        return fail("clock <bpm 20..300> start|stop, clock <bpm> continue <beats>, or clock off");
      }
    } else if (cmd == "input") {
      if (h.capture() == nullptr) {
        return fail("input: this harness has no capture device (<app>_sim makes one when the scenario has an input or radio line, or with --audio-in)");
      }
      if (w.size() >= 2 && w[1] == "tone") {
        double hz = 0.0;
        double dbfs = -12.0;
        if (w.size() < 3 || w.size() > 4 || !to_double(w[2], hz) || (w.size() == 4 && !to_double(w[3], dbfs)) || !(hz >= 20.0 && hz <= 20000.0) ||
            !(dbfs <= 0.0 && dbfs >= -96.0)) {
          return fail("input tone <hz 20..20000> [<dbfs -96..0>]");
        }
        h.capture()->input_tone(static_cast<float>(hz), static_cast<float>(dbfs));
      } else if (w.size() == 3 && w[1] == "wav") {
        std::string why;
        if (!h.capture()->input_wav_file(w[2], why)) {
          return fail("input wav: " + why);
        }
      } else if (w.size() == 2 && w[1] == "off") {
        h.capture()->input_off();
      } else {
        return fail("input tone <hz> [<dbfs>] | input wav <file> | input off");
      }
    } else if (cmd == "radio") {
      if (h.capture() == nullptr) {
        return fail("radio: this harness has no capture device (<app>_sim makes one when the scenario has an input or radio line, or with --audio-in)");
      }
      if (w.size() >= 3 && w[1] == "stations") {
        std::vector<std::uint16_t> list;
        for (std::size_t i = 2; i < w.size(); ++i) {
          std::uint16_t f = 0;
          if (!to_station(w[i], f)) {
            return fail("radio stations: '" + w[i] + "' is not a frequency (87.5 or 8750, 76 .. 108 MHz)");
          }
          list.push_back(f);
        }
        h.capture()->radio_stations(std::move(list));
      } else if (w.size() == 2 && (w[1] == "absent" || w[1] == "present")) {
        h.capture()->radio_absent(w[1] == "absent");
      } else {
        return fail("radio stations <f...> | radio absent | radio present");
      }
    } else if (cmd == "usb") {
      if (h.usb() == nullptr) {
        return fail("usb: this harness has no USB device (<app>_sim makes one when the scenario has a usb, usbwav or card disk line, or with --usb)");
      }
      platform::host::FakeUsbAudio& u = *h.usb();
      app::UsbAudio& ctx = h.runtime().context().usb_audio();
      if (w.size() == 3 && w[1] == "listen" && (w[2] == "on" || w[2] == "off")) {
        u.listen(w[2] == "on");
      } else if ((w.size() == 3 || w.size() == 4) && w[1] == "profile" && to_int(w[2], n) && (n == 2 || n == 8 || n == 10 || n == 11) && (w.size() == 3 || w[3] == "out" || w[3] == "duplex")) {
        const hal::usb_audio::Profile p = n == 2 ? hal::usb_audio::Profile::P2 : (n == 8 ? hal::usb_audio::Profile::P8 : (n == 10 ? hal::usb_audio::Profile::P10 : hal::usb_audio::Profile::P11));
        if (!ctx.request(ctx.audio(), p, w.size() == 4 ? w[3] == "duplex" : ctx.duplex())) {
          return fail("usb profile: refused (a change is running or the card is out)");
        }
      } else if (w.size() == 3 && w[1] == "audio" && (w[2] == "on" || w[2] == "off")) {
        if (!ctx.request(w[2] == "on", ctx.profile(), ctx.duplex())) {
          return fail("usb audio: refused (a change is running or the card is out)");
        }
      } else if (w.size() == 3 && w[1] == "map" && w[2] == "reset") {
        ctx.reset_stems();
      } else if (w.size() == 4 && w[1] == "map" && to_int(w[2], n) && n >= 1 && n <= static_cast<long>(hal::usb_audio::kMaxChannels)) {
        std::uint32_t sources = 0;
        if (!platform::mk66::audio::parse_sources(w[3].c_str(), sources)) {
          return fail("usb map <slot> <sources>: sources like T1+T3, ML, MR, SA, SB or -");
        }
        if (!ctx.set_stem(static_cast<unsigned>(n - 1), sources)) {
          return fail("usb map: the slot is not one of the profile's channels");
        }
      } else if (w.size() == 3 && w[1] == "level" && to_int(w[2], n) && n >= 0 && n <= 100) {
        ctx.set_return_level(static_cast<unsigned>(n));
      } else if (w.size() >= 3 && w[1] == "return" && w[2] == "tone") {
        double hz = 0.0;
        double dbfs = -12.0;
        if (w.size() < 4 || w.size() > 5 || !to_double(w[3], hz) || (w.size() == 5 && !to_double(w[4], dbfs)) || !(hz >= 20.0 && hz <= 20000.0) || !(dbfs <= 0.0 && dbfs >= -96.0)) {
          return fail("usb return tone <hz 20..20000> [<dbfs -96..0>]");
        }
        u.play_tone(static_cast<float>(hz), static_cast<float>(dbfs));
      } else if (w.size() == 4 && w[1] == "return" && w[2] == "wav") {
        std::string why;
        if (!u.play_wav_file(w[3], why)) {
          return fail("usb return wav: " + why);
        }
      } else if (w.size() == 3 && w[1] == "return" && w[2] == "off") {
        u.play_off();
      } else {
        return fail("usb listen on|off | usb profile <2|8|10|11> [out|duplex] | usb audio on|off | usb map <slot> <sources> | usb map reset | usb level <0..100> | usb return tone <hz> [<dbfs>] | usb return wav <file> | usb return off");
      }
      h.run_ms(1);
    } else if (cmd == "usbwav") {
      if (h.usb() == nullptr) {
        return fail("usbwav: this harness has no USB device");
      }
      if (w.size() != 2) {
        return fail("usbwav <name>");
      }
      if (!h.usb()->write_wav(out_dir + "/" + w[1] + ".wav")) {
        return fail("usbwav: cannot write " + out_dir + "/" + w[1] + ".wav");
      }
    } else if (cmd == "card") {
      if (h.card() == nullptr) {
        return fail("card: this harness has no card (<app>_sim --card DIR)");
      }
      if (w.size() == 3 && w[1] == "disk" && (w[2] == "on" || w[2] == "off")) {
        const char* why = h.card_disk(w[2] == "on");
        if (why != nullptr) {
          return fail(std::string("card disk ") + w[2] + ": refused: " + why);
        }
      } else if (w.size() == 2 && w[1] == "out") {
        h.card_out();
      } else if (w.size() == 2 && w[1] == "in") {
        h.card_in();
      } else if (w.size() == 3 && w[1] == "protect" && (w[2] == "on" || w[2] == "off")) {
        h.card_protect(w[2] == "on");
      } else {
        return fail("card out | card in | card protect on|off | card disk on|off");
      }
      h.run_ms(1);
    } else if (cmd == "reboot") {
      if (w.size() != 1) {
        return fail("reboot");
      }
      if (!h.reboot()) {
        return fail("reboot: the app did not start again (see stderr)");
      }
    } else if (cmd == "expect") {
      // expect file <card path> | expect nofile <card path> | expect state <text the state line holds>
      if (w.size() == 3 && (w[1] == "file" || w[1] == "nofile")) {
        if (h.card() == nullptr) {
          return fail("expect " + w[1] + ": this harness has no card (<app>_sim --card DIR)");
        }
        bool is_dir = false;
        const bool there = h.card()->exists(w[2].c_str(), &is_dir) == hal::storage::Status::Ok && !is_dir;
        if (there != (w[1] == "file")) {
          return fail("expect " + w[1] + " " + w[2] + ": " + (there ? "it is there" : "it is not there"));
        }
      } else if (w.size() == 3 && w[1] == "backlight") {
        // expect backlight <duty>: the duty the runtime last told the platform's backlight (the harness needs set_backlight / `--backlight`)
        if (h.backlight_duty() != static_cast<unsigned>(std::strtoul(w[2].c_str(), nullptr, 10))) {
          return fail("expect backlight " + w[2] + ": the backlight was told " + std::to_string(h.backlight_duty()));
        }
      } else if (w.size() >= 3 && w[1] == "state") {
        std::string want = w[2];
        for (std::size_t i = 3; i < w.size(); ++i) {
          want += " " + w[i];
        }
        char line[512];
        (void)h.runtime().state_line(line, sizeof line);
        if (std::string(line).find(want) == std::string::npos) {
          return fail("expect state " + want + ": the state line is '" + line + "'");
        }
      } else {
        return fail("expect file <card path> | expect nofile <card path> | expect state <text> | expect backlight <duty>");
      }
    } else {
      return fail("unknown command '" + cmd + "'");
    }
  }
  return result;
}

}  // namespace o3sdk::sim
