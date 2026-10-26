# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests of the MIDI timing core (doctest): one executable, one ctest per source file, label `mk66`. The cases are track3r-os's `midi` suite (tests/midi_*_test.cpp, the
# DIN emitter's fuzzed streams and lateness bounds, USB out, the router, MIDI in, the clock-follow loop against simulated masters) and mk66_midi_config_test.cpp, which
# pins midi_config.h's register values against the board, the clock and the IRQ map.
o3sdk_test_suite(mk66_midi_tests
  SOURCES
    midi_core_test.cpp midi_din_test.cpp midi_usb_test.cpp midi_router_test.cpp midi_in_test.cpp midi_cc_in_test.cpp midi_clock_dll_test.cpp midi_io_test.cpp
    mk66_midi_config_test.cpp
  DEPENDS platform_mk66_midi)
set_tests_properties(midi_core_test midi_din_test midi_usb_test midi_router_test midi_in_test midi_cc_in_test midi_clock_dll_test midi_io_test mk66_midi_config_test
  PROPERTIES LABELS mk66)
