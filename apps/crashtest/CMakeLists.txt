# SPDX-License-Identifier: GPL-3.0-or-later
# CRASH TEST: a session-only test app (never a release) whose console command `fault` provokes each fault the MK66 platform's crash record and blink code are meant to catch
# (docs/M2C-PLAN.md section 9, D15; CHECKLIST.md is the tester's procedure). It uses no unit: nothing here makes sound.
o3sdk_app(crashtest SOURCES src/crashtest_app.cpp src/faults.cpp)

# The host test (label default): the command is in the app's table and passes the console's own rules, the faults table is what CHECKLIST.md says, and `fault <kind> confirm`
# arms, fires and reports "no fault" on the host, where nothing can fault. It links the console's pure part for the table's rules (platform_mk66_console builds on the host).
if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS AND TARGET platform_mk66_console)
  o3sdk_app_test_suite(crashtest SOURCES tests/crashtest_test.cpp DEPENDS platform_mk66_console)
endif()
