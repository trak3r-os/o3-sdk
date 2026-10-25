// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/fixed_string.h>

#include <cstddef>
#include <cstring>

using o3sdk::core::FixedString;

static_assert(FixedString<8>::capacity() == 8, "capacity excludes the terminator");

TEST_CASE("fixed_string: starts as the empty string") {
  FixedString<4> s;
  CHECK(s.empty());
  CHECK(s.size() == 0u);
  CHECK(std::strcmp(s.c_str(), "") == 0);
}

TEST_CASE("fixed_string: assign and append that fit return true") {
  FixedString<8> s;
  CHECK(s.assign("abc"));
  CHECK(s.size() == 3u);
  CHECK(std::strcmp(s.c_str(), "abc") == 0);
  CHECK(s.append("de"));
  CHECK(s.append("f", 1));
  CHECK(std::strcmp(s.c_str(), "abcdef") == 0);
  CHECK(s.append("gh", 2));
  CHECK(s.size() == 8u);  // exactly full is still a fit
  CHECK(std::strcmp(s.c_str(), "abcdefgh") == 0);
  CHECK(s[0] == 'a');
  CHECK(s[7] == 'h');
}

TEST_CASE("fixed_string: text that does not fit is truncated, reported, and still terminated") {
  FixedString<5> s;
  CHECK_FALSE(s.assign("abcdefgh"));
  CHECK(s.size() == 5u);
  CHECK(std::strcmp(s.c_str(), "abcde") == 0);

  CHECK(s.assign("abc"));
  CHECK_FALSE(s.append("defg"));  // two chars of room, four wanted
  CHECK(std::strcmp(s.c_str(), "abcde") == 0);
  CHECK_FALSE(s.append("x", 1));  // already full
  CHECK_FALSE(s.append("z"));
  CHECK(s.size() == 5u);
  CHECK(std::strcmp(s.c_str(), "abcde") == 0);

  CHECK(s.assign("12345"));  // exact fit
  CHECK_FALSE(s.assign("123456"));  // one over
  CHECK(std::strcmp(s.c_str(), "12345") == 0);
}

TEST_CASE("fixed_string: append(s, n) copies exactly n bytes and truncates like the others") {
  FixedString<4> s;
  CHECK(s.append("abcdef", 3));
  CHECK(std::strcmp(s.c_str(), "abc") == 0);
  CHECK_FALSE(s.append("xyz", 3));
  CHECK(std::strcmp(s.c_str(), "abcx") == 0);
  s.clear();
  CHECK_FALSE(s.append("ab\0cd", 5));  // 5 bytes with an embedded NUL into 4 characters
  CHECK(s.size() == 4u);
}

TEST_CASE("fixed_string: null pointers are empty strings, zero capacity works") {
  FixedString<3> s;
  CHECK(s.assign("ab"));
  CHECK(s.append(static_cast<const char*>(nullptr)));
  CHECK(s.append(nullptr, 4));
  CHECK(std::strcmp(s.c_str(), "ab") == 0);

  FixedString<0> z;
  CHECK(z.append(""));
  CHECK_FALSE(z.append("a"));
  CHECK_FALSE(z.append("a", 1));
  CHECK(z.size() == 0u);
  CHECK(std::strcmp(z.c_str(), "") == 0);
}

TEST_CASE("fixed_string: append_z never reads past the room it has plus one byte") {
  // `source` has no terminator. Capacity 4 means at most 5 reads, all inside the array; an unbounded
  // strlen would read source[5], which ASan reports as a stack-buffer-overflow and aborts the test.
  const char source[5] = {'a', 'b', 'c', 'd', 'e'};
  FixedString<4> s;
  CHECK_FALSE(s.append(source));
  CHECK(std::strcmp(s.c_str(), "abcd") == 0);
}

TEST_CASE("fixed_string: clear") {
  FixedString<4> s;
  CHECK(s.assign("abcd"));
  s.clear();
  CHECK(s.empty());
  CHECK(std::strcmp(s.c_str(), "") == 0);
  CHECK(s.assign("x"));
  CHECK(std::strcmp(s.c_str(), "x") == 0);
}

TEST_CASE("fixed_string: comparison with C strings") {
  FixedString<8> a;
  CHECK(a.assign("abc"));
  CHECK(a == "abc");
  CHECK(a != "abd");
  CHECK(a != "ab");
  CHECK(a != "abcd");
  CHECK(a != static_cast<const char*>(nullptr));
  FixedString<8> embedded;
  CHECK(embedded.append("ab\0c", 4));  // an interior NUL: not equal to the C string "ab"
  CHECK(embedded != "ab");
  FixedString<8> empty;
  CHECK(empty == "");
  CHECK(empty != "a");
}
