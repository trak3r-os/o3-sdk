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

# o3sdk_app(<name> SOURCES <src>... [DEPENDS <lib>...] [LOGO <png> [LOGO_COLOR 0xRRGGBB]])
#
# An app: one firmware image (docs/FRAMEWORK.md, "Apps"). Its sources define `o3sdk::app::App& o3sdk_app() noexcept`.
# LOGO: the boot logo of this app's image (docs/FRAMEWORK.md, "The boot logo"). <png> (a path relative to the calling directory, or
# absolute) is converted by tools/logo_convert.py into the app's build directory at build time and its bitmap is linked in place of the kit's
# default (the O3 logo); without LOGO the app has the default. A grey picture is drawn white, or in LOGO_COLOR; a picture with colour in
# its own colours (a 256-colour palette). A logo that does not fit (ui/boot_logo.h: 800 x 420 drawn, 64 KiB of bitmap and palette) fails
# the build with the converter's message. Builds:
#   * the library `app_<name>` (the app and its devices), on every platform;
#   * host: the executable `<name>_sim`, the headless simulator running this app (sim/README.md);
#   * mk66: the firmware image `<name>.elf` and, behind the static checks, `<name>.hex`, `<name>.bin` and `<name>.ptf`, with their
#     ctests (o3sdk_mk66_image, platform/mk66/CMakeLists.txt; platform/mk66/README.md).
function(o3sdk_app name)
  cmake_parse_arguments(ARG "" "LOGO;LOGO_COLOR" "SOURCES;DEPENDS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "o3sdk_app(${name}): unknown argument(s) '${ARG_UNPARSED_ARGUMENTS}' (SOURCES, DEPENDS, LOGO, LOGO_COLOR)")
  endif()
  set(target app_${name})
  add_library(${target} STATIC ${ARG_SOURCES})
  if(ARG_LOGO_COLOR AND NOT ARG_LOGO)
    message(FATAL_ERROR "o3sdk_app(${name}): LOGO_COLOR needs LOGO")
  endif()
  if(ARG_LOGO)
    get_filename_component(logo_png "${ARG_LOGO}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${logo_png}")
      message(FATAL_ERROR "o3sdk_app(${name}): LOGO '${ARG_LOGO}' does not exist (${logo_png})")
    endif()
    set(logo_tint "")
    if(ARG_LOGO_COLOR)
      if(NOT ARG_LOGO_COLOR MATCHES "^0[xX][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]$")
        message(FATAL_ERROR "o3sdk_app(${name}): LOGO_COLOR '${ARG_LOGO_COLOR}' is not 0xRRGGBB")
      endif()
      set(logo_tint --tint ${ARG_LOGO_COLOR})
    endif()
    # The generated file defines boot_logo() strongly; the kit's default is weak, so the linker takes this one (ui/boot_logo.h).
    set(logo_cpp "${CMAKE_CURRENT_BINARY_DIR}/${name}_boot_logo.cpp")
    add_custom_command(
      OUTPUT "${logo_cpp}"
      COMMAND ${O3SDK_PYTHON} -I "${O3SDK_ROOT}/tools/logo_convert.py" --png "${logo_png}" --out "${logo_cpp}" ${logo_tint}
      DEPENDS "${logo_png}" "${O3SDK_ROOT}/tools/logo_convert.py"
      COMMENT "Converting the boot logo of ${name}: ${ARG_LOGO}"
      VERBATIM)
    target_sources(${target} PRIVATE "${logo_cpp}")
  endif()
  # The app's id, for its folder on the card (Apps/<id>/, app/files.h, docs/M2C-PLAN.md D6): the CMake id as given, generated into a source that defines o3sdk_app_id() strongly (the
  # kit's default is weak and has none). At most 15 bytes of letters, digits, '_', '-' and ' ', starting with a letter or digit: the card's guard (hal/app_guard.h) takes no other.
  if(NOT name MATCHES "^[A-Za-z0-9][A-Za-z0-9_ -]*$" OR name MATCHES ".{16,}")
    message(FATAL_ERROR "o3sdk_app(${name}): the id is the app's folder on the SD card (Apps/<id>/): at most 15 characters of letters, digits, '_', '-' and ' ', starting with a letter or digit")
  endif()
  set(id_cpp "${CMAKE_CURRENT_BINARY_DIR}/${name}_app_id.cpp")
  file(GENERATE OUTPUT "${id_cpp}" CONTENT "// Generated by o3sdk_app(${name}): the app's id, its folder on the card (app/app.h).\n#include <o3sdk/app/app.h>\nconst char* o3sdk_app_id() noexcept { return \"${name}\"; }\n")
  target_sources(${target} PRIVATE "${id_cpp}")
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

# o3sdk_app_test_suite(<app-id> SOURCES <*_test.cpp>... [DEPENDS <lib>...] [EXE <name>] [SCENARIO_DIR <dir>] [SCENARIO_DEFINE <NAME>])
#   -- host only; the app must already be defined with o3sdk_app(<app-id> ...) (add_subdirectory(app) before add_subdirectory(tests))
#
# The tests of an app repository: o3sdk_test_suite() with the wiring an app's tests need, spelled once.
#   * the executable is `<app-id>_tests` (EXE renames it) and links `app`, `sim`, `platform_host`, DEPENDS (the units under test, say) and
#     the app library `app_<app-id>` WHOLE, because that library defines o3sdk_app() and nothing else in the test names it first;
#   * SCENARIO_DIR (default: ../app/scenarios next to the tests directory, when it exists) is handed to the sources as the string macro
#     SCENARIO_DEFINE (default APP_SCENARIO_DIR), so a test can run every scenario of the app as it is (templates/app/tests/app_test.cpp).
# One ctest per source file, as o3sdk_test_suite(). The explicit form (o3sdk_test_suite with the whole-archive item in DEPENDS) is still
# fine, and what o3-303 uses.
function(o3sdk_app_test_suite app_id)
  cmake_parse_arguments(ARG "" "EXE;SCENARIO_DIR;SCENARIO_DEFINE" "SOURCES;DEPENDS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "o3sdk_app_test_suite(${app_id}): unknown argument(s) '${ARG_UNPARSED_ARGUMENTS}' "
                        "(SOURCES, DEPENDS, EXE, SCENARIO_DIR, SCENARIO_DEFINE)")
  endif()
  if(NOT TARGET app_${app_id})
    message(FATAL_ERROR "o3sdk_app_test_suite(${app_id}): there is no app library app_${app_id}: call o3sdk_app(${app_id} ...) first, "
                        "that is add_subdirectory(app) before add_subdirectory(tests)")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "o3sdk_app_test_suite(${app_id}): no SOURCES")
  endif()
  if(NOT ARG_EXE)
    set(ARG_EXE ${app_id}_tests)
  endif()
  if(NOT ARG_SCENARIO_DEFINE)
    set(ARG_SCENARIO_DEFINE APP_SCENARIO_DIR)
  endif()
  if(ARG_SCENARIO_DIR)
    get_filename_component(scenario_dir "${ARG_SCENARIO_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT IS_DIRECTORY "${scenario_dir}")
      message(FATAL_ERROR "o3sdk_app_test_suite(${app_id}): SCENARIO_DIR '${ARG_SCENARIO_DIR}' is not a directory")
    endif()
  else()
    get_filename_component(scenario_dir "${CMAKE_CURRENT_SOURCE_DIR}/../app/scenarios" ABSOLUTE)
  endif()
  o3sdk_test_suite(${ARG_EXE}
    SOURCES ${ARG_SOURCES}
    DEPENDS app sim platform_host ${ARG_DEPENDS} "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_${app_id}>")
  if(IS_DIRECTORY "${scenario_dir}")
    set(scenario_macro "${ARG_SCENARIO_DEFINE}=\"${scenario_dir}\"")
    target_compile_definitions(${ARG_EXE} PRIVATE "${scenario_macro}")
  endif()
endfunction()
