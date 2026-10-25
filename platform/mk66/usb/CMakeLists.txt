# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/usb (OS-006b, ported from track3r-os; docs/M2B-PLAN.md, slice D): the MK66's USB0 device driver and the `diag` console's platform half. The pure parts (the
# controller's register facts and clock arithmetic, the unique-ID serial number, the text of the platform's diag commands, the `log` pager, the `peek`/`poke` allow-lists, the
# key/pad/encoder injector, the input hold, the `shot` stream, and the USB audio tap that feeds the capture stream with the master) are `platform_mk66_usb`, tested on the host
# (tests/mk66_usb_*_test.cpp, mk66_hil_inject_test.cpp, mk66_input_hold_test.cpp, mk66_mem_access_test.cpp, mk66_shot_stream_test.cpp). The register policy, the driver objects and the
# interrupt handler (src/usb0.cpp) are `platform_mk66_usb_hw`, which joins the image (platform/mk66/CMakeLists.txt). The device logic itself (control pipe, endpoints, rings, shell) is
# the kit's usb library; the console that composes these (platform/mk66/console) implements console.h.
#
# Dependencies: platform_mk66_audio (audio.h's AudioClockFault, audio_config.h, audio::diag::Text), platform_mk66_display (ft812_regs.h, for the `shot` bands),
# platform_mk66_update (crc32.h, the `shot` stream's checksum) and usb. The update path's hardware half (update/src/fw_host_mk66.cpp) names usb0.h and console.h, so it links this.
o3sdk_library(platform_mk66_usb
  SOURCES src/diag_text.cpp src/log_command.cpp src/mem_access.cpp src/hil_inject.cpp src/shot_stream.cpp src/usb_audio.cpp
  DEPENDS platform_mk66_base platform_mk66_audio platform_mk66_display platform_mk66_update usb)
# The single-precision rule (cmake/O3sdkTargets.cmake): no double in an image; the kit's usb library is built this way too.
target_compile_options(platform_mk66_usb PRIVATE -fsingle-precision-constant)

if(O3SDK_PLATFORM STREQUAL "mk66")
  # The USB0 register policy, the descriptors, the streams and USB0_IRQHandler. src/internal.h (boot::, appimg::) and src/regs.h are private to the platform's sources: the file
  # names them relative to itself. It runs from flash (the interrupt is at priority 64, not in the hot SRAM_L path).
  add_library(platform_mk66_usb_hw OBJECT src/usb0.cpp)
  target_link_libraries(platform_mk66_usb_hw PUBLIC platform_mk66_usb)
  target_compile_options(platform_mk66_usb_hw PRIVATE -fsingle-precision-constant)
  o3sdk_strict_warnings(platform_mk66_usb_hw)
endif()
