# SPDX-License-Identifier: GPL-3.0-or-later
# app: the app framework. The Tracker's panel, inputs, screens, the pad LEDs, the F-key + encoder convention, shared widgets and the
# runtime (the main loop every platform runs for an app). Depends on ui (and through it eve, hal, core) and dsp. See
# docs/FRAMEWORK.md.
o3sdk_library(app SOURCES src/panel.cpp src/param_keys.cpp src/widgets.cpp src/runtime.cpp DEPENDS ui dsp)
