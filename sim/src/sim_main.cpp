// SPDX-License-Identifier: GPL-3.0-or-later
//
// <app>_sim: runs the app this executable is linked with (o3sdk_app(), cmake/O3sdkTargets.cmake) headless on the host.
//
//   <app>_sim [--scenario FILE] [--out DIR] [--wav FILE] [--ms N] [--firmware-dir DIR] [--midi] [--audio-in] [--card DIR]
//
// Without a scenario it runs N milliseconds (default 500) and writes DIR/<app>.png. --wav records the whole run. Exit 1 when the
// scenario fails, when a frame did not fit the display list or the raster refused one, or when the app's setup had a call the engine
// refused (an attach, a route or a hook that was not taken: Engine::setup_errors(); the refusals are listed on stderr), so a script can
// gate on it. --firmware-dir gives the system Config screen a fake update path over the *.ptf files of DIR (load_firmware_dir: nothing is
// read or written, no flash exists); without it the screen has no FIRMWARE section. --midi gives the app a fake MIDI device
// (platform/host/fake_midi.h: the MIDI section of the system Config, the routes, the clock); a scenario that has a `midi` or `clock` line gets
// one without the option, and an app run without one sees the platform it had before MIDI existed. --audio-in gives it a fake codec input and FM radio (platform/host/fake_capture.h: the
// AUDIO section of the system Config, Context::audio_in() and radio()); a scenario that has an `input` or `radio` line gets one without the option, and an app run without one has no
// audio in and a silent input. --card DIR makes the directory (created when it is not there) the app's SD card
// (platform/host/dir_card.h): Context::files() saves and loads there, under DIR/Apps/<app>/, the kit keeps the theme and the MIDI settings in DIR/Apps/<app>/system.o3s, and a scenario may
// pull the card, protect it and reboot (scenario.h). Without it the app has no card ("NO CARD") and the kit keeps nothing; DIR is the user's, nothing outside Apps/<app>/ and Samples/ is
// written in it.
#include <o3sdk/app/app.h>
#include <o3sdk/platform/host/dir_card.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s [--scenario FILE] [--out DIR] [--wav FILE] [--ms N] [--firmware-dir DIR] [--midi] [--audio-in] [--card DIR]\n", argv0);
}

// Does the scenario text have a `midi` or `clock` command (the first word of a line)?
bool uses_midi(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words(line.substr(0, line.find('#')));
    std::string first;
    if ((words >> first) && (first == "midi" || first == "clock")) {
      return true;
    }
  }
  return false;
}

// Does the scenario text have an `input` or `radio` command?
bool uses_capture(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words(line.substr(0, line.find('#')));
    std::string first;
    if ((words >> first) && (first == "input" || first == "radio")) {
      return true;
    }
  }
  return false;
}

std::string lower_name(const char* s) {
  std::string out;
  for (; *s != '\0'; ++s) {
    const char c = *s;
    out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : (c == ' ' ? '_' : c);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario;
  std::string out = ".";
  std::string wav;
  std::string firmware_dir;
  std::string card_dir;
  bool midi = false;
  bool audio_in = false;
  long ms = 500;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool has_value = i + 1 < argc;
    if (std::strcmp(a, "--scenario") == 0 && has_value) {
      scenario = argv[++i];
    } else if (std::strcmp(a, "--out") == 0 && has_value) {
      out = argv[++i];
    } else if (std::strcmp(a, "--wav") == 0 && has_value) {
      wav = argv[++i];
    } else if (std::strcmp(a, "--firmware-dir") == 0 && has_value) {
      firmware_dir = argv[++i];
    } else if (std::strcmp(a, "--card") == 0 && has_value) {
      card_dir = argv[++i];
    } else if (std::strcmp(a, "--midi") == 0) {
      midi = true;
    } else if (std::strcmp(a, "--audio-in") == 0) {
      audio_in = true;
    } else if (std::strcmp(a, "--ms") == 0 && has_value) {
      ms = std::strtol(argv[++i], nullptr, 10);
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  o3sdk::app::App& app = o3sdk_app();
  o3sdk::platform::host::FakeFirmwareSink firmware;
  if (!firmware_dir.empty()) {
    std::string error;
    if (!o3sdk::sim::load_firmware_dir(firmware, firmware_dir, error)) {
      std::fprintf(stderr, "--firmware-dir: %s\n", error.c_str());
      return 2;
    }
  }
  std::string scenario_text;
  if (!scenario.empty()) {
    std::ifstream f(scenario);
    if (!f) {
      std::fprintf(stderr, "cannot read %s\n", scenario.c_str());
      return 2;
    }
    std::stringstream text;
    text << f.rdbuf();
    scenario_text = text.str();
  }
  o3sdk::platform::host::FakeMidi fake_midi;
  o3sdk::platform::host::FakeCapture fake_capture;
  std::unique_ptr<o3sdk::platform::host::DirCard> card;
  if (!card_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(card_dir, ec);
    if (!std::filesystem::is_directory(card_dir, ec)) {
      std::fprintf(stderr, "--card: not a directory: %s\n", card_dir.c_str());
      return 2;
    }
    card = std::make_unique<o3sdk::platform::host::DirCard>(card_dir);
  }
  o3sdk::sim::Harness h(app, firmware_dir.empty() ? nullptr : &firmware, (midi || uses_midi(scenario_text)) ? &fake_midi : nullptr, card.get(), nullptr,
                    (audio_in || uses_capture(scenario_text)) ? &fake_capture : nullptr);
  if (!h.start()) {
    // start() has printed the refused setup calls, if there were any (Engine::setup_errors()); the display is the other cause.
    if (h.engine().setup_errors() != 0u) {
      std::fprintf(stderr, "%s: the app's setup had %u refused call(s): fix the wiring in setup() (the return values of attach, set_source, ...)\n",
                   app.info().name, h.engine().setup_errors());
    } else {
      std::fprintf(stderr, "the display refused the UI assets\n");
    }
    return 1;
  }
  if (!wav.empty()) {
    h.record(true);
  }

  int status = 0;
  if (!scenario.empty()) {
    const o3sdk::sim::ScenarioResult r = o3sdk::sim::run_scenario(h, scenario_text, out);
    if (!r.ok) {
      std::fprintf(stderr, "%s:%d: %s\n", scenario.c_str(), r.line, r.error.c_str());
      status = 1;
    }
  } else {
    h.run_ms(static_cast<std::uint32_t>(ms > 0 ? ms : 1));
    h.runtime().draw();
    const std::string path = out + "/" + lower_name(app.info().name) + ".png";
    if (!h.snapshot(path)) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      status = 1;
    } else {
      std::printf("snap %s\n", path.c_str());
    }
  }

  if (!wav.empty()) {
    if (!h.write_wav(wav)) {
      std::fprintf(stderr, "cannot write %s\n", wav.c_str());
      status = 1;
    } else {
      std::printf("wav %s (%zu frames)\n", wav.c_str(), h.audio().size() / 2u);
    }
  }

  if (card != nullptr && card->violations() != 0u) {  // a change the write guard refused: a bug of the files service, never input
    std::fprintf(stderr, "the card's write guard refused %u change(s)\n", card->violations());
    status = 1;
  }

  const o3sdk::dsp::EngineStats s = h.engine().stats();
  std::printf("%s %s: %u frames drawn, %u refused, peak list %zu/2048 words; %u audio blocks, %u events dropped, %u commands dropped\n",
              app.info().name, app.info().version, h.runtime().frames(), h.runtime().frame_failures(),
              h.display().peak_frame_words(), s.blocks, s.dropped_events, h.engine().dropped_commands());
  if (h.runtime().frame_failures() != 0u || h.display().render_failures() != 0u) {
    std::fprintf(stderr, "frames failed: %u refused by the builder, %zu by the raster (%s)\n", h.runtime().frame_failures(),
                 h.display().render_failures(), h.display().first_error().c_str());
    status = 1;
  }
  return status;
}
