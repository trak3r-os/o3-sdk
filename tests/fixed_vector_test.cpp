// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3sdk/core/fixed_vector.h>

#include <cstddef>
#include <cstdint>

using o3sdk::core::FixedVector;

namespace {
constexpr int sum_of_pushed() {
  FixedVector<int, 4> v;
  (void)v.push_back(3);
  (void)v.push_back(4);
  int total = 0;
  for (int x : v) {
    total += x;
  }
  return total;
}
}  // namespace

static_assert(FixedVector<int, 4>::capacity() == 4, "capacity is the template argument");
static_assert(sum_of_pushed() == 7, "usable in constant expressions");

TEST_CASE("fixed_vector: starts empty") {
  FixedVector<int, 3> v;
  CHECK(v.empty());
  CHECK_FALSE(v.full());
  CHECK(v.size() == 0u);
  CHECK(v.begin() == v.end());
}

TEST_CASE("fixed_vector: push_back until full, then it refuses and changes nothing") {
  FixedVector<int, 3> v;
  CHECK(v.push_back(1));
  CHECK(v.push_back(2));
  CHECK(v.push_back(3));
  CHECK(v.full());
  CHECK_FALSE(v.push_back(4));
  CHECK(v.size() == 3u);
  CHECK(v[0] == 1);
  CHECK(v[1] == 2);
  CHECK(v[2] == 3);
  CHECK(v.front() == 1);
  CHECK(v.back() == 3);
}

TEST_CASE("fixed_vector: pop_back, clear and reuse") {
  FixedVector<int, 2> v;
  CHECK_FALSE(v.pop_back());  // empty
  CHECK(v.push_back(7));
  CHECK(v.push_back(8));
  CHECK(v.pop_back());
  CHECK(v.size() == 1u);
  CHECK(v.back() == 7);
  CHECK(v.push_back(9));
  CHECK(v.back() == 9);
  v.clear();
  CHECK(v.empty());
  CHECK(v.push_back(5));
  CHECK(v[0] == 5);
}

TEST_CASE("fixed_vector: iteration covers exactly the live items") {
  FixedVector<std::uint8_t, 8> v;
  for (std::uint8_t i = 0; i < 5; ++i) {
    CHECK(v.push_back(static_cast<std::uint8_t>(i * 2)));
  }
  std::size_t count = 0;
  int total = 0;
  for (std::uint8_t x : v) {
    total += x;
    ++count;
  }
  CHECK(count == 5u);
  CHECK(total == 0 + 2 + 4 + 6 + 8);
  CHECK(v.end() - v.begin() == 5);
}

TEST_CASE("fixed_vector: structs and copies") {
  struct P {
    int x;
    int y;
  };
  FixedVector<P, 2> a;
  CHECK(a.push_back(P{1, 2}));
  FixedVector<P, 2> b = a;  // trivially copyable: a plain copy of the storage
  CHECK(a.push_back(P{3, 4}));
  CHECK(b.size() == 1u);
  CHECK(b[0].y == 2);
  CHECK(a.size() == 2u);
  CHECK(a[1].x == 3);
}
