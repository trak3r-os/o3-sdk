# SPDX-License-Identifier: GPL-3.0-or-later
# Host test of the card's writer (doctest), label `mk66`: track3r-os's mk66_save_writer_test.cpp, the writer over the software SD card of the storage tests (their fakes
# and need.h are platform/mk66/tests'). The save job over a FAT32 image is tests/save_fatfs_test.cpp.
o3sdk_test_suite(mk66_save_tests SOURCES mk66_save_writer_test.cpp DEPENDS platform_mk66_save)
target_include_directories(mk66_save_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests)
set_tests_properties(mk66_save_writer_test PROPERTIES LABELS mk66)
