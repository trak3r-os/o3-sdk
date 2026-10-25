# SPDX-License-Identifier: GPL-3.0-or-later
# FatFs R0.15a (ChaN), vendored unmodified except ffconf.h, which is the OS's configuration (see its header and THIRD_PARTY.md):
# a read-only FAT12/16/32 and exFAT file system with long file names, no heap, nothing written. Only two of the archive's sources
# are compiled: ff.c (the file system) and ffunicode.c (the OEM code page 437 tables and the case folding); the glue to the card
# (the disk_* functions) is first-party, platform/mk66/storage/src/diskio_glue.cpp.
#
# WARNING RELAXATION (third-party code only, the same rule as stb_image_write in platform/host): this target is built with `-w`, not the
# first-party -Wall -Wextra -Wshadow -Wconversion -Werror, because the code is upstream's and is not edited here (a vendored file must stay
# byte-identical to its archive). Its behaviour is held by tests/mk66_storage_*: a FAT32 volume read through the whole stack.
add_library(o3sdk_fatfs STATIC ff.c ffunicode.c)
set_target_properties(o3sdk_fatfs PROPERTIES OUTPUT_NAME o3sdk_fatfs C_STANDARD 11 C_EXTENSIONS ON)
target_include_directories(o3sdk_fatfs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(o3sdk_fatfs PRIVATE -w)
if(O3SDK_PLATFORM STREQUAL "mk66")
  add_custom_command(TARGET o3sdk_fatfs POST_BUILD
    COMMAND ${CMAKE_SIZE} -t $<TARGET_FILE:o3sdk_fatfs>
    VERBATIM)
endif()
