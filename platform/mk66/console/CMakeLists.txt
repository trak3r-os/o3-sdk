# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/console: the kit's console (docs/M2B-PLAN.md, slice C). The pure part is `platform_mk66_console`: the command table (the kit's rows, then an app's, with the rules for
# the app's), `state`, the shell binding, `key` / `pad` / `enc`, the `mem` report, the `midi` report, the `files` report and its CRC, the module ids and the crash-loop rule for USB, and the guard that takes the unit back to the STANDARD
# set when the host does not configure the AUDIO one. Pure and tested on the host (tests/mk66_console_*_test.cpp, usb_mode_guard_test.cpp). The rest of the console, the commands that
# read the board (src/console.cpp: the shell over the CDC port, the log ring, the handlers) and the image's composition hooks (src/appimg.cpp), is `platform_mk66_console_hw`, which
# joins the image (platform/mk66/CMakeLists.txt).
o3sdk_library(platform_mk66_console
  SOURCES src/core.cpp src/shell_bind.cpp src/inject_commands.cpp src/midi_report.cpp src/files_command.cpp
  DEPENDS platform_mk66_base platform_mk66_audio platform_mk66_midi platform_mk66_storage platform_mk66_usb usb app)

if(O3SDK_PLATFORM STREQUAL "mk66" AND O3SDK_MK66_CONSOLE)
  # src/internal.h (boot::, appimg::, cache::) and src/regs.h are private to the platform's sources: the files name them relative to themselves. The generated
  # o3sdk_build_info.h (the banner and `version` print O3SDK_IMAGE_LABEL) is the image's: platform/mk66/CMakeLists.txt adds its directory to this target.
  add_library(platform_mk66_console_hw OBJECT src/console.cpp src/appimg.cpp)
  target_link_libraries(platform_mk66_console_hw PUBLIC platform_mk66_console platform_mk66_display platform_mk66_input platform_mk66_power platform_mk66_storage platform_mk66_update platform_mk66_usb)
  target_compile_options(platform_mk66_console_hw PRIVATE -fsingle-precision-constant)
  o3sdk_strict_warnings(platform_mk66_console_hw)
endif()
