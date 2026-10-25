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
    } else {
      return fail("unknown command '" + cmd + "'");
    }
  }
  return result;
}

}  // namespace o3sdk::sim
