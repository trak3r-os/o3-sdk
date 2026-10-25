# SPDX-License-Identifier: GPL-3.0-or-later
# dsp: the unit contract (unit.h), events, the transport and the engine that runs the units of an app. Depends on core only; no
# heap, no hardware. See docs/FRAMEWORK.md.
o3sdk_library(dsp SOURCES src/transport.cpp src/engine.cpp DEPENDS core)
target_compile_options(dsp PRIVATE -fno-math-errno)
