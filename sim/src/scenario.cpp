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
    } else {
      return fail("unknown command '" + cmd + "'");
    }
  }
  return result;
}

}  // namespace o3sdk::sim
