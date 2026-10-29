# SPDX-License-Identifier: GPL-3.0-or-later
#
# GNU Arm Embedded (arm-none-eabi) toolchain file for the Cortex-M4F targets (MK66 and the QEMU mps2-an386 test target).
#
# Where the compiler is found, first match wins:
#   1. the CMake variable ARM_GNU_ROOT (-DARM_GNU_ROOT=/path/to/arm-gnu-toolchain-...; the directory that holds bin/),
#   2. the environment variable ARM_GNU_ROOT, then O3_ARM_TOOLCHAIN (same meaning),
#   3. .toolchains/arm-gnu-toolchain-15.3.rel1-*-arm-none-eabi in the project being built, then in o3sdk (tools/fetch_toolchain.py puts it
#      there; the directory is git-ignored),
#   4. arm-none-eabi-gcc on PATH.
# No path of a particular machine is written in this tree. CLAUDE.md ("Build and test") shows the cross build and
# platform/mk66/README.md names the Arm GNU Toolchain release (15.3.rel1) that the project is built and tested with.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR cortex-m4)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# try_compile() runs this file again in a scratch project: hand it the root variable.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ARM_GNU_ROOT)

if(NOT ARM_GNU_ROOT)
  if(DEFINED ENV{ARM_GNU_ROOT} AND NOT "$ENV{ARM_GNU_ROOT}" STREQUAL "")
    set(ARM_GNU_ROOT "$ENV{ARM_GNU_ROOT}")
  elseif(DEFINED ENV{O3_ARM_TOOLCHAIN} AND NOT "$ENV{O3_ARM_TOOLCHAIN}" STREQUAL "")
    set(ARM_GNU_ROOT "$ENV{O3_ARM_TOOLCHAIN}")
  endif()
endif()
if(NOT ARM_GNU_ROOT)
  foreach(_base IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_LIST_DIR}/..")
    file(GLOB _found "${_base}/.toolchains/arm-gnu-toolchain-15.3.rel1-*-arm-none-eabi")
    foreach(_dir IN LISTS _found)
      if(NOT ARM_GNU_ROOT AND EXISTS "${_dir}/bin/arm-none-eabi-gcc")
        get_filename_component(ARM_GNU_ROOT "${_dir}" ABSOLUTE)
        message(STATUS "arm-none-eabi toolchain: ${ARM_GNU_ROOT} (from .toolchains/)")
      endif()
    endforeach()
  endforeach()
endif()
if(ARM_GNU_ROOT)
  set(_arm_hints HINTS "${ARM_GNU_ROOT}/bin")
  # With a root given, only its bin/ may supply the tools: another arm-none-eabi-gcc on PATH must not win silently.
  set(_arm_find_args NO_DEFAULT_PATH)
else()
  set(_arm_hints)
  set(_arm_find_args)
endif()

set(ARM_GNU_PREFIX "arm-none-eabi" CACHE STRING "GNU Arm tool prefix")
foreach(_tool GCC GXX AR RANLIB OBJCOPY SIZE)
  string(TOLOWER "${_tool}" _name)
  if(_tool STREQUAL "GXX")
    set(_name "g++")
  endif()
  find_program(ARM_${_tool} NAMES "${ARM_GNU_PREFIX}-${_name}" ${_arm_hints} ${_arm_find_args})
  if(NOT ARM_${_tool})
    message(FATAL_ERROR "${ARM_GNU_PREFIX}-${_name} not found. Put the Arm GNU Toolchain on PATH, or pass -DARM_GNU_ROOT=<toolchain dir> "
                        "(or export ARM_GNU_ROOT / O3_ARM_TOOLCHAIN); see CLAUDE.md, \"Build and test\", and platform/mk66/README.md.")
  endif()
endforeach()
set(ARM_AS "${ARM_GCC}")

set(CMAKE_C_COMPILER "${ARM_GCC}")
set(CMAKE_CXX_COMPILER "${ARM_GXX}")
set(CMAKE_ASM_COMPILER "${ARM_AS}")
set(CMAKE_AR "${ARM_AR}" CACHE FILEPATH "GNU Arm archiver")
set(CMAKE_RANLIB "${ARM_RANLIB}" CACHE FILEPATH "GNU Arm ranlib")
set(CMAKE_OBJCOPY "${ARM_OBJCOPY}" CACHE FILEPATH "GNU Arm objcopy")
set(CMAKE_SIZE "${ARM_SIZE}" CACHE FILEPATH "GNU Arm size")

# $<LINK_LIBRARY:WHOLE_ARCHIVE,...> (o3sdk_app links the app library whole into its image): CMake defines the feature for its
# hosted platforms only, not for a Generic system, so it is spelled here for GNU ld.
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)

set(ARM_CPU_FLAGS "-mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${ARM_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${ARM_CPU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${ARM_CPU_FLAGS}")
