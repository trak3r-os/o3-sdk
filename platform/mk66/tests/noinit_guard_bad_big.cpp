// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fixture of ctest `mk66_noinit_guard`: a non-zero initialiser on an O3SDK_BIG object. Must NOT compile: the object lives in the
// NOLOAD `.sdram_bss` output section, which keeps no contents, so the value would be silently all zeros on the unit. O3SDK_BIG names
// the input section `.bss.sdram_bss` (NOBITS) and the compiler refuses ("only zero initializers are allowed in section").
#include <o3sdk/core/placement.h>

O3SDK_BIG float g_table[8] = {1.f};
