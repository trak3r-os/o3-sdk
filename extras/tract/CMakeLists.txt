# SPDX-License-Identifier: GPL-3.0-or-later
# The tract: a vocal-tract effect DSP shared by o3-303 and o3-voc03er (README.md). OPT-IN: the sdk's own CMake never adds this directory and its
# CI never builds it; an app adds it with
#   add_subdirectory(${O3SDK_DIR}/extras/tract ${CMAKE_BINARY_DIR}/extras_tract)
# and compiles tests/*_test.cpp into its own test suite (DEPENDS unit_tract_dsp). The library is `unit_tract_dsp`; the headers are
# <o3sdk/extras/tract/...>.
o3sdk_unit(tract_dsp SOURCES src/tube.cpp src/shape.cpp src/halfband.cpp src/processor.cpp)
# Built for speed like the growl (the image is MinSizeRel). No fused multiply-add: on the Cortex-M4F a VFMA/VMLA takes 3 cycles and a VMUL +
# VADD 2, and the loops are chains of such pairs.
target_compile_options(unit_tract_dsp PRIVATE -O2 -ffp-contract=off)
