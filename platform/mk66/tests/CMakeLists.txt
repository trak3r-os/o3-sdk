# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests of the MK66 platform's pure parts (doctest): one executable per domain, one ctest per source file, label `mk66`.
find_package(Threads REQUIRED)  # audio_status_thread_test.cpp runs a writer and a reader in std::threads

function(mk66_suite name lib)
  set(sources "")
  foreach(pattern IN LISTS ARGN)
    file(GLOB found CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${pattern})
    list(APPEND sources ${found})
  endforeach()
  if(NOT sources)
    return()
  endif()
  add_executable(${name} ${PROJECT_SOURCE_DIR}/tests/main.cpp ${sources})
  target_include_directories(${name} SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(${name} PRIVATE ${lib})
  o3sdk_strict_warnings(${name})
  target_compile_definitions(${name} PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
  foreach(src IN LISTS sources)
    get_filename_component(test ${src} NAME_WE)
    add_test(NAME ${test} COMMAND ${name} --source-file=*${test}.cpp)
    set_tests_properties(${test} PROPERTIES LABELS mk66)
  endforeach()
endfunction()

mk66_suite(mk66_base_tests platform_mk66_base
  mk66_mem_map_test.cpp mk66_crash_record_test.cpp mk66_memtest_test.cpp mk66_sdramc_config_test.cpp mk66_flash_config_test.cpp
  mk66_lmem_test.cpp mk66_blink_irq_test.cpp mk66_resources_test.cpp mk66_idle_hook_test.cpp)
mk66_suite(mk66_display_tests platform_mk66_display mk66_display_*_test.cpp)
# (mk66_input_hold_test.cpp tests usb/input_hold.h and is in mk66_usb_tests below, so the glob names the others.)
mk66_suite(mk66_input_tests platform_mk66_input
  mk66_input_filter_test.cpp mk66_input_leds_test.cpp mk66_input_map_test.cpp mk66_input_padbus_test.cpp mk66_input_pins_test.cpp mk66_input_scanner_test.cpp)
# (mk66_storage_swap_test.cpp drives the firmware upload through the update path's FwService: the storage suite links the update library, which brings storage.)
mk66_suite(mk66_storage_tests platform_mk66_update mk66_storage_*_test.cpp)
mk66_suite(mk66_update_tests platform_mk66_update mk66_update_*_test.cpp)
# (M2c slice AP, ported from track3r-os: audio_kernels, audio_test_signal, audio_stem_map, audio_taps (its cases that do not drive the Graph) and mk66_si4703_test, which tests the
# Si4703 driver of audio/ against a model of the chip. M2c slice RC adds record_service_test (the record service on a real engine's transport). support/static_slot.h is track3r-os's tests/support one: a Cortex-M4 test stack is small.)
mk66_suite(mk66_audio_tests platform_mk66_audio
  mk66_audio_config_test.cpp mk66_sgtl5000_test.cpp mk66_output_gate_test.cpp mk66_si4703_test.cpp audio_*_test.cpp record_service_test.cpp)
if(TARGET mk66_audio_tests)
  # The taps call the kit's usb kernels (sum_slots, interleave) and the stem map reads its profiles: platform_mk66_audio has usb's headers, not the library (audio/CMakeLists.txt).
  target_link_libraries(mk66_audio_tests PRIVATE usb)
  # The kernels' property test runs every kernel against its reference over many random blocks: slow under the sanitizers.
  set_tests_properties(audio_kernels_test PROPERTIES TIMEOUT 300)
endif()
# audio_status_thread_test.cpp: std::thread needs the platform's thread library (pthread on Linux).
if(TARGET mk66_audio_tests)
  target_link_libraries(mk66_audio_tests PRIVATE Threads::Threads)
endif()
# mk66_input_map_test.cpp checks the scanner's map against the kit's panel.h (app::Button, the counts, the encoder id): the test
# links `app`, the platform's input library does not depend on it.
if(TARGET mk66_input_tests)
  target_link_libraries(mk66_input_tests PRIVATE app)
endif()
# The console's pure part: the table and its rules (mk66_console_core_test.cpp), `state` over a real Runtime on the host fakes (platform_host),
# the USB mode guard and the crash-loop rule (usb_mode_guard_test.cpp).
mk66_suite(mk66_console_tests platform_mk66_console mk66_console_*_test.cpp usb_mode_guard_test.cpp)
if(TARGET mk66_console_tests)
  target_link_libraries(mk66_console_tests PRIVATE platform_host)
  # mk66_console_e2e_test.cpp runs the real shell and pump over the device and its fake controller: the rig and the fake controller are the kit's tests' (tests/usb).
  target_include_directories(mk66_console_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/usb)
endif()
# The Power switch's shutdown (power/): the state machine over a PowerHost, the VLPR off's sequence and poll loop over an OffPlatform, the record and the `power` report; the very code the image runs.
mk66_suite(mk66_power_tests platform_mk66_power mk66_power_*_test.cpp)
# The USB driver's pure parts (usb/): the SoC facts, the diag text, the injector, the input hold, the peek/poke lists, the `shot` stream, the real shell + pump + log pager over the
# device and its fake controller (mk66_usb_console_test.cpp), the USB audio tap and the boot layout's descriptors. The fake controller and the rig are the kit's tests' (tests/usb).
mk66_suite(mk66_usb_tests platform_mk66_usb
  mk66_usb_*_test.cpp mk66_hil_inject_test.cpp mk66_input_hold_test.cpp mk66_mem_access_test.cpp mk66_shot_stream_test.cpp)
if(TARGET mk66_usb_tests)
  target_include_directories(mk66_usb_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests/usb)
endif()
