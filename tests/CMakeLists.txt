# SPDX-License-Identifier: GPL-3.0-or-later
# Unit tests (doctest, host only): every *_test.cpp in this directory, one executable, one ctest per source file.
file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
if(NOT TARGET app_hello)  # O3SDK_BUILD_APPS off: no HELLO to run
  list(REMOVE_ITEM test_sources ${CMAKE_CURRENT_SOURCE_DIR}/sim_hello_test.cpp)
endif()
add_executable(o3sdk_tests main.cpp ${test_sources})
target_include_directories(o3sdk_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
target_include_directories(o3sdk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # <doctest/doctest.h>
target_link_libraries(o3sdk_tests PRIVATE app sim platform_host unit_synth unit_delay)
# The hello app end to end (sim_hello_test.cpp): its library defines o3sdk_app(), which only the test names.
if(TARGET app_hello)
  target_link_libraries(o3sdk_tests PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,app_hello>")
endif()
o3sdk_strict_warnings(o3sdk_tests)
# The tree builds without exceptions: REQUIRE then ends the run with std::terminate on a failure, which ctest reports.
target_compile_definitions(o3sdk_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
foreach(src IN LISTS test_sources)
  get_filename_component(name ${src} NAME_WE)
  add_test(NAME ${name} COMMAND o3sdk_tests --source-file=*${name}.cpp)
endforeach()
add_subdirectory(usb)  # o3sdk_usb_tests: the USB library's suite, ported from track3r-os
