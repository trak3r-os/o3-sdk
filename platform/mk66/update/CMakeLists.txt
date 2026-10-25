# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/update (OS-006c, ported from track3r-os): the firmware update path over the console. Everything in it is pure and is
# `platform_mk66_update`, tested on the host: the path guard, the streaming Intel HEX validator, the transfer and arm state machines, the
# `fw` command and the FatFs-backed FirmwareStore, and the adapter of the App's firmware port (fw_sink.cpp)
# (tests/mk66_update_*_test.cpp). Only src/fw_host_mk66.cpp (the PTA1 hand-over, the mute, the entropy) is the image's:
# `platform_mk66_update_hw`, which joins the image (platform/mk66/CMakeLists.txt). Its references to appimg::, console::log and
# usb0::{serial, cdc_writer} are the image's to define (docs/M2B-PLAN.md section 3).
o3sdk_library(platform_mk66_update
  SOURCES src/path_guard.cpp src/hex_validator.cpp src/fat_firmware_store.cpp src/fw_service.cpp src/fw_sink.cpp
  DEPENDS platform_mk66_base platform_mk66_storage usb)

if(O3SDK_PLATFORM STREQUAL "mk66")
  # platform_mk66_usb: <o3sdk/platform/mk66/usb/{usb0,console}.h> (the headers only; the symbols are the image's: usb0.cpp in platform_mk66_usb_hw, console::log in the console).
  add_library(platform_mk66_update_hw OBJECT src/fw_host_mk66.cpp)
  target_link_libraries(platform_mk66_update_hw PUBLIC platform_mk66_update platform_mk66_audio platform_mk66_usb)
  o3sdk_strict_warnings(platform_mk66_update_hw)
endif()

# Host only: the validator against tools/check_image.py on the same files (the mutations of check_image's own self-test, and the real
# image when fw_hex_cross.py is given --hex). The helper prints one verdict per file; the script compares.
if(O3SDK_PLATFORM STREQUAL "host")
  add_executable(o3sdk_fw_hex_verdict tools/fw_hex_verdict.cpp)
  target_link_libraries(o3sdk_fw_hex_verdict PRIVATE platform_mk66_update)
  o3sdk_strict_warnings(o3sdk_fw_hex_verdict)
  if(O3SDK_BUILD_TESTS)
    add_test(NAME mk66_fw_hex_cross
      COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/fw_hex_cross.py
              --tool $<TARGET_FILE:o3sdk_fw_hex_verdict> --check-image ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_image.py)
    set_tests_properties(mk66_fw_hex_cross PROPERTIES TIMEOUT 120 LABELS mk66)
  endif()
endif()
