// SPDX-License-Identifier: GPL-3.0-or-later
// OS-010a: the folder browser's listing (app/dir_index.h): its order, its fixed capacity and what happens at the edges of it, the stable keys the cursor
// follows, the letter groups, hostile names. Pure: no storage, no heap (it also runs on the Cortex-M4F, the qemu suite `dir_index`).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/app/dir_index.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using o3sdk::app::DirIndex;

namespace {
constexpr std::size_t kNameCap = o3sdk::hal::storage::kNameMax;  // 255
}

namespace {

DirIndex& fresh() {
  static DirIndex index;  // 9 KB: not on a small stack
  index.clear();
  return index;
}

// The order, stated apart from the code under test: folders first, then the names with the ASCII letters folded, then the raw bytes.
void folded(const char* in, char* out) {
  std::size_t i = 0;
  for (; in[i] != '\0'; ++i) {
    const char c = in[i];
    out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  }
  out[i] = '\0';
}
bool oracle_before(bool a_dir, const char* a, bool b_dir, const char* b) {
  if (a_dir != b_dir) {
    return a_dir;
  }
  char fa[300];
  char fb[300];
  folded(a, fa);
  folded(b, fb);
  const int c = std::strcmp(fa, fb);  // strcmp compares unsigned bytes
  return c != 0 ? c < 0 : std::strcmp(a, b) < 0;
}

struct Item {
  const char* name;
  bool dir;
};

std::uint32_t lcg(std::uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

}  // namespace

TEST_CASE("dir_index: folders first, then names compared without regard to case") {
  DirIndex& ix = fresh();
  const Item items[] = {{"zebra", false}, {"Alpha", true}, {"beta.wav", false}, {"Charlie", true}, {"alpha2", true}, {"BETA", false}, {"10", false}, {"9", true}, {"_x", false}};
  for (const Item& it : items) {
    CHECK(ix.add(it.name, it.dir) >= 0);
  }
  NEED(ix.count() == 9u);
  const char* want[] = {"9", "Alpha", "alpha2", "Charlie", "10", "_x", "BETA", "beta.wav", "zebra"};
  // folders: "9" < "alpha" < "alpha2" < "charlie"; files: '1' (0x31) < '_' (0x5F) < 'b' < "beta.wav" (a prefix first) < 'z'
  for (unsigned i = 0; i < ix.count(); ++i) {
    CHECK(std::strcmp(ix.name(i), want[i]) == 0);
  }
  CHECK(ix.is_dir(0));
  CHECK(ix.is_dir(3));
  CHECK_FALSE(ix.is_dir(4));
  CHECK(ix.name_length(1) == 5u);
}

TEST_CASE("dir_index: the order does not depend on the order the entries arrive in") {
  static const char* const names[] = {"kick",  "Kick2", "snare", "SNARE B", "hat",  "Hat open", "clap",  "x.wav", "X.WAV2", "-dash", "~tilde", "a",
                                      "A",     "b",     "B",     "caf\xC3\xA9", "caf\xC3\xA8", "\xFF", "9", "09", "Zed"};
  constexpr unsigned n = sizeof(names) / sizeof(names[0]);
  std::uint32_t seed = 12345;
  static char first[n][16];
  for (unsigned round = 0; round < 40; ++round) {
    unsigned order[n];
    for (unsigned i = 0; i < n; ++i) {
      order[i] = i;
    }
    for (unsigned i = n - 1; i > 0; --i) {  // Fisher-Yates
      const unsigned j = lcg(seed) % (i + 1u);
      const unsigned t = order[i];
      order[i] = order[j];
      order[j] = t;
    }
    DirIndex& ix = fresh();
    for (unsigned i = 0; i < n; ++i) {
      const unsigned k = order[i];
      NEED(ix.add(names[k], k % 4u == 0u) >= 0);  // every fourth is a folder
    }
    NEED(ix.count() == n);
    for (unsigned i = 1; i < n; ++i) {
      CHECK_FALSE(oracle_before(ix.is_dir(i), ix.name(i), ix.is_dir(i - 1), ix.name(i - 1)));  // never out of order
      CHECK(DirIndex::compare(ix.is_dir(i - 1), ix.name(i - 1), ix.is_dir(i), ix.name(i)) <= 0);
    }
    if (round == 0) {
      for (unsigned i = 0; i < n; ++i) {
        std::snprintf(first[i], sizeof first[i], "%s", ix.name(i));
      }
    } else {
      for (unsigned i = 0; i < n; ++i) {
        CHECK(std::strcmp(ix.name(i), first[i]) == 0);  // the same list whatever the arrival order
      }
    }
  }
}

TEST_CASE("dir_index: add reports where the entry went, and a listing that is read is sorted all along") {
  DirIndex& ix = fresh();
  const std::uint32_t g0 = ix.generation();
  CHECK(ix.add("m", true) == 0);
  CHECK(ix.add("z", true) == 1);
  CHECK(ix.add("a", true) == 0);
  CHECK(ix.add("n", true) == 2);
  CHECK(ix.add("file", false) == 4);  // after every folder
  CHECK(ix.generation() == g0 + 5u);
  CHECK(ix.add("", false) == DirIndex::kNone);  // refused: no change
  CHECK(ix.generation() == g0 + 5u);
  ix.clear();
  CHECK(ix.count() == 0u);
  CHECK(ix.generation() == g0 + 6u);
  CHECK_FALSE(ix.full());
}

TEST_CASE("dir_index: 256 entries fit and the 257th is refused, said so, and nothing else changes") {
  DirIndex& ix = fresh();
  char name[16];
  for (unsigned i = 0; i < DirIndex::kMaxEntries; ++i) {
    std::snprintf(name, sizeof name, "p%03u", i);
    NEED(ix.add(name, true) >= 0);
  }
  CHECK(ix.count() == 256u);
  CHECK_FALSE(ix.full());  // exactly full is not overflow
  const std::uint32_t g = ix.generation();
  CHECK(ix.add("p999", true) == DirIndex::kNone);
  CHECK(ix.full());
  CHECK(ix.count() == 256u);
  CHECK(ix.generation() == g);
  CHECK(std::strcmp(ix.name(0), "p000") == 0);
  CHECK(std::strcmp(ix.name(255), "p255") == 0);
  ix.clear();
  CHECK_FALSE(ix.full());
  CHECK(ix.add("again", false) == 0);
}

TEST_CASE("dir_index: the pool of names fills before the entries when the names are long, and no name is ever cut") {
  DirIndex& ix = fresh();
  static char name[kNameCap + 1];  // 255 bytes and the NUL
  // 32 names of 255 bytes take 32 * 256 = 8192 bytes: exactly the pool.
  for (unsigned i = 0; i < DirIndex::kPoolBytes / 256u; ++i) {
    std::memset(name, 'x', 255);
    std::snprintf(name, 4, "%03u", i);
    name[3] = 'x';  // snprintf wrote a NUL there
    name[255] = '\0';
    NEED(ix.add(name, true) >= 0);
  }
  CHECK(ix.count() == 32u);
  CHECK_FALSE(ix.full());
  CHECK(ix.add("a", true) == DirIndex::kNone);  // not one byte left, however short
  CHECK(ix.full());
  CHECK(ix.count() == 32u);
  for (unsigned i = 0; i < ix.count(); ++i) {
    CHECK(ix.name_length(i) == 255u);  // whole
    CHECK(std::strlen(ix.name(i)) == 255u);
  }
  CHECK(std::strncmp(ix.name(31), "031", 3) == 0);
}

TEST_CASE("dir_index: a name that cannot be an entry is refused without calling the list full") {
  DirIndex& ix = fresh();
  static char too_long[kNameCap + 2];
  std::memset(too_long, 'y', sizeof too_long - 1);
  too_long[sizeof too_long - 1] = '\0';  // 256 bytes
  CHECK(ix.add(too_long, false) == DirIndex::kNone);
  CHECK(ix.add("", false) == DirIndex::kNone);
  CHECK(ix.add(nullptr, false) == DirIndex::kNone);
  CHECK_FALSE(ix.full());
  CHECK(ix.count() == 0u);
  too_long[255] = '\0';  // 255 bytes: the longest a FAT name is
  CHECK(ix.add(too_long, false) == 0);
  CHECK(std::strcmp(ix.name(0), too_long) == 0);
}

TEST_CASE("dir_index: hostile names are kept as they are and sorted by their bytes") {
  DirIndex& ix = fresh();
  // ".wav" as a folder's name is a folder (PAR-033): the kind is what the card said, never what the name looks like.
  CHECK(ix.add("kick.wav", true) >= 0);
  CHECK(ix.add("a.wav", false) >= 0);
  CHECK(ix.add(".wav", true) >= 0);
  CHECK(ix.add("\xC3\xA9t\xC3\xA9", false) >= 0);  // "ete" with accents, UTF-8
  CHECK(ix.add("\xFF\xFE", false) >= 0);               // not UTF-8 at all
  CHECK(ix.add("tab\there", false) >= 0);               // a control byte
  CHECK(ix.add("A", false) >= 0);
  CHECK(ix.add("a", false) >= 0);                        // differs from "A" by case alone: still a deterministic order
  NEED(ix.count() == 8u);
  CHECK(std::strcmp(ix.name(0), ".wav") == 0);
  CHECK(ix.is_dir(0));
  CHECK(std::strcmp(ix.name(1), "kick.wav") == 0);
  CHECK(ix.is_dir(1));
  CHECK(std::strcmp(ix.name(2), "A") == 0);  // "A" and "a" fold equal: the raw bytes order them
  CHECK(std::strcmp(ix.name(3), "a") == 0);
  CHECK(std::strcmp(ix.name(4), "a.wav") == 0);
  CHECK(std::strcmp(ix.name(5), "tab\there") == 0);
  CHECK(std::strcmp(ix.name(6), "\xC3\xA9t\xC3\xA9") == 0);  // bytes of 0x80 and above after every ASCII byte
  CHECK(std::strcmp(ix.name(7), "\xFF\xFE") == 0);
  CHECK(DirIndex::compare(false, "A", false, "a") < 0);
  CHECK(DirIndex::compare(false, "a", false, "A") > 0);
  CHECK(DirIndex::compare(false, "same", false, "same") == 0);
  CHECK(DirIndex::compare(true, "z", false, "a") < 0);
  CHECK(std::strcmp(ix.name(100), "") == 0);  // outside the list: an empty name, never a wild pointer
  CHECK_FALSE(ix.is_dir(100));
  CHECK(ix.name_length(100) == 0u);
}

TEST_CASE("dir_index: a key follows its entry while others are inserted around it") {
  DirIndex& ix = fresh();
  (void)ix.add("b", false);
  (void)ix.add("d", false);
  const std::uint16_t kd = ix.key(1);
  CHECK(ix.find_key(kd) == 1);
  (void)ix.add("a", false);
  (void)ix.add("c", false);
  (void)ix.add("e", false);
  CHECK(std::strcmp(ix.name(static_cast<unsigned>(ix.find_key(kd))), "d") == 0);
  CHECK(ix.find_key(kd) == 3);
  CHECK(ix.key(99) == 0u);
  ix.clear();
  CHECK(ix.find_key(kd) == DirIndex::kNone);  // gone with the listing
}

TEST_CASE("dir_index: letter groups: next, previous, and find by letter") {
  DirIndex& ix = fresh();
  CHECK(ix.next_letter(0) == DirIndex::kNone);
  CHECK(ix.prev_letter(0) == DirIndex::kNone);
  CHECK(ix.find_letter('a') == DirIndex::kNone);
  const Item items[] = {{"alpha", true}, {"Apple", true}, {"beta", true}, {"1.wav", false}, {"2.wav", false}, {"_x", false},
                        {"Alpha.wav", false}, {"apple2.wav", false}, {"bass.wav", false}, {"zz", false}, {"\xC3\xA9" "a", false}};
  for (const Item& it : items) {
    NEED(ix.add(it.name, it.dir) >= 0);
  }
  // 0 alpha, 1 Apple | 2 beta || 3 1.wav | 4 2.wav | 5 _x | 6 Alpha.wav, 7 apple2.wav | 8 bass.wav | 9 zz | 10 e-acute
  NEED(ix.count() == 11u);
  CHECK(std::strcmp(ix.name(10), "\xC3\xA9" "a") == 0);
  CHECK(ix.next_letter(0) == 2);
  CHECK(ix.next_letter(1) == 2);
  CHECK(ix.next_letter(2) == 3);  // the folders end: the files are a group of their own, even where the letter would match
  CHECK(ix.next_letter(3) == 4);
  CHECK(ix.next_letter(6) == 8);
  CHECK(ix.next_letter(10) == 10);  // the last group: stays
  CHECK(ix.next_letter(-5) == 2);   // clamped into the list
  CHECK(ix.next_letter(99) == 10);
  CHECK(ix.prev_letter(0) == 0);
  CHECK(ix.prev_letter(1) == 0);  // mid-group: the start of its own
  CHECK(ix.prev_letter(2) == 0);  // at a start: the start of the group before
  CHECK(ix.prev_letter(3) == 2);
  CHECK(ix.prev_letter(7) == 6);
  CHECK(ix.prev_letter(6) == 5);
  CHECK(ix.prev_letter(8) == 6);
  CHECK(ix.prev_letter(10) == 9);
  CHECK(ix.prev_letter(99) == 9);
  CHECK(ix.find_letter('A') == 0);
  CHECK(ix.find_letter('a') == 0);
  CHECK(ix.find_letter('b') == 2);  // the folder, before the file bass.wav
  CHECK(ix.find_letter('1') == 3);
  CHECK(ix.find_letter('_') == 5);
  CHECK(ix.find_letter('z') == 9);
  CHECK(ix.find_letter('q') == DirIndex::kNone);
  // Walking right from the top visits every group start once, and walking left brings the same ones back.
  const int starts[] = {0, 2, 3, 4, 5, 6, 8, 9, 10};
  unsigned n = 0;
  int at = 0;
  CHECK(starts[n++] == at);
  for (int next = ix.next_letter(at); next != at; next = ix.next_letter(at)) {
    at = next;
    NEED(n < sizeof(starts) / sizeof(starts[0]));
    CHECK(starts[n++] == at);
  }
  CHECK(n == sizeof(starts) / sizeof(starts[0]));
  for (unsigned k = n - 1; k > 0; --k) {
    at = ix.prev_letter(at);
    CHECK(at == starts[k - 1]);
  }
  CHECK(ix.prev_letter(at) == at);  // the top
}
