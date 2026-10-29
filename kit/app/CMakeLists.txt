# SPDX-License-Identifier: GPL-3.0-or-later
# app: the app framework. The Tracker's panel, inputs, screens, the pad LEDs, the F-key + encoder convention, shared widgets and the
# runtime (the main loop every platform runs for an app) and the system Config screen it owns (FIRMWARE, MIDI, DISPLAY, INFO), and the app's
# MIDI (routes of notes to units, the settings; the engine follows the clock through dsp), and the save machinery of docs/M2C-PLAN.md slice FS (the atomic
# save job, the sliced folder listing and the text entry's state machine). Depends on ui (and through it eve, hal, core), dsp and formats. See docs/FRAMEWORK.md.
o3sdk_library(app SOURCES src/panel.cpp src/param_keys.cpp src/widgets.cpp src/runtime.cpp src/firmware_flow.cpp src/system_config.cpp
  src/system_firmware_page.cpp src/system_midi_page.cpp src/midi.cpp
  src/atomic_save.cpp src/og_timing.cpp src/dir_index.cpp src/dir_scan.cpp src/text_entry.cpp DEPENDS ui dsp formats)
