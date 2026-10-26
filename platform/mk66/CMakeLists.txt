# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66 (milestone M2): the MK66FX1M0 platform, ported from track3r-os platform/mk66. Implements the HAL contracts and runs
# an app (docs/FRAMEWORK.md section 9, docs/M2-PLAN.md).
#
# One library per domain, so each builds and tests on its own:
#   platform_mk66_base     memory map, crash record, clock/SDRAM/cache register values, blink codes, interrupt map (include/, src/)
#   platform_mk66_display  the FT812 driver's pure parts (display/)
#   platform_mk66_input    keys, pads, encoder, pad LEDs: pure parts (input/)
#   platform_mk66_audio    the audio pipeline (slot ring, status), codec and I2S facts (audio/)
#   platform_mk66_storage  the SD card: SDHC facts, SD protocol, FatFs-backed storage, card swap, `sd` and `ls` text (storage/)
#   platform_mk66_update   the firmware update path: path guard, HEX validator, `fw` service, FirmwareStore, App firmware port (update/)
#   platform_mk66_usb      the USB0 driver's pure parts, the `diag` platform text, the HIL injector, the `shot` stream, the USB audio tap (usb/)
#   platform_mk66_console  the console's table, `state`, the USB crash-loop rule and the mode guard: pure parts (console/)
#   platform_mk66_power    the Power switch's shutdown: the state machine, the VLPR off's sequence, the report; pure (power/)
# The pure parts build on every platform and their tests (tests/) run on the host. With -DO3SDK_PLATFORM=mk66 the block at the end
# builds one firmware image per app (`<app>.elf`, `<app>.hex`, `<app>.ptf`) with the hardware halves of each domain.

# The USB console, the SD card and the firmware update path (docs/M2B-PLAN.md): ON (the default) links them into every image, with the composition main.cpp makes of them; OFF links
# exactly M2's image (no usb, storage, update or console objects), so the M2 session images do not change. The skeleton image has none either way.
option(O3SDK_MK66_CONSOLE "Image: the USB console (CDC `diag`, USB audio, the firmware update path) and the SD card. OFF: M2's image" ON)

o3sdk_library(platform_mk66_base SOURCES src/crash_record.cpp DEPENDS hal core)

add_subdirectory(display)
add_subdirectory(input)
add_subdirectory(audio)
add_subdirectory(storage)
add_subdirectory(update)
add_subdirectory(usb)
add_subdirectory(midi)
add_subdirectory(power)
add_subdirectory(console)

if(NOT O3SDK_CROSS AND O3SDK_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- The python tools (owner: the boot slice) --------------------------------------------------------------------------
# Stdlib only, no target: their self-tests, the eDMA ownership rule over this tree's sources, and (host) the generated linker script
# against the header it comes from; the mk66 build checks the real generated file and the images as well (below). The tests are o3sdk's
# own (O3SDK_BUILD_TESTS); the tools themselves are used by every image.
set(_mk66_tools "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(_mk66_header "${CMAKE_CURRENT_SOURCE_DIR}/include/o3sdk/platform/mk66/mem_map.h")
set(_mk66_template "${CMAKE_CURRENT_SOURCE_DIR}/mk66.ld.in")
if(O3SDK_BUILD_TESTS)
  add_test(NAME mk66_gen_ld_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py --self-test)
  add_test(NAME mk66_image_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py --self-test)
  add_test(NAME mk66_ramfunc_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_ramfunc.py --self-test)
  # Only boot::dma_init() writes DMA_CR and the DCHPRIs (ADR 0013 D1): a source check over this tree, every slice's files included.
  add_test(NAME mk66_dma_owner_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_dma_owner.py --self-test)
  add_test(NAME mk66_dma_owner COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_dma_owner.py check --root ${CMAKE_CURRENT_SOURCE_DIR})
  # The host tools of the USB console (tools/README.md), copied from track3r-os and identical but for the names: each one's self-test, with no
  # hardware. o3console talks to a pseudo-terminal that plays the unit; o3shot decodes and compares screens; o3audio stands in for arecord;
  # o3hil runs scenarios against a mock unit; o3disk finds the unit's disk in a fake /sys (the disk set itself is not in o3sdk yet).
  add_test(NAME mk66_o3console_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3console.py --self-test)
  add_test(NAME mk66_o3shot_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3shot.py --self-test)
  add_test(NAME mk66_o3audio_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3audio.py --self-test)
  add_test(NAME mk66_o3hil_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3hil.py --self-test)
  add_test(NAME mk66_o3disk_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3disk.py --self-test)
  # Only the update path writes the card and only it drives the bootloader request pin: a source check over o3sdk's whole tree (it passes
  # while a file is absent and fails when a writer turns up anywhere else). The root is o3sdk's own directory, not the top-level project's.
  add_test(NAME mk66_writers_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_writers.py --self-test)
  add_test(NAME mk66_writers COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_writers.py check --root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  # The USB check of an image (vector, g_bdt, the identity string): only its self-test here; each app's image gets `<app>_usb` (o3sdk_app).
  add_test(NAME mk66_usb_image_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_usb_image.py --self-test)
  # The build identity script: the kit's and the app's hashes, "unknown", "-dirty", the version tag (tests/gen_build_info_test.cmake).
  add_test(NAME mk66_build_info COMMAND ${CMAKE_COMMAND} -DGEN=${_mk66_tools}/gen_build_info.cmake
           -DWORK=${CMAKE_CURRENT_BINARY_DIR}/build_info_test -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_build_info_test.cmake)
  set_tests_properties(mk66_gen_ld_selftest mk66_image_selftest mk66_ramfunc_selftest mk66_dma_owner_selftest mk66_dma_owner
                       mk66_build_info mk66_writers_selftest mk66_writers mk66_usb_image_selftest PROPERTIES TIMEOUT 60 LABELS mk66)
  set_tests_properties(mk66_o3console_selftest mk66_o3shot_selftest mk66_o3audio_selftest mk66_o3hil_selftest mk66_o3disk_selftest
                       PROPERTIES TIMEOUT 180 LABELS mk66)  # (a pseudo-terminal and a few seconds of mock unit: 16 s idle, slower on a busy host)
  if(NOT O3SDK_PLATFORM STREQUAL "mk66")
    add_test(NAME mk66_ld COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py check --header ${_mk66_header} --template ${_mk66_template})
    set_tests_properties(mk66_ld PROPERTIES TIMEOUT 60 LABELS mk66)
  endif()
endif()

if(NOT O3SDK_PLATFORM STREQUAL "mk66")
  return()
endif()

# ---- The image (owner: the boot slice of docs/M2-PLAN.md) ---------------------------------------------------------------
# One image per app: o3sdk_app() (cmake/O3sdkTargets.cmake) calls o3sdk_mk66_image() below, which links `<app>.elf` from the reset
# sequence and main (platform_mk66_image), the drivers' hardware halves (the `_hw` object libraries of display/, input/, audio/) and
# the app library, then writes `<app>.hex` behind the static checks, `<app>.bin`, and `<app>.ptf`: the checked HEX under the name the
# Tracker's bootloader takes from the card (README.md). Every option below is one place of its default.
option(O3SDK_MK66_SKELETON "Image: link without the display, input and audio drivers (the _hw object libraries), to prove the reset sequence and the image pipeline alone" OFF)
option(O3SDK_MK66_SDRAM_FULL_TEST "Image: test every SDRAM cell at boot (a few seconds by estimate); the wiring tests always run" ON)
# OS-PERF3, as proven in track3r-os: the LMEM code cache on in the reset sequence (lmem.h), and the SDRAM's ICode alias decoded after the
# SDRAM tests (sdramc_config.h kCm0WithAlias; nothing in the kit reads through the alias yet). OFF undoes either for an A/B on the unit.
option(O3SDK_CODE_CACHE "Image: turn the K66's 8 KiB code cache on in the reset sequence, so flash code and constants run at cache speed" ON)
option(O3SDK_SDRAM_ICODE_ALIAS "Image: after the SDRAM tests, make the controller answer at the ICode alias 0x0800_0000 as well as at 0x7000_0000" ON)
option(O3SDK_MK66_AUDIO_TEST "Image: the audio bring-up image, the device plays the 1 kHz test tone (audio/test_tone.h) instead of the app's engine" OFF)
# OS-POWER P2, as in track3r-os (O3OS_POWER_VLPR_OFF): the end of a real power-off (a hold of Power, `power off`) is the VLPR off: PTC3 polled in very-low-power RUN, a press is a software reset. OFF: the end is P1's
# restart after the release of Power (the fallback of the decision "if the wake fails on the unit the alpha ships P1's restart-off only", spec section 1). `power off dry` is the restart in both. One place of the default.
# Only an image with the console has the Power switch (power_host_mk66.cpp names the console's log, USB, the card and the update path).
option(O3SDK_POWER_VLPR_OFF "Image: the end of a real power-off is the VLPR off with a polled wake (OS-POWER P2). OFF: a restart after the release of Power (P1)" ON)

# The linker script, generated from mem_map.h at build time.
set(_mk66_ld "${CMAKE_CURRENT_BINARY_DIR}/mk66.ld")
add_custom_command(OUTPUT ${_mk66_ld}
  COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py generate --header ${_mk66_header} --template ${_mk66_template} --out ${_mk66_ld}
  DEPENDS ${_mk66_tools}/gen_ld.py ${_mk66_template} ${_mk66_header}
  COMMENT "Generating mk66.ld from mem_map.h"
  VERBATIM)
add_custom_target(mk66_ld_script DEPENDS ${_mk66_ld})

# The build identity (tools/gen_build_info.cmake: the git hash, "-dirty", the version tag), written at build time and only when it
# changed; main.cpp keeps the label in the image, so a `.bin` or `.elf` says which trees it came from (`strings`). Two trees, two identities: the
# kit's is o3sdk's own directory (O3SDK_ROOT, wherever the project that uses it keeps it), the app's is the top-level project's
# (CMAKE_SOURCE_DIR); they are one when the app is one of o3sdk's own. The header's O3SDK_IMAGE_LABEL is what the device shows.
set(_mk66_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_custom_target(o3sdk_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${O3SDK_ROOT} -DAPP_SRC=${CMAKE_SOURCE_DIR} -DOUT=${_mk66_gen_dir}/o3sdk_build_info.h
          -P ${_mk66_tools}/gen_build_info.cmake
  BYPRODUCTS ${_mk66_gen_dir}/o3sdk_build_info.h
  COMMENT "o3sdk_build_info.h"
  VERBATIM)

# The reset sequence, the fault path, the clock, the I2C master, the code cache and main: compiled once, linked into every app's image.
# main.cpp is the composition root (display, input, the app's Runtime, audio); the skeleton's main_skeleton.cpp only names o3sdk_app().
set(_mk66_console OFF)
if(O3SDK_MK66_SKELETON)
  set(_mk66_main src/main_skeleton.cpp)
  set(_mk66_main_libs "")
else()
  set(_mk66_main src/main.cpp)
  set(_mk66_main_libs platform_mk66_display platform_mk66_input platform_mk66_audio)
  if(O3SDK_MK66_CONSOLE)
    set(_mk66_console ON)
    list(APPEND _mk66_main_libs platform_mk66_console platform_mk66_usb platform_mk66_storage platform_mk66_update platform_mk66_power usb)
  endif()
endif()
add_library(platform_mk66_image OBJECT src/startup.cpp src/boot.cpp src/crash_store.cpp src/clock.cpp src/i2c.cpp src/cache.cpp ${_mk66_main})
target_link_libraries(platform_mk66_image PUBLIC platform_mk66_base app ${_mk66_main_libs})
target_include_directories(platform_mk66_image PRIVATE ${_mk66_gen_dir})
add_dependencies(platform_mk66_image o3sdk_build_info)
o3sdk_strict_warnings(platform_mk66_image)
target_compile_options(platform_mk66_image PRIVATE -fsingle-precision-constant)
target_compile_definitions(platform_mk66_image PRIVATE
  O3SDK_SDRAM_FULL_TEST=$<BOOL:${O3SDK_MK66_SDRAM_FULL_TEST}>
  O3SDK_CODE_CACHE=$<BOOL:${O3SDK_CODE_CACHE}>
  O3SDK_SDRAM_ICODE_ALIAS=$<BOOL:${O3SDK_SDRAM_ICODE_ALIAS}>
  O3SDK_AUDIO_TEST=$<BOOL:${O3SDK_MK66_AUDIO_TEST}>
  O3SDK_CONSOLE=$<BOOL:${_mk66_console}>)
# The Power switch's image half (power/src/power_host_mk66.cpp): the end of a real off, see O3SDK_POWER_VLPR_OFF above.
if(TARGET platform_mk66_power_hw)
  target_compile_definitions(platform_mk66_power_hw PRIVATE O3SDK_POWER_VLPR_OFF=$<BOOL:${O3SDK_POWER_VLPR_OFF}>)
endif()
# The console's hardware half names the generated build identity (the banner and `version` print O3SDK_IMAGE_LABEL).
if(TARGET platform_mk66_console_hw)
  target_include_directories(platform_mk66_console_hw PRIVATE ${_mk66_gen_dir})
  add_dependencies(platform_mk66_console_hw o3sdk_build_info)
endif()
# The reset sequence is the C runtime: its copy and zero loops must stay loops, not become memcpy / memset calls.
set_source_files_properties(src/startup.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)

# The SRAM_L code region the vector table may point into (the audio ISRs run from `.ramfunc_l`): mem_map.h kSramLBase and
# kRamfuncLBudget, pinned by mk66_mem_map_test.cpp. Every other address outside flash is refused.
set(O3SDK_MK66_RAMFUNC_REGION "0x1FFF0000:0x8000" CACHE INTERNAL "mem_map.h kSramLBase:kRamfuncLBudget")
set(O3SDK_MK66_LD "${_mk66_ld}" CACHE INTERNAL "the generated linker script")

# o3sdk_mk66_image(<name> <app library>): the firmware image of one app, in the app's binary directory. It runs in the caller's directory,
# which may belong to a project outside o3sdk: the tools are found next to this file, the python is O3SDK_PYTHON (a cache variable).
function(o3sdk_mk66_image name app_target)
  set(tools "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tools")
  set(out "${CMAKE_CURRENT_BINARY_DIR}")
  add_executable(${name})
  target_link_libraries(${name} PRIVATE platform_mk66_image)
  if(NOT O3SDK_MK66_SKELETON)
    target_link_libraries(${name} PRIVATE platform_mk66_display_hw platform_mk66_input_hw platform_mk66_audio_hw)
    if(O3SDK_MK66_CONSOLE)
      target_link_libraries(${name} PRIVATE platform_mk66_console_hw platform_mk66_usb_hw platform_mk66_storage_hw platform_mk66_update_hw platform_mk66_power_hw)
    endif()
  endif()
  # Whole archive: o3sdk_app() is the app library's entry point and only main names it.
  target_link_libraries(${name} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${app_target}>")
  set_target_properties(${name} PROPERTIES SUFFIX .elf)
  add_dependencies(${name} mk66_ld_script)
  set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${O3SDK_MK66_LD})
  # Our linker script and startup instead of newlib's crt0; newlib-nano and libstdc++ for the few library calls the image makes (no
  # libnosys: there is no heap, no stdio and no exit, and a library function that wants one is a link error, which is how "no heap"
  # is enforced). Unused sections are dropped; a section the script does not place is an error.
  target_link_options(${name} PRIVATE
    -nostartfiles -T${O3SDK_MK66_LD} --specs=nano.specs
    -Wl,--gc-sections -Wl,--orphan-handling=error -Wl,--print-memory-usage -Wl,-Map=${out}/${name}.map)

  # HEX, BIN and PTF behind the static checks. The HEX is an OUTPUT of a custom command, so a failed check (which deletes the HEX)
  # is repeated by the next build instead of being skipped as "up to date"; the PTF is removed first and copied only from a HEX
  # that passed. --set-start=0: no start-address record (the check refuses one). --gap-fill 0xFF: the linker script asserts there
  # is no hole; this fills one if it ever appeared, and the check (`contiguous`) refuses a hole either way.
  set(hex "${out}/${name}.hex")
  set(bin "${out}/${name}.bin")
  set(ptf "${out}/${name}.ptf")
  set(ramfunc_args "")
  if(O3SDK_MK66_SKELETON)
    set(ramfunc_args --allow-empty)
  endif()
  add_custom_command(OUTPUT ${hex} ${ptf}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${ptf}
    COMMAND ${CMAKE_SIZE} -A -d $<TARGET_FILE:${name}>
    COMMAND ${O3SDK_PYTHON} -I ${tools}/check_ramfunc.py check $<TARGET_FILE:${name}> --objdump ${CMAKE_OBJDUMP} ${ramfunc_args}
    COMMAND ${CMAKE_OBJCOPY} -O ihex --set-start=0 --gap-fill 0xFF $<TARGET_FILE:${name}> ${hex}
    COMMAND ${O3SDK_PYTHON} -I ${tools}/check_image.py check ${hex} --expect-sp 0x20030000 --ramfunc-l ${O3SDK_MK66_RAMFUNC_REGION}
            --delete-on-fail --json ${out}/${name}.image-check.json
    COMMAND ${CMAKE_COMMAND} -E copy ${hex} ${ptf}
    DEPENDS ${name} ${tools}/check_image.py ${tools}/check_ramfunc.py
    COMMENT "${name}.hex and ${name}.ptf behind the static image checks"
    VERBATIM)
  add_custom_command(OUTPUT ${bin}
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${bin}
    DEPENDS ${hex}
    COMMENT "${name}.bin"
    VERBATIM)
  add_custom_target(${name}_image ALL DEPENDS ${hex} ${ptf} ${bin})

  # ctest: the static checks of the built image. `<name>_flashconfig` carries its own copy of the required 16 bytes: a wrong flash
  # configuration field can permanently lock the MCU, so the post-build check and this test must not share a constant.
  add_test(NAME ${name}_image COMMAND ${O3SDK_PYTHON} -I ${tools}/check_image.py check ${hex} --expect-sp 0x20030000
           --ramfunc-l ${O3SDK_MK66_RAMFUNC_REGION})
  add_test(NAME ${name}_flashconfig COMMAND ${O3SDK_PYTHON} -I ${tools}/check_image.py flashconfig ${ptf}
           --expect ffffffffffffffffffffffffdef9ffff)
  add_test(NAME ${name}_ramfunc COMMAND ${O3SDK_PYTHON} -I ${tools}/check_ramfunc.py check $<TARGET_FILE:${name}>
           --objdump ${CMAKE_OBJDUMP} ${ramfunc_args})
  set_tests_properties(${name}_image ${name}_flashconfig ${name}_ramfunc PROPERTIES TIMEOUT 60 LABELS mk66)
  # An image with the USB console: the interrupt vector of USB0 (16 + 53) resolves to USB0_IRQHandler and not to the default handler, the buffer descriptor table is
  # 512-aligned, and the USB identity string is in the image (tools/check_usb_image.py).
  if(NOT O3SDK_MK66_SKELETON AND O3SDK_MK66_CONSOLE)
    add_test(NAME ${name}_usb COMMAND ${O3SDK_PYTHON} -I ${tools}/check_usb_image.py check $<TARGET_FILE:${name}> --nm ${CMAKE_NM})
    set_tests_properties(${name}_usb PROPERTIES TIMEOUT 60 LABELS mk66)
  endif()
endfunction()

# ---- ctest: the generated linker script and the noinit guard (o3sdk's own tests) --------------------------------------------
if(O3SDK_BUILD_TESTS)
  add_test(NAME mk66_ld COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py check
           --header ${_mk66_header} --template ${_mk66_template} --ld ${_mk66_ld})
  # The placement macros (sections.h, and O3SDK_BIG of kit/core placement.h) accept what they should and refuse an initialised object
  # at compile time.
  add_test(NAME mk66_noinit_guard
    COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
            "-DFLAGS=-mthumb|-mcpu=cortex-m4|-mfpu=fpv4-sp-d16|-mfloat-abi=hard|-std=gnu++17|-fno-rtti|-fno-exceptions"
            -DINCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/include -DCORE_INCLUDE=${PROJECT_SOURCE_DIR}/kit/core/include
            -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests -DWORK=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/noinit_guard.cmake)
  set_tests_properties(mk66_ld mk66_noinit_guard PROPERTIES TIMEOUT 60 LABELS mk66)
endif()
