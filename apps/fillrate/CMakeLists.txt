# SPDX-License-Identifier: GPL-3.0-or-later
# FILLRATE: a test image that measures how much the FT812 draws on one scan line before lines break (README.md). No unit, no sound.
o3sdk_app(fillrate SOURCES src/fillrate_app.cpp)

# Host only: the band's arithmetic and the app in the simulator (every kind, K and cell width fits the display list).
if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS)
  o3sdk_test_suite(fillrate_tests SOURCES tests/fillrate_test.cpp
    DEPENDS sim platform_host "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_fillrate>")
  target_include_directories(fillrate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()
