# SPDX-License-Identifier: GPL-3.0-or-later
#
# Helpers shared by the kit, the units and the apps, including the ones of a project that uses o3sdk from outside (docs/FRAMEWORK.md,
# "Using o3sdk from another project"). The functions run in the caller's directory: a path that means "inside o3sdk" is O3SDK_ROOT
# (cmake/O3sdkPlatform.cmake), never PROJECT_SOURCE_DIR, and a variable they read must be visible from there (hence the cache).

# Strict warnings for first-party code. PRIVATE on purpose: they never leak into consumers.
function(o3sdk_strict_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wdouble-promotion -Wconversion)
  if(O3SDK_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# The single-precision rule: the compiler treats every unsuffixed floating constant as float, so a stray `x / 3.0` can never
# pull in the software double routines (the Cortex-M4F has a single-precision FPU only).
set(O3SDK_SINGLE_PRECISION_LAYERS core hal eve ui dsp app CACHE INTERNAL "Layers built with -fsingle-precision-constant")

# o3sdk_library(<name> [SOURCES <src>...] [DEPENDS <lib>...])
#
# One library per layer. Public headers live in <dir>/include/o3sdk/<layer>/ and are included as <o3sdk/<layer>/x.h>, so a
# layer sees another layer's headers only through a link dependency.
#   * STATIC when it has sources, INTERFACE (headers only) otherwise.
#   * Every public header is also compiled on its own into <name>_hdrcheck (self-containment).
#   * On mk66 the size of each archive is printed after it is built.
function(o3sdk_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(inc "${CMAKE_CURRENT_SOURCE_DIR}/include")

  if(ARG_SOURCES)
    add_library(${name} STATIC ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME o3sdk_${name})
    target_include_directories(${name} PUBLIC ${inc})
    target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    o3sdk_strict_warnings(${name})
    if(name IN_LIST O3SDK_SINGLE_PRECISION_LAYERS)
      target_compile_options(${name} PRIVATE -fsingle-precision-constant)
    endif()
    if(O3SDK_PLATFORM STREQUAL "mk66")
      add_custom_command(TARGET ${name} POST_BUILD COMMAND ${CMAKE_SIZE} -t $<TARGET_FILE:${name}> VERBATIM)
    endif()
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE ${inc})
    target_link_libraries(${name} INTERFACE ${ARG_DEPENDS})
  endif()

  file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE ${inc} ${inc}/*.h)
  set(stubs "")
  foreach(header IN LISTS headers)
    string(MAKE_C_IDENTIFIER ${header} id)
    set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck/${id}.cpp")
    file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
    list(APPEND stubs ${stub})
  endforeach()
  if(stubs)
    add_library(${name}_hdrcheck OBJECT ${stubs})
    target_link_libraries(${name}_hdrcheck PRIVATE ${name})
    o3sdk_strict_warnings(${name}_hdrcheck)
    if(name IN_LIST O3SDK_SINGLE_PRECISION_LAYERS)
      target_compile_options(${name}_hdrcheck PRIVATE -fsingle-precision-constant)
    endif()
  endif()
endfunction()

# o3sdk_unit(<name> [SOURCES <src>...] [DEPENDS <lib>...])
#
# A unit: DSP only, against kit/dsp (docs/FRAMEWORK.md, "Units"). The library is `unit_<name>`; the header prefix is the caller's own
# `include/` layout: the kit's units use <o3sdk/units/<name>/x.h>, a project's own units its own prefix (<myapp/units/<name>/x.h>,
# templates/app). Units follow the single-precision rule and are built without errno for the maths functions.
function(o3sdk_unit name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(target unit_${name})
  set(inc "${CMAKE_CURRENT_SOURCE_DIR}/include")
  if(ARG_SOURCES)
    add_library(${target} STATIC ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME o3sdk_${target})
    target_include_directories(${target} PUBLIC ${inc})
    target_link_libraries(${target} PUBLIC dsp ${ARG_DEPENDS})
    o3sdk_strict_warnings(${target})
    target_compile_options(${target} PRIVATE -fsingle-precision-constant -fno-math-errno)
    if(O3SDK_PLATFORM STREQUAL "mk66")
      add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_SIZE} -t $<TARGET_FILE:${target}> VERBATIM)
    endif()
  else()
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE ${inc})
    target_link_libraries(${target} INTERFACE dsp ${ARG_DEPENDS})
  endif()
endfunction()

# o3sdk_app(<name> SOURCES <src>... [DEPENDS <lib>...])
#
# An app: one firmware image (docs/FRAMEWORK.md, "Apps"). Its sources define `o3sdk::app::App& o3sdk_app() noexcept`. Builds:
#   * the library `app_<name>` (the app and its devices), on every platform;
#   * host: the executable `<name>_sim`, the headless simulator running this app (sim/README.md);
#   * mk66: the firmware image `<name>.elf` and, behind the static checks, `<name>.hex`, `<name>.bin` and `<name>.ptf`, with their
#     ctests (o3sdk_mk66_image, platform/mk66/CMakeLists.txt; platform/mk66/README.md).
function(o3sdk_app name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(target app_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME o3sdk_${target})
  target_link_libraries(${target} PUBLIC app ${ARG_DEPENDS})
  o3sdk_strict_warnings(${target})
  target_compile_options(${target} PRIVATE -fsingle-precision-constant)
  if(NOT O3SDK_CROSS)
    add_executable(${name}_sim ${O3SDK_ROOT}/sim/src/sim_main.cpp)
    # Whole archive: o3sdk_app() is the app library's entry point and nothing else in the simulator names it first.
    target_link_libraries(${name}_sim PRIVATE sim "$<LINK_LIBRARY:WHOLE_ARCHIVE,${target}>")
    o3sdk_strict_warnings(${name}_sim)
  endif()
  if(O3SDK_PLATFORM STREQUAL "mk66")
    o3sdk_mk66_image(${name} ${target})
  endif()
endfunction()

# o3sdk_test_suite(<exe> SOURCES <*_test.cpp>... [DEPENDS <lib>...]) -- host only
#
# The doctest tests of a project that uses o3sdk: one executable (doctest's main from o3sdk/tests/main.cpp, the vendored header as
# <doctest/doctest.h>, o3sdk's tests/support helpers), one ctest per source file (`ctest -R <file name without .cpp>`), strict
# warnings, no exceptions (REQUIRE ends the run with std::terminate, which ctest reports). A test that names the app's entry point
# o3sdk_app() links the app library whole: DEPENDS "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_<name>>".
function(o3sdk_test_suite exe)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  add_executable(${exe} ${O3SDK_ROOT}/tests/main.cpp ${ARG_SOURCES})
  target_include_directories(${exe} SYSTEM PRIVATE ${O3SDK_ROOT}/third_party)
  target_include_directories(${exe} PRIVATE ${O3SDK_ROOT}/tests)  # "support/need.h", "support/rng.h"
  target_link_libraries(${exe} PRIVATE ${ARG_DEPENDS})
  o3sdk_strict_warnings(${exe})
  target_compile_definitions(${exe} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
  foreach(src IN LISTS ARG_SOURCES)
    get_filename_component(test ${src} NAME_WE)
    add_test(NAME ${test} COMMAND ${exe} --source-file=*${test}.cpp)
  endforeach()
endfunction()
