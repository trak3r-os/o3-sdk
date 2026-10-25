# SPDX-License-Identifier: GPL-3.0-or-later
#
# The platform, the toolchain and the build-wide compile settings of an o3sdk build. Everything that has to be decided before
# project() lives here, so o3sdk's own CMakeLists.txt and a project that uses o3sdk from outside (docs/FRAMEWORK.md, "Using o3sdk
# from another project") start the same way:
#
#   cmake_minimum_required(VERSION 3.24)
#   set(O3SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../o3-sdk" CACHE PATH "The o3sdk framework")
#   include("${O3SDK_DIR}/cmake/O3sdkPlatform.cmake")       # BEFORE project()
#   project(myapp LANGUAGES C CXX)
#   enable_testing()
#   add_subdirectory(${O3SDK_DIR} o3sdk)
#
# Variables it defines:
#   O3SDK_PLATFORM  (cache) host or mk66, from -DO3SDK_PLATFORM=...
#   O3SDK_CROSS     TRUE when the platform is a cross build (mk66); the toolchain file is chosen here
#   O3SDK_ROOT      (cache, internal) the absolute path of the o3sdk directory, for every path that means "inside o3sdk"
# The compile settings are directory-scoped (add_compile_options): they reach every target of the project that includes this
# file and of the subdirectories it adds, which is what keeps the kit and an outside app compiled alike (the RTTI and exception
# switches, the audio clock define that changes core's headers).
include_guard(GLOBAL)

if(PROJECT_NAME)  # not CMAKE_PROJECT_NAME: that one is a cache entry and survives into the next configure
  message(FATAL_ERROR "cmake/O3sdkPlatform.cmake must be included before project(): the toolchain and the compile settings of the "
                      "whole build are decided here (docs/FRAMEWORK.md, \"Using o3sdk from another project\")")
endif()

get_filename_component(_o3sdk_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(O3SDK_ROOT "${_o3sdk_root}" CACHE INTERNAL "The o3sdk directory (cmake/O3sdkPlatform.cmake)" FORCE)

# ---- Platform and toolchain (must precede project()) --------------------------------------------------------------------------
set(O3SDK_PLATFORM "host" CACHE STRING "Target platform: host or mk66")
set_property(CACHE O3SDK_PLATFORM PROPERTY STRINGS host mk66)
if(NOT O3SDK_PLATFORM MATCHES "^(host|mk66)$")
  message(FATAL_ERROR "O3SDK_PLATFORM must be 'host' or 'mk66' (got '${O3SDK_PLATFORM}')")
endif()

if(O3SDK_PLATFORM STREQUAL "mk66")
  set(O3SDK_CROSS TRUE)
  if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/mk66-toolchain.cmake")
  endif()
else()
  set(O3SDK_CROSS FALSE)
endif()

# ---- Build-wide settings --------------------------------------------------------------------------------------------------------
option(O3SDK_SANITIZE "Host only: build everything with AddressSanitizer + UBSan" OFF)
option(O3SDK_WERROR "Treat warnings as errors" ON)
# The audio sample clock (core/audio_clock.h): ON is exactly 44,100 Hz, the rate track3r-os ships since v1.0.0.
option(O3SDK_RATE_44100 "Audio clock at exactly 44,100 Hz (1800000/441 cycles per sample); OFF builds 4080/1, 44,117.647 Hz" ON)
if(O3SDK_RATE_44100)
  add_compile_definitions(O3SDK_RATE_44100=1)
endif()

get_property(_o3sdk_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT CMAKE_BUILD_TYPE AND NOT _o3sdk_multi_config)
  if(O3SDK_CROSS)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
  else()
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti> -fno-exceptions)

if(O3SDK_CROSS)
  add_compile_options(-ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
  if(O3SDK_SANITIZE)
    message(FATAL_ERROR "sanitizers are host only")
  endif()
elseif(O3SDK_SANITIZE)
  set(_san -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_compile_options(${_san} -O1)
  add_link_options(${_san})
endif()
