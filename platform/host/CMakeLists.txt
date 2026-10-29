# SPDX-License-Identifier: GPL-3.0-or-later
# platform/host: host fakes of the HAL contracts plus host utilities (SHA-256, PNG writer). Standard library allowed; never
# linked by the kit's layers, only by the simulator and the tests.
# The card (docs/M2C-PLAN.md, slice FS; ported from track3r-os): DirStorage, a host directory as the read-only card (hal/storage.h); FakeStorage, the directory side
# of a card in memory; DirCard, a scratch directory as a card with a write side (hal/storage_write.h) that applies the real guard (hal/app_guard.h) to every change.
o3sdk_library(platform_host SOURCES src/fakes.cpp src/sha256.cpp src/png.cpp src/stb_image_write_impl.cpp src/host_cycles.cpp
  src/dir_storage.cpp src/dir_card.cpp src/fake_storage.cpp
  DEPENDS hal)

# stb_image_write.h is vendored third-party code (third_party/stb), found as <stb/stb_image_write.h> through a SYSTEM include
# directory. Its implementation lives in stb_image_write_impl.cpp, the only file built without the first-party warning flags.
# The fake USB port runs the unit's own USB audio service (fake_usb_audio.h, M2c slice U): the platform's audio library (the stem tap and map, the service; it brings dsp) and the kit's usb library.
target_link_libraries(platform_host PUBLIC platform_mk66_audio usb)

target_include_directories(platform_host SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
set_source_files_properties(src/stb_image_write_impl.cpp PROPERTIES COMPILE_OPTIONS "-w;-O2")
set_source_files_properties(src/sha256.cpp src/png.cpp PROPERTIES COMPILE_OPTIONS "-O2")

# The card on a FAT32 image file (FatFsStorage, FatImage), its own library: it needs platform_mk66_save, with FatFs and the unit's own FatStorage behind it (platform/mk66
# is configured on a host build only when the tests are on), and platform_host, which every host test links, does not pull the unit's file system in.
if(TARGET platform_mk66_save)
  add_subdirectory(fat)
endif()
