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
mk66_suite(mk66_input_tests platform_mk66_input mk66_input_*_test.cpp)
mk66_suite(mk66_audio_tests platform_mk66_audio
  mk66_audio_config_test.cpp mk66_sgtl5000_test.cpp mk66_output_gate_test.cpp audio_*_test.cpp)
# audio_status_thread_test.cpp: std::thread needs the platform's thread library (pthread on Linux).
if(TARGET mk66_audio_tests)
  target_link_libraries(mk66_audio_tests PRIVATE Threads::Threads)
endif()
# mk66_input_map_test.cpp checks the scanner's map against the kit's panel.h (app::Button, the counts, the encoder id): the test
# links `app`, the platform's input library does not depend on it.
if(TARGET mk66_input_tests)
  target_link_libraries(mk66_input_tests PRIVATE app)
endif()
