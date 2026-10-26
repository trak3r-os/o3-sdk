# SPDX-License-Identifier: GPL-3.0-or-later
# The simulator runner on an app whose setup had a refused call: bad_setup_sim (built by o3sdk_app like any app's) must exit 1 and name
# the refusal on stderr, without writing a picture (sim/src/sim_main.cpp; the Harness side is tests/setup_errors_test.cpp).
o3sdk_app(bad_setup SOURCES bad_setup_app.cpp)
add_test(NAME sim_setup_refusal
  COMMAND ${CMAKE_COMMAND} -DSIM=$<TARGET_FILE:bad_setup_sim> -DOUT=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/expect_refusal.cmake)
