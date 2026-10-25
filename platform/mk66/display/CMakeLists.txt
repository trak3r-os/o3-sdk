# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/display: the FT812 display driver. The pure parts (register facts, panel timing, SPI rates, the chip bring-up, the
# hal::Display with its staging and swap logic) are `platform_mk66_display`, tested on the host (tests/mk66_display_*_test.cpp); the
# DSPI2 / eDMA code that touches the hardware (src/device.cpp) joins the image (platform/mk66/CMakeLists.txt).
o3sdk_library(platform_mk66_display SOURCES src/chip.cpp src/display.cpp DEPENDS platform_mk66_base)
# The panel orientation of the build, REG_ROTATE 0..7 (0 landscape, 1 inverted landscape). 1 was proven upright on the maintainer's unit
# with this driver (track3r-os OS-HW1); 0 shows it upside down. A compile definition of the library itself, PUBLIC, so that every
# translation unit that includes display/device.h (the _hw object, the image's main, the tests) sees the same kBuildOrientation.
set(O3SDK_DISPLAY_ROTATE "1" CACHE STRING "Image: panel orientation, REG_ROTATE 0..7 (0 landscape, 1 inverted). 1 is upright on the maintainer's unit (OS-HW1); 0 shows it upside down")
if(NOT O3SDK_DISPLAY_ROTATE MATCHES "^[0-7]$")
  message(FATAL_ERROR "O3SDK_DISPLAY_ROTATE must be 0..7 (REG_ROTATE), got '${O3SDK_DISPLAY_ROTATE}'")
endif()
target_compile_definitions(platform_mk66_display PUBLIC O3SDK_DISPLAY_ROTATE=${O3SDK_DISPLAY_ROTATE})
if(O3SDK_PLATFORM STREQUAL "mk66")
  add_library(platform_mk66_display_hw OBJECT src/device.cpp)
  target_link_libraries(platform_mk66_display_hw PUBLIC platform_mk66_display)
  o3sdk_strict_warnings(platform_mk66_display_hw)
endif()
