# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/input: buttons, pads, encoder and the pad LEDs. The pure parts (the key map, the TCA8418 protocol, the debounce, the
# detent accumulator, the scan, the IS31FL3731 driver) are `platform_mk66_input`, tested on the host (tests/mk66_input_*_test.cpp);
# the board's registers (src/device.cpp) join the image (platform/mk66/CMakeLists.txt).
o3sdk_library(platform_mk66_input SOURCES src/tca8418.cpp src/scanner.cpp src/pad_leds.cpp DEPENDS platform_mk66_base)
if(O3SDK_PLATFORM STREQUAL "mk66")
  add_library(platform_mk66_input_hw OBJECT src/device.cpp)
  target_link_libraries(platform_mk66_input_hw PUBLIC platform_mk66_input)
  o3sdk_strict_warnings(platform_mk66_input_hw)
endif()
