# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests (doctest): every *_test.cpp in this directory, one executable, one ctest per source file (o3sdk_test_suite(),
# o3-sdk/cmake/O3sdkTargets.cmake). Rerun the cmake configure after adding a file.
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
# app_test.cpp runs the app end to end: its library defines o3sdk_app(), which only that test names, so it is linked whole.
o3sdk_test_suite(@APP_ID@_tests
  SOURCES ${test_sources}
  DEPENDS app sim platform_host unit_voice "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_@APP_ID@>")
# The scenarios of app/scenarios run as they are (app_test.cpp).
target_compile_definitions(@APP_ID@_tests PRIVATE APP_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../app/scenarios")
