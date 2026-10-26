# SPDX-License-Identifier: GPL-3.0-or-later
# app: the app framework. The Tracker's panel, inputs, screens, the pad LEDs, the F-key + encoder convention, shared widgets and the
# runtime (the main loop every platform runs for an app) and the system Config screen it owns (FIRMWARE, DISPLAY, INFO). Depends on ui
# (and through it eve, hal, core) and dsp. See docs/FRAMEWORK.md.
o3sdk_library(app SOURCES src/panel.cpp src/param_keys.cpp src/widgets.cpp src/runtime.cpp src/firmware_flow.cpp src/system_config.cpp
  src/system_firmware_page.cpp DEPENDS ui dsp)
