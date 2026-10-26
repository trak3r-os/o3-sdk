# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests (doctest): every *_test.cpp in this directory, one executable (@APP_ID@_tests), one ctest per source file
# (o3sdk_app_test_suite(), o3-sdk/cmake/O3sdkTargets.cmake). Rerun the cmake configure after adding a file.
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
# The helper links what the tests need: the kit, the simulator, the host platform and the app library WHOLE (it defines o3sdk_app(),
# which app_test.cpp names), and hands app/scenarios to the sources as the macro APP_SCENARIO_DIR (app_test.cpp runs every scenario
# as it is). DEPENDS: the units the tests exercise.
o3sdk_app_test_suite(@APP_ID@ SOURCES ${test_sources} DEPENDS unit_voice)
