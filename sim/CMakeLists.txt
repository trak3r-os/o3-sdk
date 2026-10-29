# SPDX-License-Identifier: GPL-3.0-or-later
# sim: runs any o3sdk app on the host, headless (sim/README.md). The FT81x display-list interpreter (EveRaster, from track3r-os),
# the HAL fakes, scripted input, PNG snapshots of the screen and the pads, and the audio as a WAV file. Standard library allowed. The record service of platform/mk66/audio (and
# the usb kernels its tap uses) is linked: Context::recorder() is the unit's code over the fake codec (docs/M2C-PLAN.md D8).
o3sdk_library(sim SOURCES src/eve_raster.cpp src/raster_display.cpp src/harness.cpp src/scenario.cpp
  DEPENDS app platform_host platform_mk66_audio usb)
