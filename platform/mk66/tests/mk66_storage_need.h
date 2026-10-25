// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <doctest/doctest.h>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: NEED(cond) checks the condition once and leaves the current void
// function (the test case) when it is false. The same macro as tests/support/need.h, which a platform test may not include (the layer check:
// platform must not include tests); guarded, so a host test that includes both gets one.
#ifndef NEED
#define NEED(...)                                      \
  do {                                                 \
    const bool o3_need_ok = static_cast<bool>(__VA_ARGS__); \
    CHECK_MESSAGE(o3_need_ok, #__VA_ARGS__);           \
    if (!o3_need_ok) {                                 \
      return;                                          \
    }                                                  \
  } while (0)
#endif
