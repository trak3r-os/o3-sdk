// SPDX-License-Identifier: GPL-3.0-or-later
//
// <app>_sim: runs the app this executable is linked with (o3sdk_app(), cmake/O3sdkTargets.cmake) headless on the host.
//
//   <app>_sim [--scenario FILE] [--out DIR] [--wav FILE] [--ms N] [--firmware-dir DIR]
//
// Without a scenario it runs N milliseconds (default 500) and writes DIR/<app>.png. --wav records the whole run. Exit 1 when the
// scenario fails, when a frame did not fit the display list or the raster refused one, or when the app's setup had a call the engine
// refused (an attach, a route or a hook that was not taken: Engine::setup_errors(); the refusals are listed on stderr), so a script can
// gate on it. --firmware-dir gives the system Config screen a fake update path over the *.ptf files of DIR (load_firmware_dir: nothing is
// read or written, no flash exists); without it the screen has no FIRMWARE section.
#include <o3sdk/app/app.h>
#include <o3sdk/sim/harness.h>
#include <o3sdk/sim/scenario.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

void usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s [--scenario FILE] [--out DIR] [--wav FILE] [--ms N] [--firmware-dir DIR]\n", argv0);
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
  o3sdk::sim::Harness h(app, firmware_dir.empty() ? nullptr : &firmware);
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
    std::ifstream f(scenario);
    if (!f) {
      std::fprintf(stderr, "cannot read %s\n", scenario.c_str());
      return 2;
    }
    std::stringstream text;
    text << f.rdbuf();
    const o3sdk::sim::ScenarioResult r = o3sdk::sim::run_scenario(h, text.str(), out);
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
