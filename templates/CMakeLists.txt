# SPDX-License-Identifier: GPL-3.0-or-later
#
# The tests of the app template (templates/app, docs/NEW-APP.md). The template is a product: a new app starts from it, so it is
# instantiated by tools/new_app.py and built the way a user builds it, on every ctest run of o3-sdk's own tree (O3SDK_BUILD_TESTS).
#
#   new_app_selftest     tools/new_app.py's own checks (valid and refused ids and names, no overwrite): a second.
#   gate_selftest        tools/gate.py's pure parts (the link table, the manifest, the identity).
#   template_host_build  (host tree) instantiate, configure, build warning-free, run the instance's three ctests and its scenario.
#   template_mk66_build  (mk66 tree, label mk66) the same cross-built: the instance's four image ctests (image, flashconfig, ramfunc,
#                        usb), the image files, no software double.
#
# Both builds use their own directory under this one (<build>/templates/work-*), kept between runs, so a second run rebuilds only what
# changed; a first run compiles the kit once more (about 20 s with 4 jobs on a recent machine, ccache is used when it is installed).
# Label `template`: `ctest -L template` runs only these, `ctest -LE template` leaves them out.
add_test(NAME new_app_selftest COMMAND ${O3SDK_PYTHON} -I ${O3SDK_ROOT}/tools/new_app.py --self-test)
set_tests_properties(new_app_selftest PROPERTIES TIMEOUT 60 LABELS template)
# tools/gate.py (the mechanical half of the release gate, copied into every app repository): its pure parts. The gate itself builds an image
# twice, so it is run by hand or by an agent before a candidate goes to the maintainer (docs/NEW-APP.md section 10), not by ctest.
add_test(NAME gate_selftest COMMAND ${O3SDK_PYTHON} -I ${O3SDK_ROOT}/tools/gate.py --self-test)
set_tests_properties(gate_selftest PROPERTIES TIMEOUT 60 LABELS template)

find_program(O3SDK_CCACHE ccache)
set(_ccache_arg "")
if(O3SDK_CCACHE)
  set(_ccache_arg --ccache)
endif()

if(O3SDK_CROSS)
  get_filename_component(_arm_bin "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_arm_root "${_arm_bin}" DIRECTORY)
  add_test(NAME template_mk66_build
    COMMAND ${O3SDK_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_template.py mk66 --sdk ${O3SDK_ROOT}
            --work ${CMAKE_CURRENT_BINARY_DIR}/work-mk66 --arm ${_arm_root} --nm ${CMAKE_NM}
            --cmake ${CMAKE_COMMAND} --ctest ${CMAKE_CTEST_COMMAND} ${_ccache_arg})
  set_tests_properties(template_mk66_build PROPERTIES TIMEOUT 900 LABELS "mk66;template")
else()
  add_test(NAME template_host_build
    COMMAND ${O3SDK_PYTHON} -I ${CMAKE_CURRENT_SOURCE_DIR}/check_template.py host --sdk ${O3SDK_ROOT}
            --work ${CMAKE_CURRENT_BINARY_DIR}/work-host
            --cmake ${CMAKE_COMMAND} --ctest ${CMAKE_CTEST_COMMAND} ${_ccache_arg})
  set_tests_properties(template_host_build PROPERTIES TIMEOUT 900 LABELS template)
endif()
