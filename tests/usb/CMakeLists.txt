# SPDX-License-Identifier: GPL-3.0-or-later
# The USB library's tests (kit/usb): track3r-os's tests/usb_*, rename only (usb_return_sync_test.cpp is not ported: it tests the
# tracker's settings). One executable, one ctest per source file; the helpers (usb_rig.h, usb_fake_controller.h, ...) are headers
# beside the tests. usb_sim_test.cpp (millions of simulated SOFs) is the slow one, about 6 s on the host; kit/usb is compiled
# -O2 in a host Debug build so the sanitizer build stays usable.
find_package(Threads REQUIRED)  # usb_ring_thread_test.cpp and usb_diag_log_thread_test.cpp run a writer and a reader in std::threads
file(GLOB usb_test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/usb_*_test.cpp)
o3sdk_test_suite(o3sdk_usb_tests SOURCES ${usb_test_sources} DEPENDS usb Threads::Threads)
# A source file that is renamed or empties out must not pass by matching no case (doctest exits 0 on "test cases: 0").
foreach(src IN LISTS usb_test_sources)
  get_filename_component(test ${src} NAME_WE)
  set_tests_properties(${test} PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 ")
endforeach()
