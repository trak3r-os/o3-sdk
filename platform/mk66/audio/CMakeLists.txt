# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/audio: the audio path. The pure parts (the slot ring, the pipeline between the DMA ISR and the render line, the status
# counters, the `cpu` / `mem` text formatters (diag), the test tone, the SGTL5000 register plan, the I2S/eDMA values, the output gate) are `platform_mk66_audio`, tested on the
# host (tests/mk66_audio_config_test.cpp, mk66_sgtl5000_test.cpp, mk66_output_gate_test.cpp, audio_*_test.cpp); the I2S0 / eDMA / codec
# code (src/audio.cpp) is `platform_mk66_audio_hw`, which joins the image (platform/mk66/CMakeLists.txt).
# M2c slice AP (docs/M2C-PLAN.md section 2, ported from track3r-os audio/ and platform/mk66): the taps (RecTap, StemTap, StemPool, SampleRing, TapMem), the sample kernels
# and their SIMD helpers, the test signal, the stem map and the Si4703 FM radio driver, all pure and host-tested (tests/audio_taps_test.cpp, audio_kernels_test.cpp,
# audio_test_signal_test.cpp, audio_stem_map_test.cpp, audio_tap_mem_test.cpp, mk66_si4703_test.cpp). Nothing here is linked into an image yet (a static library member is
# pulled only when referenced): the slices that wire it (AI, RC, U) do.
# taps.h, taps.cpp and stem_map.h name the kit's usb library (the stem ring, the slot sources, the sum and interleave kernels, the profiles; usb depends on core only, so there is
# no cycle and no platform_mk66_usb). Its HEADERS are a usage requirement of this library; the library is not on its link line: linking `usb` here would put it right after this
# archive in every image's link line and move every object of the image (the plan's "hello is unchanged" check), and the images that use the taps (all of them, from RC) already
# link `usb` after this library (platform/mk66/CMakeLists.txt, _mk66_main_libs). A target that links this library and runs the taps (the tests) links `usb` itself.
o3sdk_library(platform_mk66_audio
  SOURCES src/pipeline.cpp src/status.cpp src/test_tone.cpp src/diag.cpp src/taps.cpp src/kernels.cpp src/stem_map.cpp src/test_signal.cpp src/si4703.cpp
  DEPENDS platform_mk66_base)
target_include_directories(platform_mk66_audio PUBLIC $<TARGET_PROPERTY:usb,INTERFACE_INCLUDE_DIRECTORIES>)
# track3r-os builds its audio library with -ffp-contract=off on the target (no fused multiply-add: the host and the unit round alike). Only the ported float code gets it here, so the
# objects of the files already in the image do not change.
if(O3SDK_CROSS)
  set_source_files_properties(src/kernels.cpp src/test_signal.cpp src/taps.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# The single-precision rule (cmake/O3sdkTargets.cmake): the test tone's table is float.
target_compile_options(platform_mk66_audio PRIVATE -fsingle-precision-constant)
# The codec's CHIP_I2S_CTRL (sgtl5000.h): the value proven on the unit, or the data sheet's letter for a 64 Fs slave, an A/B item of the
# hardware checklist (track3r-os HW-A row E-1). PUBLIC: the image's audio.cpp and the host test see the same value.
set(O3SDK_SGTL5000_I2S_CTRL "0x0130" CACHE STRING "SGTL5000 CHIP_I2S_CTRL value: 0x0130 (default, the value proven on this board) or 0x0030 (SCLKFREQ 64 Fs, A/B item E-1)")
set_property(CACHE O3SDK_SGTL5000_I2S_CTRL PROPERTY STRINGS 0x0130 0x0030)
if(NOT O3SDK_SGTL5000_I2S_CTRL MATCHES "^0x0[01]30$")
  message(FATAL_ERROR "O3SDK_SGTL5000_I2S_CTRL must be 0x0130 or 0x0030 (got '${O3SDK_SGTL5000_I2S_CTRL}')")
endif()
target_compile_definitions(platform_mk66_audio PUBLIC O3SDK_SGTL5000_I2S_CTRL=${O3SDK_SGTL5000_I2S_CTRL})
if(O3SDK_PLATFORM STREQUAL "mk66")
  # The real-time functions (O3SDK_AUDIO_HOT, audio/hot.h) run from SRAM_L (ADR 0013 D7), where the reset sequence copies them. PUBLIC:
  # every translation unit that sees their declarations sees the same placement.
  target_compile_definitions(platform_mk66_audio PUBLIC O3SDK_AUDIO_RAMFUNC=1)
  # The hot path may not call into flash (tools/check_ramfunc.py): a zeroing or copying loop must stay a loop, not become a memset or
  # memcpy call.
  target_compile_options(platform_mk66_audio PRIVATE -fno-tree-loop-distribute-patterns)

  add_library(platform_mk66_audio_hw OBJECT src/audio.cpp)
  target_link_libraries(platform_mk66_audio_hw PUBLIC platform_mk66_audio)
  # src/internal.h (boot::delay_ms, the I2C master) and src/regs.h are private to the platform's sources.
  target_include_directories(platform_mk66_audio_hw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
  target_compile_options(platform_mk66_audio_hw PRIVATE -fno-tree-loop-distribute-patterns -fsingle-precision-constant)
  o3sdk_strict_warnings(platform_mk66_audio_hw)
endif()
