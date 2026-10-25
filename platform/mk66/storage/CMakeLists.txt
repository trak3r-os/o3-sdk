# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66/storage (OS-007e, ported from track3r-os): the SD card. The pure parts (the SD command set and the decoding of its
# responses, the SDHC register facts and the clock arithmetic, the timeout policy, the card's identification sequence and block reads
# over the SdBus seam, the FatFs-backed hal::storage::Storage, the text of `sd` and the `ls` pager, the card-swap and claims logic)
# are `platform_mk66_storage`, tested on the host against a software card (tests/mk66_storage_*_test.cpp); the SDHC register policy and
# the driver objects (src/sdhc_bus.cpp, src/device.cpp) are `platform_mk66_storage_hw`, which joins the image
# (platform/mk66/CMakeLists.txt). FatFs itself is third_party/fatfs (target o3sdk_fatfs): the host tests read a FAT32 volume through it.
if(NOT TARGET o3sdk_fatfs)
  add_subdirectory(${O3SDK_ROOT}/third_party/fatfs ${CMAKE_CURRENT_BINARY_DIR}/fatfs)
endif()

# usb: the `ls` pager and the `sd` text write through the console's Shell (kit/usb); platform_mk66_audio: audio::diag::Text, the bounded
# text builder (no printf).
o3sdk_library(platform_mk66_storage
  SOURCES src/sd_protocol.cpp src/sd_card.cpp src/fat_storage.cpp src/sd_text.cpp src/ls_command.cpp src/card_swap.cpp
  DEPENDS platform_mk66_base platform_mk66_audio usb o3sdk_fatfs)
# As track3r-os's image option of the same name (sd_card.cpp defaults it to 1): ON switches the card to high speed (CMD6, 50 MHz asked,
# 45 MHz made) after the 25 MHz identification. OFF stays at 22.5 MHz, for a hardware session that sees CRC errors at the higher clock.
option(O3SDK_SD_HIGH_SPEED "Image: switch the SD card to high speed (CMD6, 50 MHz asked, 45 MHz made) after the 25 MHz identification. OFF: stay at 22.5 MHz, for a hardware session that sees CRC errors at the higher clock" ON)
target_compile_definitions(platform_mk66_storage PRIVATE O3SDK_SD_HIGH_SPEED=$<BOOL:${O3SDK_SD_HIGH_SPEED}>)

if(O3SDK_PLATFORM STREQUAL "mk66")
  # The SDHC register policy and the bus, card and volume objects. src/internal.h (boot::delay_ms, appimg::) and src/regs.h are private to
  # the platform's sources: the two files name them relative to themselves.
  add_library(platform_mk66_storage_hw OBJECT src/sdhc_bus.cpp src/device.cpp)
  target_link_libraries(platform_mk66_storage_hw PUBLIC platform_mk66_storage)
  o3sdk_strict_warnings(platform_mk66_storage_hw)
endif()
