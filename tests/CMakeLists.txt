# SPDX-License-Identifier: GPL-3.0-or-later
# Unit tests (doctest, host only): every *_test.cpp in this directory, one executable, one ctest per source file.
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(NOT TARGET app_hello)  # O3SDK_BUILD_APPS off: no HELLO to run
  list(REMOVE_ITEM test_sources ${CMAKE_CURRENT_SOURCE_DIR}/sim_hello_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_midi_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_files_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_record_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_usb_test.cpp)
endif()
add_executable(o3sdk_tests main.cpp ${test_sources})
target_include_directories(o3sdk_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_include_directories(o3sdk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # <doctest/doctest.h>
target_link_libraries(o3sdk_tests PRIVATE app sim platform_host unit_synth unit_delay)
# The save machinery's tests (docs/M2C-PLAN.md, slice FS): formats (wav_test), the FatFs writer and the card on a FAT32 image (save_fatfs_test, fat_fs_storage_test).
# (the tests build only with O3SDK_BUILD_TESTS, which also configures platform/mk66 and so platform_host_fat)
target_link_libraries(o3sdk_tests PRIVATE formats platform_host_fat platform_mk66_save platform_mk66_storage)
# The hello app end to end (sim_hello_test.cpp, sim_midi_test.cpp, sim_files_test.cpp, sim_record_test.cpp, sim_usb_test.cpp): its library defines o3sdk_app(), which only the tests name. The second
# to the fourth run apps/hello/scenarios/midi.txt, files.txt and record.txt, which they find through O3SDK_HELLO_SCENARIOS (app_config_pages_test.cpp runs config_pages.txt and picker.txt).
if(TARGET app_hello)
  target_link_libraries(o3sdk_tests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_hello>")
  get_filename_component(hello_scenarios "${CMAKE_CURRENT_SOURCE_DIR}/../apps/hello/scenarios" ABSOLUTE)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sim_midi_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_files_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_record_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_usb_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/app_audio_in_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/app_config_pages_test.cpp PROPERTIES COMPILE_DEFINITIONS "O3SDK_HELLO_SCENARIOS=\"${hello_scenarios}\"")
endif()
o3sdk_strict_warnings(o3sdk_tests)
# raster_line_load_test.cpp ties the line-load counter to the fill-rate band (apps/fillrate/src/band.h, pure logic: the app's own header).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/raster_line_load_test.cpp PROPERTIES INCLUDE_DIRECTORIES ${PROJECT_SOURCE_DIR}/apps/fillrate/src)
# boot_screen_test.cpp compares the boot frames with tests/digests/boot_screen.txt.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/boot_screen_test.cpp PROPERTIES COMPILE_DEFINITIONS "O3SDK_DIGESTS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/digests\"")
# The tree builds without exceptions: REQUIRE then ends the run with std::terminate on a failure, which ctest reports.
target_compile_definitions(o3sdk_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
foreach(src IN LISTS test_sources)
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME ${name} COMMAND o3sdk_tests --source-file=*${name}.cpp)
endforeach()
add_subdirectory(sim_refusal)  # bad_setup_sim: the runner on an app whose setup was refused (exit 1 and a message)
# Pinned frames (tools/digest_check.py, tests/digests/<app>.txt): the simulator writes the FNV-1a of the display-list words of every picture a
# scenario takes (digests.txt); each ctest runs one app's scenarios and compares them with the pin file. A change that moves a digest fails
# until the pin file is regenerated (`digest_check.py ... --update`) and reviewed with the change. The kit's apps have no handle 0: a run that
# uses it fails too (ui/plot/canvas.h). The template app's pins are checked by templates/check_template.py; the boot screen's by boot_screen_test.
# o3sdk_digest_test(<ctest name> <app>_sim <pin file> --run 'NAME|SCENARIO|ARGS'...): digest_check.py explains the run syntax.
function(o3sdk_digest_test name sim pins)
  add_test(NAME ${name} COMMAND ${O3SDK_PYTHON} -I ${PROJECT_SOURCE_DIR}/tools/digest_check.py --sim $<TARGET_FILE:${sim}> --pins ${pins} ${ARGN})
  set_tests_properties(${name} PROPERTIES TIMEOUT 300 LABELS digest)
endfunction()
add_test(NAME digest_check_selftest COMMAND ${O3SDK_PYTHON} -I ${PROJECT_SOURCE_DIR}/tools/digest_check.py --self-test)
set_tests_properties(digest_check_selftest PROPERTIES TIMEOUT 60 LABELS digest)
set(_digests ${PROJECT_SOURCE_DIR}/tests/digests)
if(TARGET hello_sim)
  set(_hs ${PROJECT_SOURCE_DIR}/apps/hello/scenarios)
  o3sdk_digest_test(digest_hello hello_sim ${_digests}/hello.txt
    "--run=audio_in_absent|${_hs}/audio_in_absent.txt"
    "--run=audio_in|${_hs}/audio_in.txt|--wav {wav}"
    "--run=boot|${_hs}/boot.txt"
    "--run=config|${_hs}/config.txt|--firmware-dir ${_hs}/firmware"
    "--run=config_pages|${_hs}/config_pages.txt|--card {card} --audio-in"
    "--run=files|${_hs}/files.txt|--card {card}"
    "--run=midi|${_hs}/midi.txt"
    "--run=picker|${_hs}/picker.txt|--card {card}"
    "--run=record|${_hs}/record.txt|--card {card} --wav {wav}"
    "--run=tour|${_hs}/tour.txt"
    "--run=usb|${_hs}/usb.txt|--card {card} --wav {wav}")
endif()
if(TARGET crashtest_sim)
  o3sdk_digest_test(digest_crashtest crashtest_sim ${_digests}/crashtest.txt
    "--run=screen|${PROJECT_SOURCE_DIR}/apps/crashtest/scenarios/screen.txt")
endif()
if(TARGET fillrate_sim)
  o3sdk_digest_test(digest_fillrate fillrate_sim ${_digests}/fillrate.txt
    "--run=tour|${PROJECT_SOURCE_DIR}/apps/fillrate/scenarios/tour.txt")
endif()
add_subdirectory(logo_apps)  # apps with their own boot logo (LOGO, LOGO_COLOR): a colour one and a tinted grey one
add_subdirectory(usb)  # o3sdk_usb_tests: the USB library's suite, ported from track3r-os
