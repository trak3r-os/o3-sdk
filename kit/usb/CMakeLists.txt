# SPDX-License-Identifier: GPL-3.0-or-later
# usb: host-pure USB library (ADR 0012, OS-006a): descriptor builder and validator, UAC2 class requests, SOF
# rate estimator, IN packetiser, OUT feedback, overwrite rings and interleave kernels. No hardware access (the
# MK66 USB0 driver is OS-006b in platform/mk66). Depends on core only; no heap, no double.
o3sdk_library(usb
  SOURCES
    src/descriptors.cpp
    src/validate.cpp
    src/uac2.cpp
    src/rate.cpp
    src/kernels.cpp
    src/stream.cpp
    src/iso_audio.cpp
    src/instances.cpp
    src/control.cpp
    src/diag_shell.cpp
    src/diag_log.cpp
    src/msc.cpp
    src/msc_link.cpp
  DEPENDS core)
# track3r-os checks this layer for unsuffixed floating literals (tools/check_layers.py, not ported); the compiler flag is
# the line of defence here, as for the ranked layers in cmake/O3sdkTargets.cmake (O3SDK_SINGLE_PRECISION_LAYERS).
target_compile_options(usb PRIVATE -fsingle-precision-constant)
# Host Debug builds (the sanitizer build) compile this library with -O2: its loops are the real-time path, the
# property test pushes millions of simulated packets through it, and an unoptimised ASan build of the kernels is
# ten times slower. Sanitizers stay on, so undefined behaviour is still caught at the optimisation level the code
# runs at. The MK66 build keeps the project's -Os.
if(O3SDK_PLATFORM STREQUAL "host")
  target_compile_options(usb PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
