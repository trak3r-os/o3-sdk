# SPDX-License-Identifier: GPL-3.0-or-later
# core: ring, fixed containers, input event, tick helpers, and the input pipeline's pure parts (gesture
# recogniser, binding resolver, keymap parser), the notification queue and the cooperative job runner, and (OS-002d) the parameter
# descriptors with their maths and the MIDI Learn rules. No heap, depends on nothing.
o3sdk_library(core SOURCES src/fixed_string.cpp src/instances.cpp src/gesture.cpp src/binding.cpp src/keymap.cpp
          src/notify.cpp src/job.cpp src/fast_math.cpp src/param.cpp src/learn.cpp)
