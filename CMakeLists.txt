# SPDX-License-Identifier: GPL-3.0-or-later
#
# o3sdk: a framework for instruments and grooveboxes on the Tracker hardware (MK66FX1M0, FT812, SGTL5000, 48 pads,
# 33 buttons, one encoder). An app built with it is a whole firmware image; see docs/FRAMEWORK.md.
#
#   host:  cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
#          build-host/apps/hello/hello_sim --scenario apps/hello/scenarios/tour.txt --out /tmp/hello
#   mk66:  cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain dir>
#          cmake --build build-mk66 -j4     (the kit, units and apps cross-built for the Cortex-M4F; the image is milestone M2)
#
# From another project (an app that lives in its own repository): include cmake/O3sdkPlatform.cmake before its project(), then
# add_subdirectory(<o3sdk dir> o3sdk); docs/FRAMEWORK.md, "Using o3sdk from another project". Only the kit, the units and the platform
# build then: O3SDK_BUILD_APPS and O3SDK_BUILD_TESTS default to ON only when o3sdk is the top-level project.
#
# Layering (enforced by CMake link dependencies):
#   core <- hal <- eve <- ui <- app;  core <- dsp <- app;  core <- formats <- app;  units depend on dsp only;  an app depends on kit/app and its units.
#   platform/<name> and sim implement the HAL contracts and run an app.
cmake_minimum_required(VERSION 3.24)  # $<LINK_LIBRARY:WHOLE_ARCHIVE,...> (cmake/O3sdkTargets.cmake, the mk66 image) needs 3.24

# The platform, the toolchain and the build-wide compile settings: decided before project(), in a file an outside project
# includes the same way (docs/FRAMEWORK.md, "Using o3sdk from another project").
include(${CMAKE_CURRENT_LIST_DIR}/cmake/O3sdkPlatform.cmake)

project(o3sdk VERSION 0.1.0 LANGUAGES C CXX)

# Used from outside (add_subdirectory), o3sdk builds the kit, its units and its platform; the example apps, the tests and the
# python tools' self-tests are its own and stay off unless asked for.
option(O3SDK_BUILD_APPS "Build o3sdk's own apps (apps/*)" ${PROJECT_IS_TOP_LEVEL})
option(O3SDK_BUILD_TESTS "Build and register o3sdk's own tests (tests/, the platform's pure suites, the tools' self-tests)" ${PROJECT_IS_TOP_LEVEL})

include(${CMAKE_CURRENT_LIST_DIR}/cmake/O3sdkTargets.cmake)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# find_package's result is directory-scoped; the helper functions run in the caller's directory (an app of an outside project).
set(O3SDK_PYTHON "${Python3_EXECUTABLE}" CACHE INTERNAL "The python the build runs its tools with")
enable_testing()

# The kit.
add_subdirectory(kit/core)
add_subdirectory(kit/hal)
add_subdirectory(kit/eve)
add_subdirectory(kit/ui)
add_subdirectory(kit/dsp)
add_subdirectory(kit/formats)  # core <- formats: CRC-32, the resumable WAV reader and the sample probe (app depends on it)
add_subdirectory(kit/app)
add_subdirectory(kit/usb)  # core <- usb: the portable USB device library (descriptors, control pipe, CDC, shell, log, UAC2)

# The MK66 platform: its pure parts on every platform (host tests), the firmware images with -DO3SDK_PLATFORM=mk66.
if(O3SDK_CROSS OR O3SDK_BUILD_TESTS)
  add_subdirectory(platform/mk66)
endif()

# Host-only: the HAL fakes and the simulator that runs any app headless.
if(NOT O3SDK_CROSS)
  add_subdirectory(platform/host)
  add_subdirectory(sim)
endif()

# Units (DSP) and apps. The units are the kit's: an outside project uses them (synth, delay) like any library.
file(GLOB _unit_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/units/*")
foreach(_dir IN LISTS _unit_dirs)
  if(EXISTS "${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()
if(O3SDK_BUILD_APPS)
  file(GLOB _app_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/apps/*")
  foreach(_dir IN LISTS _app_dirs)
    if(EXISTS "${_dir}/CMakeLists.txt")
      add_subdirectory(${_dir})
    endif()
  endforeach()
endif()

if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS)
  add_subdirectory(tests)
endif()
# The app template (templates/app, docs/NEW-APP.md) is instantiated and built like a user would, host and mk66 (label `template`).
if(O3SDK_BUILD_TESTS)
  add_subdirectory(templates)
endif()
