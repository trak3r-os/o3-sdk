# SPDX-License-Identifier: GPL-3.0-or-later
# platform/host: host fakes of the HAL contracts plus host utilities (SHA-256, PNG writer). Standard library allowed; never
# linked by the kit's layers, only by the simulator and the tests.
o3sdk_library(platform_host SOURCES src/fakes.cpp src/sha256.cpp src/png.cpp src/stb_image_write_impl.cpp src/host_cycles.cpp
  DEPENDS hal)

# stb_image_write.h is vendored third-party code (third_party/stb), found as <stb/stb_image_write.h> through a SYSTEM include
# directory. Its implementation lives in stb_image_write_impl.cpp, the only file built without the first-party warning flags.
target_include_directories(platform_host SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
set_source_files_properties(src/stb_image_write_impl.cpp PROPERTIES COMPILE_OPTIONS "-w;-O2")
set_source_files_properties(src/sha256.cpp src/png.cpp PROPERTIES COMPILE_OPTIONS "-O2")
