# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/power (docs/M2B2-PLAN.md, slice P; OS-POWER P1 and P2, ported from track3r-os): the shutdown that a hold of the Power switch starts (power_service.h) as a pure state machine over a
# PowerHost, so the host tests run the very code the image runs (tests/mk66_power_service_test.cpp). OS-POWER P2 adds the VLPR off's sequence and poll loop over an OffPlatform (power_off.h,
# tests/mk66_power_off_test.cpp) and the record of the last shutdown (power_record.h). That is `platform_mk66_power`, pure and tested on the host. Only src/power_host_mk66.cpp (the image's PowerHost
# and OffPlatform: the stop of the engine and the audio, the card, USB, the lights, the restart or the VLPR off, the registers) is the image's: `platform_mk66_power_hw`, which joins the image with the
# console (platform/mk66/CMakeLists.txt). It names appimg:: (the composition root's hooks: console/src/appimg.cpp), console::log and usb0::, which are the image's to define.
o3sdk_library(platform_mk66_power
  SOURCES src/power_service.cpp src/power_report.cpp src/power_off.cpp
  DEPENDS core platform_mk66_base platform_mk66_audio)  # platform_mk66_audio: audio::diag::Text, the bounded text builder (no printf)
# The single-precision rule (cmake/O3sdkTargets.cmake): no double in an image.
target_compile_options(platform_mk66_power PRIVATE -fsingle-precision-constant)

if(O3SDK_PLATFORM STREQUAL "mk66" AND O3SDK_MK66_CONSOLE)
  # src/internal.h (boot::, appimg::) and src/regs.h are private to the platform's sources: the file names them relative to itself. THE ONLY SOURCE THAT NAMES board::kPowerSwitch's pull and reads PTC3.
  add_library(platform_mk66_power_hw OBJECT src/power_host_mk66.cpp)
  target_link_libraries(platform_mk66_power_hw PUBLIC platform_mk66_power platform_mk66_audio platform_mk66_display platform_mk66_input platform_mk66_storage platform_mk66_usb)
  target_compile_options(platform_mk66_power_hw PRIVATE -fsingle-precision-constant)
  o3sdk_strict_warnings(platform_mk66_power_hw)
endif()
