// SPDX-License-Identifier: GPL-3.0-or-later
// Single translation unit that provides doctest's main(); the test cases live in the other *_test.cpp files.
//
// O3SDK_TEST_SHARD="i/n" (CMake: set_tests_properties(... ENVIRONMENT), see tests/CMakeLists.txt of an app): this process runs the i-th of n
// shares of every source file's test cases. The share of a test case is its index in the file, by line, mod n, so the n shares of a file are
// disjoint and their union is the file, whatever the names hold; --list-test-cases (-ltc) with the variable set prints one share, and the
// union of the n lists against the list with no variable is the proof of coverage. Unset, or malformed: everything runs.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {

void apply_shard() {
  const char* v = std::getenv("O3SDK_TEST_SHARD");
  unsigned i = 0;
  unsigned n = 0;
  if (v == nullptr || std::sscanf(v, "%u/%u", &i, &n) != 2 || n < 2u || i >= n) {
    return;
  }
  std::map<std::string, unsigned> index;  // per source file; the registry is ordered by file, then line
  for (const auto& tc : doctest::detail::getRegisteredTests()) {
    const unsigned k = index[tc.m_file.c_str()]++;
    if (k % n != i) {
      const_cast<doctest::detail::TestCase&>(tc).m_skip = true;  // the registry's order does not depend on it
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  apply_shard();
  doctest::Context context;
  context.applyCommandLine(argc, argv);
  return context.run();
}
