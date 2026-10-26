# SPDX-License-Identifier: GPL-3.0-or-later
#
# @APP_NAME@: an app on the o3-sdk framework. It is one firmware image for the Tracker hardware; the framework is the sibling
# repository ../o3-sdk, used as it is (add_subdirectory) and never edited from here (CLAUDE.md, AGENTS.md).
#
#   host:  cmake -S . -B build-host && cmake --build build-host -j4 && ctest --test-dir build-host --output-on-failure
#          build-host/app/@APP_ID@_sim --scenario app/scenarios/boot.txt --out <dir>      (mkdir -p <dir> first)
#   mk66:  cmake -S . -B build-mk66 -DO3SDK_PLATFORM=mk66 -DARM_GNU_ROOT=<arm-gnu-toolchain-15.3.rel1 dir>
#          cmake --build build-mk66 -j4 && ctest --test-dir build-mk66 -L mk66      (build-mk66/app/@APP_ID@.ptf)
#
# The framework is somewhere else: -DO3SDK_DIR=<path>, or the environment variable O3SDK_DIR (tools/ci.sh reads the same one).
cmake_minimum_required(VERSION 3.24)

if(DEFINED ENV{O3SDK_DIR} AND NOT "$ENV{O3SDK_DIR}" STREQUAL "")
  set(_o3sdk_default "$ENV{O3SDK_DIR}")
else()
  set(_o3sdk_default "${CMAKE_CURRENT_SOURCE_DIR}/../o3-sdk")
endif()
set(O3SDK_DIR "${_o3sdk_default}" CACHE PATH "The o3-sdk framework (a checkout of the o3-sdk repository)")
if(NOT EXISTS "${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")
  message(FATAL_ERROR "O3SDK_DIR='${O3SDK_DIR}' is not an o3-sdk checkout (cmake/O3sdkPlatform.cmake not found): clone o3-sdk next "
                      "to this directory or pass -DO3SDK_DIR=<path>")
endif()

# The platform (host or mk66), the toolchain and the compile settings every o3-sdk target shares: this must come before project().
include("${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")

project(@APP_ID@ VERSION 0.1.0 LANGUAGES C CXX)

enable_testing()

# The kit, o3-sdk's units (synth, delay), the platform and, on the host, the simulator. o3-sdk's own apps and tests are not built from
# here (O3SDK_BUILD_APPS and O3SDK_BUILD_TESTS default to OFF when o3-sdk is not the top-level project).
add_subdirectory(${O3SDK_DIR} o3sdk)

add_subdirectory(units)
add_subdirectory(app)
if(NOT O3SDK_CROSS)
  add_subdirectory(tests)
endif()
