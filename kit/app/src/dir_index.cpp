// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3sdk/app/dir_index.h>

#include <cstring>

namespace o3sdk::app {

void DirIndex::clear() noexcept {
  pool_used_ = 0;
  count_ = 0;
  full_ = false;
  ++generation_;
}

int DirIndex::compare(bool a_dir, const char* a, bool b_dir, const char* b) noexcept {
  if (a_dir != b_dir) {
    return a_dir ? -1 : 1;  // folders first
  }
  for (std::size_t i = 0;; ++i) {
    const unsigned char ca = fold_ascii(static_cast<unsigned char>(a[i]));
    const unsigned char cb = fold_ascii(static_cast<unsigned char>(b[i]));
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
    if (ca == 0) {
      break;  // equal when the case is ignored
    }
  }
  // Only the case differs (FAT has no such pair in one folder, a hostile source might): the raw bytes decide, so the order does not depend on
  // which of the two arrived first.
  for (std::size_t i = 0;; ++i) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }
    if (ca == 0) {
      return 0;
    }
  }
}

int DirIndex::add(const char* name, bool is_dir, EntryTag tag, bool stereo) noexcept {
  if (name == nullptr) {
    return kNone;
  }
  std::size_t len = 0;
  while (len <= hal::storage::kNameMax && name[len] != '\0') {
    ++len;
  }
  if (len == 0 || len > hal::storage::kNameMax) {
    return kNone;
  }
  if (count_ >= kMaxEntries || static_cast<std::size_t>(pool_used_) + len + 1u > kPoolBytes) {
    full_ = true;
    return kNone;
  }
  // The place: after every entry that sorts before it or equals it (binary search, the entries are sorted).
  unsigned lo = 0;
  unsigned hi = count_;
  while (lo < hi) {
    const unsigned mid = (lo + hi) / 2u;
    const Entry& e = entries_[mid];
    if (compare(is_dir, name, (e.flags & kDirFlag) != 0u, pool_ + e.off) < 0) {
      hi = mid;
    } else {
      lo = mid + 1u;
    }
  }
  std::memmove(&entries_[lo + 1u], &entries_[lo], (count_ - lo) * sizeof(Entry));
  std::memcpy(pool_ + pool_used_, name, len);
  pool_[pool_used_ + len] = '\0';
  entries_[lo] = Entry{pool_used_, static_cast<std::uint8_t>(len), static_cast<std::uint8_t>((is_dir ? kDirFlag : 0u) | (static_cast<unsigned>(tag) << kTagShift) | (stereo ? kStereoFlag : 0u))};
  pool_used_ = static_cast<std::uint16_t>(pool_used_ + len + 1u);
  ++count_;
  ++generation_;
  return static_cast<int>(lo);
}

int DirIndex::find_key(std::uint16_t key) const noexcept {
  for (unsigned i = 0; i < count_; ++i) {
    if (entries_[i].off == key) {
      return static_cast<int>(i);
    }
  }
  return kNone;
}

int DirIndex::find_name(const char* name) const noexcept {
  if (name == nullptr) {
    return kNone;
  }
  for (unsigned i = 0; i < count_; ++i) {
    const char* have = pool_ + entries_[i].off;
    std::size_t k = 0;
    while (have[k] != '\0' && fold_ascii(static_cast<unsigned char>(have[k])) == fold_ascii(static_cast<unsigned char>(name[k]))) {
      ++k;
    }
    if (have[k] == '\0' && name[k] == '\0') {
      return static_cast<int>(i);
    }
  }
  return kNone;
}

unsigned DirIndex::group_of(unsigned i) const noexcept {
  return ((entries_[i].flags & kDirFlag) != 0u ? 0x100u : 0u) | fold_ascii(static_cast<unsigned char>(pool_[entries_[i].off]));
}

int DirIndex::next_letter(int from) const noexcept {
  if (count_ == 0u) {
    return kNone;
  }
  const unsigned last = count_ - 1u;
  const unsigned f = from < 0 ? 0u : (static_cast<unsigned>(from) > last ? last : static_cast<unsigned>(from));
  const unsigned g = group_of(f);
  for (unsigned i = f + 1u; i < count_; ++i) {
    if (group_of(i) != g) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(f);
}

int DirIndex::prev_letter(int from) const noexcept {
  if (count_ == 0u) {
    return kNone;
  }
  const unsigned last = count_ - 1u;
  const unsigned f = from < 0 ? 0u : (static_cast<unsigned>(from) > last ? last : static_cast<unsigned>(from));
  unsigned start = f;
  const unsigned g = group_of(f);
  while (start > 0u && group_of(start - 1u) == g) {
    --start;
  }
  if (start < f || start == 0u) {
    return static_cast<int>(start);  // mid-group: its own start; the first group: the top
  }
  const unsigned before = group_of(start - 1u);
  unsigned s = start - 1u;
  while (s > 0u && group_of(s - 1u) == before) {
    --s;
  }
  return static_cast<int>(s);
}

int DirIndex::find_letter(char letter) const noexcept {
  const unsigned char want = fold_ascii(static_cast<unsigned char>(letter));
  for (unsigned i = 0; i < count_; ++i) {
    if (fold_ascii(static_cast<unsigned char>(pool_[entries_[i].off])) == want) {
      return static_cast<int>(i);
    }
  }
  return kNone;
}

}  // namespace o3sdk::app
