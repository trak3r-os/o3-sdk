# SPDX-License-Identifier: GPL-3.0-or-later
# app: the app framework. The Tracker's panel, inputs, screens, the pad LEDs, the F-key + encoder convention, shared widgets and the
# runtime (the main loop every platform runs for an app) and the system Config screen it owns (FIRMWARE, AUDIO, MIDI, DISPLAY, INFO), and the app's
# MIDI (routes of notes to units, the settings; the engine follows the clock through dsp), audio in and the FM radio (audio_in.h, radio.h, the AUDIO section: slice AI of M2c), and the save machinery of docs/M2C-PLAN.md slice FS (the atomic
# save job, the sliced folder listing and the text entry's state machine) and the app file API of slice FA (files.h: save, load and list in Apps/<app>/ as jobs of the main loop, and the
# kit's own settings kept on the card), and recording (recorder.h: Context::recorder(), slice RC), and the app's bitmaps in RAM_G (app_assets.h: App::assets(), uploaded by Runtime::start()). Depends on ui (and through it eve, hal, core), dsp and formats. See docs/FRAMEWORK.md.
o3sdk_library(app SOURCES src/panel.cpp src/param_keys.cpp src/widgets.cpp src/runtime.cpp src/firmware_flow.cpp src/system_config.cpp
  src/system_firmware_page.cpp src/system_midi_page.cpp src/system_audio_page.cpp src/system_card_page.cpp src/usb_audio.cpp src/midi.cpp src/audio_in.cpp src/radio.cpp
  src/config_page.cpp src/file_picker.cpp src/name_entry.cpp src/atomic_save.cpp src/og_timing.cpp src/dir_index.cpp src/dir_scan.cpp src/text_entry.cpp
  src/settings.cpp src/save_sources.cpp src/files.cpp src/recorder.cpp src/app_assets.cpp DEPENDS ui dsp formats)
