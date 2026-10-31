# SPDX-License-Identifier: GPL-3.0-or-later
# ui: immediate-mode renderer on eve, the colour tokens and themes, the faces and bitmap assets, the components.
# Depends on eve (and through it hal).
#
# The face and icon data are generated at build time into the build tree and compiled into this library; they are never
# committed. Faces: tools/font_convert.py turns the vendored BDF files into the four glyph faces (Terminus 16x32 and
# 10x20, SIL OFL 1.1; Spleen 8x16 and 6x12, BSD-2-Clause); icons: tools/icon_convert.py draws the in-house icon face.
# Both emit word-aligned bitmaps with a static_assert (ADR 0011). Tiles are not generated here: they are composed
# from the Data face at boot (src/assets.cpp).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_spleen_dir "${PROJECT_SOURCE_DIR}/third_party/fonts/spleen")
set(_terminus_dir "${PROJECT_SOURCE_DIR}/third_party/fonts/terminus")
set(_font_display "${_terminus_dir}/ter-u32b.bdf")
set(_font_data "${_terminus_dir}/ter-u20b.bdf")
set(_font_label "${_spleen_dir}/spleen-8x16.bdf")
set(_font_micro "${_spleen_dir}/spleen-6x12.bdf")
set(_font_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/font_data.cpp")
set(_icon_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/icon_data.cpp")

add_custom_command(
  OUTPUT "${_font_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/font_convert.py" --out "${_font_gen}"
          --face "f16x32=${_font_display}" --face "f10x20=${_font_data}"
          --face "f8x16=${_font_label}" --face "f6x12=${_font_micro}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/font_convert.py"
          "${_font_display}" "${_font_data}" "${_font_label}" "${_font_micro}"
  COMMENT "Converting the BDF faces to FT81x L1 font data"
  VERBATIM)

add_custom_command(
  OUTPUT "${_icon_gen}"
  COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/icon_convert.py" --out "${_icon_gen}"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/icon_convert.py"
  COMMENT "Converting the in-house icons to FT81x L1 data"
  VERBATIM)

o3sdk_library(ui
  SOURCES src/renderer.cpp src/assets.cpp src/themes.cpp src/components.cpp "${_font_gen}" "${_icon_gen}"
          src/boot_screen.cpp src/boot_logo_data.cpp src/list_view.cpp src/keyboard.cpp
  DEPENDS eve)

# OS-003f, the boot logo: logo/o3.png (at the root of o3-sdk, the maintainer's O3 logo: the one source, no copy) is the kit's default;
# tools/logo_convert.py turns it into src/boot_logo_data.cpp, which IS committed (unlike the font and icon data above, which are
# generated into the build tree): the converter takes seconds and the picture changes about never, so a build does not run it. Its
# boot_logo() is WEAK (--weak): an app that gives its own picture, o3sdk_app(... LOGO <png>), links a generated file with the strong
# definition and the default drops out. The ctest `logo_convert_fresh` (host tree) regenerates the default in memory and fails when
# the committed file differs; regenerate with
#   python3 -I tools/logo_convert.py --png logo/o3.png --weak --out kit/ui/src/boot_logo_data.cpp

# ---- ui::plot (OS-003d): part of ui, sources and headers under plot/ (ADR 0010 amendment 2) ----
# Peak-cache mipmap, wave view (vector and bitmap), curves, XY scope, spectrum, meters, the RAM_G map, the app's Canvas (canvas.h). They draw through
# ui::Renderer's raw sections and are built into the same library, so the MK66 `ui` size includes them.
target_sources(ui PRIVATE
  plot/src/peak_cache.cpp plot/src/plot_math.cpp plot/src/wave_bitmap.cpp plot/src/wave_view.cpp plot/src/curve.cpp
  plot/src/fft.cpp plot/src/spectrum.cpp plot/src/scope.cpp plot/src/meters.cpp plot/src/canvas.cpp)
target_include_directories(ui PUBLIC plot/include)
file(GLOB_RECURSE _plot_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/plot/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/plot/include/*.h")
set(_plot_stubs "")
foreach(header IN LISTS _plot_headers)
  string(MAKE_C_IDENTIFIER ${header} id)
  set(stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_plot/${id}.cpp")
  file(CONFIGURE OUTPUT ${stub} CONTENT "#include <@header@>\n" @ONLY)
  list(APPEND _plot_stubs ${stub})
endforeach()
if(_plot_stubs)  # every plot header compiles on its own, like the rest of ui's
  add_library(ui_plot_hdrcheck OBJECT ${_plot_stubs})
  target_link_libraries(ui_plot_hdrcheck PRIVATE ui)
  o3sdk_strict_warnings(ui_plot_hdrcheck)
  target_compile_options(ui_plot_hdrcheck PRIVATE -fsingle-precision-constant)
endif()

if(O3SDK_PLATFORM STREQUAL "host" AND O3SDK_BUILD_TESTS)
  enable_testing()
  add_test(NAME font_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/font_convert.py" --self-test
            --spleen-dir "${_spleen_dir}" --terminus-dir "${_terminus_dir}")
  add_test(NAME icon_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/icon_convert.py" --self-test)
  # OS-003f: the logo converter (PNG decoder on every filter and colour type, grid detection, quantisation, the palette, the output) and
  # the staleness check of the committed boot_logo_data.cpp against logo/o3.png.
  add_test(NAME logo_convert_selftest
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/logo_convert.py" --self-test)
  add_test(NAME logo_convert_fresh
    COMMAND ${Python3_EXECUTABLE} -I "${PROJECT_SOURCE_DIR}/tools/logo_convert.py"
            --png "${PROJECT_SOURCE_DIR}/logo/o3.png" --weak --check "${CMAKE_CURRENT_SOURCE_DIR}/src/boot_logo_data.cpp")
  set_tests_properties(logo_convert_selftest logo_convert_fresh PROPERTIES TIMEOUT 120)
endif()

