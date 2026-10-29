# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/midi (docs/M2B2-PLAN.md, slice MC): the MIDI timing core, ported from track3r-os midi/ (OS-005c, ADR 0013 D5) and its MK66 constants
# (midi_config.h, OS-005c2). Pure, host-tested and cross-built, no hardware access: the UART4 and PIT0 registers are the driver's (platform/mk66/src/midi_din.cpp);
# what their interrupts do is here (din_io.h, usb_bind.h, over a register policy, with the time base they share, time_ref.h, and the settings value, config.h).
# Messages and the byte-time model of DIN, the timed queue, the DIN emitter's "clock is sacred" state machine, the USB-MIDI 1 ms frame quantiser, the note-hang
# bookkeeping and the out router; the clock-follow DLL with its stamp map, and the MIDI in parser with the transport events. It lives under the platform, not in
# the kit, because it includes audio/hot.h and audio/events.h (the hot-path placement macros and the event struct), which are the platform's, and the kit must
# not include a platform. Depends on core (the rings, Q32 time), dsp (transport.h: the engine's tempo range, for the frames' increment), hal (hal::midi::Device), usb (the contracts it is bound to) and platform_mk66_audio; nothing above it. No heap, no
# double, no printf. midi_service.cpp (slice MD) is hal::midi::Device over all of it: the kit's one MIDI device on the MK66.
o3sdk_library(platform_mk66_midi
  SOURCES
    src/din.cpp
    src/usb_out.cpp
    src/clock_dll.cpp
    src/in.cpp
    src/out.cpp
    src/usb_bind.cpp
    src/midi_service.cpp
  DEPENDS core dsp hal usb platform_mk66_audio)
# The single-precision rule (cmake/O3sdkTargets.cmake): no double in an image (track3r-os built this library with it as one of its ranked layers).
target_compile_options(platform_mk66_midi PRIVATE -fsingle-precision-constant -fno-math-errno)
# The real-time functions (O3SDK_AUDIO_HOT: the emitter's poll, the DLL update, the drains) run from SRAM_L on the MK66, and may not call into flash (tools/check_ramfunc.py):
# a zeroing or copying loop must stay a loop.
if(O3SDK_CROSS)
  target_compile_options(platform_mk66_midi PRIVATE -fno-tree-loop-distribute-patterns)
endif()
if(O3SDK_PLATFORM STREQUAL "host")
  target_compile_options(platform_mk66_midi PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS)
  add_subdirectory(tests)
endif()
