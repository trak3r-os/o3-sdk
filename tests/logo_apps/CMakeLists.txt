# SPDX-License-Identifier: GPL-3.0-or-later
# The boot logo of an app (o3sdk_app's LOGO and LOGO_COLOR, ui/boot_logo.h): two apps built from HELLO's source, one with a colour logo
# (PALETTED565) and one with a grey logo tinted orange, each made by tools/logo_convert.py from a picture the tool makes at configure time
# (--make-test-png). Linking them proves the generated strong boot_logo() replaces the kit's weak default (no duplicate symbol); the ctests
# run each one's boot screen in the simulator and look at the picture (check_logo.py).
set(_pictures ${CMAKE_CURRENT_BINARY_DIR})
foreach(kind grey colour)
  execute_process(
    COMMAND ${O3SDK_PYTHON} -I ${PROJECT_SOURCE_DIR}/tools/logo_convert.py --make-test-png ${_pictures}/${kind}.png --kind ${kind}
    RESULT_VARIABLE _status)
  if(NOT _status EQUAL 0)
    message(FATAL_ERROR "logo_convert.py --make-test-png ${kind} failed (${_status})")
  endif()
endforeach()
set(_hello_source ${PROJECT_SOURCE_DIR}/apps/hello/src/hello_app.cpp)
o3sdk_app(logo_colour SOURCES ${_hello_source} DEPENDS unit_synth unit_delay LOGO ${_pictures}/colour.png)
o3sdk_app(logo_tint SOURCES ${_hello_source} DEPENDS unit_synth unit_delay LOGO ${_pictures}/grey.png LOGO_COLOR 0xFF8000)

set(_check ${O3SDK_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_logo.py --scenario ${CMAKE_CURRENT_SOURCE_DIR}/boot.txt)
add_test(NAME logo_colour_boot COMMAND ${_check} --sim $<TARGET_FILE:logo_colour_sim> --out ${CMAKE_CURRENT_BINARY_DIR}/colour_out --expect colour --rows 410)
add_test(NAME logo_tint_boot COMMAND ${_check} --sim $<TARGET_FILE:logo_tint_sim> --out ${CMAKE_CURRENT_BINARY_DIR}/tint_out --expect tint --tint 0xFF8000 --rows 410)
if(TARGET hello_sim)  # an app without LOGO has the kit's default: the O3 logo, white, 684 x 420 at its own size
  add_test(NAME logo_default_boot COMMAND ${_check} --sim $<TARGET_FILE:hello_sim> --out ${CMAKE_CURRENT_BINARY_DIR}/default_out --expect default)
endif()
set_tests_properties(logo_colour_boot logo_tint_boot PROPERTIES TIMEOUT 60)
