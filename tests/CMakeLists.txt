# SPDX-License-Identifier: GPL-3.0-or-later
# Unit tests (doctest, host only): every *_test.cpp in this directory, one executable, one ctest per source file.
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(NOT TARGET app_hello)  # O3SDK_BUILD_APPS off: no HELLO to run
  list(REMOVE_ITEM test_sources ${CMAKE_CURRENT_SOURCE_DIR}/sim_hello_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_midi_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_files_test.cpp)
endif()
add_executable(o3sdk_tests main.cpp ${test_sources})
target_include_directories(o3sdk_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_include_directories(o3sdk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # <doctest/doctest.h>
target_link_libraries(o3sdk_tests PRIVATE app sim platform_host unit_synth unit_delay)
# The save machinery's tests (docs/M2C-PLAN.md, slice FS): formats (wav_test), the FatFs writer and the card on a FAT32 image (save_fatfs_test, fat_fs_storage_test).
# (the tests build only with O3SDK_BUILD_TESTS, which also configures platform/mk66 and so platform_host_fat)
target_link_libraries(o3sdk_tests PRIVATE formats platform_host_fat platform_mk66_save platform_mk66_storage)
# The hello app end to end (sim_hello_test.cpp, sim_midi_test.cpp, sim_files_test.cpp): its library defines o3sdk_app(), which only the tests name. The second
# and the third run apps/hello/scenarios/midi.txt and files.txt, which they find through O3SDK_HELLO_SCENARIOS.
if(TARGET app_hello)
  target_link_libraries(o3sdk_tests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_hello>")
  get_filename_component(hello_scenarios "${CMAKE_CURRENT_SOURCE_DIR}/../apps/hello/scenarios" ABSOLUTE)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sim_midi_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/sim_files_test.cpp ${CMAKE_CURRENT_SOURCE_DIR}/app_audio_in_test.cpp PROPERTIES COMPILE_DEFINITIONS "O3SDK_HELLO_SCENARIOS=\"${hello_scenarios}\"")
endif()
o3sdk_strict_warnings(o3sdk_tests)
# The tree builds without exceptions: REQUIRE then ends the run with std::terminate on a failure, which ctest reports.
target_compile_definitions(o3sdk_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
foreach(src IN LISTS test_sources)
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME ${name} COMMAND o3sdk_tests --source-file=*${name}.cpp)
endforeach()
add_subdirectory(sim_refusal)  # bad_setup_sim: the runner on an app whose setup was refused (exit 1 and a message)
add_subdirectory(logo_apps)  # apps with their own boot logo (LOGO, LOGO_COLOR): a colour one and a tinted grey one
add_subdirectory(usb)  # o3sdk_usb_tests: the USB library's suite, ported from track3r-os
