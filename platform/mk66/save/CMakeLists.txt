# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/save (docs/M2C-PLAN.md, slice FS): the card's write side for an app's saves, ported from track3r-os platform/mk66/save (OS-007n, without the bench's
# scratch file): the FatFs-backed hal::storage::StorageWriter. Pure, as in track3r-os: it joins no image yet (the files service of the app kit does, in a later slice) and its
# tests run on the host against a software volume (tests/mk66_save_writer_test.cpp). It is one of the two writers of the card, with the update path's
# fat_firmware_store.cpp, and answers through hal/app_guard.h; platform/mk66/tools/check_writers.py keeps the list of who may name FatFs's write calls.
# Depends on platform_mk66_storage (FatStorage, the card swap, the block device, FatFs).
o3sdk_library(platform_mk66_save SOURCES src/fat_storage_writer.cpp DEPENDS platform_mk66_storage)

if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS)
  add_subdirectory(tests)
endif()
