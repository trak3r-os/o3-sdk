// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sorted listing of one folder (OS-010a): what a browser shows and moves through. Pure data: it knows nothing of the card (dir_scan.h
// fills it from a hal::storage::Storage), nothing of the screen (file_view.h keeps the cursor), so it is the same for the File screen's
// project list and for a later Sample load screen's .wav list.
//
// Order: folders first, then files; inside each, the name compared byte by byte with the ASCII letters folded to lower case (a byte of
// 0x80 and above, UTF-8, sorts by its value after every ASCII byte), and equal names (FAT has none) in the order they came. The array is
// kept sorted as entries arrive, so a listing that is still being read is already in order.
//
// Fixed capacity, no heap (this is what a "few hundred entries" costs, 9.2 KB in all):
//   kMaxEntries = 256  entries, 4 bytes each (a name's place in the pool, its length, a flag):;
//   kPoolBytes  = 8192 bytes of names, each stored whole with its NUL (a name is up to hal::storage::kNameMax = 255 bytes): 256 names of
//                      31 characters fit. Names are never cut, so the path an entry gives back is the one on the card.
// An entry that does not fit (the 257th, or one whose name would overflow the pool) is not added: add() says so, `full()` stays true and
// the owner shows "list full" instead of pretending the list is complete. What was kept is whatever arrived first, not the alphabetically
// first 256: a listing that long wants a narrower folder, not a cleverer eviction.
//
// Keys. An entry's key is its place in the pool: it does not change when other entries are inserted before it, so a cursor can follow
// "the same entry" while the scan adds around it (FileView).
//
// Letters. A group is a run of entries of the same kind (folder / file) whose first byte is the same letter, folded; digits, symbols and
// non-ASCII bytes are each their own group by that first byte. next_letter() / prev_letter() move between groups, find_letter() goes to the
// first entry that starts with a letter.
//
// Contexts: main loop only. Nothing here blocks or allocates.

#include <o3sdk/hal/storage.h>

#include <cstddef>
#include <cstdint>

namespace o3sdk::app {

// What a listed entry is, beyond file or folder (OS-007o): the File screen's project folders say which format they hold. None for every entry of a listing that does not
// tag (the .wav list).
enum class EntryTag : std::uint8_t { None = 0, Og = 1, Native = 2, Recover = 3 };  // Recover: a native project whose file is missing and whose backup is there (an interrupted save)

class DirIndex {
 public:
  static constexpr unsigned kMaxEntries = 256;
  static constexpr unsigned kPoolBytes = 8192;
  static constexpr int kNone = -1;

  // The listing empties (and its generation moves on).
  void clear() noexcept;

  // Inserts `name` (NUL-terminated, 1..kNameMax bytes) in its place; returns the position it took, or kNone when it was not added: the
  // list or the pool is full (full() is true from then on, nothing changes), or the name is empty or longer than kNameMax.
  // `stereo` (OS-LOADCH): the .wav list's file has two channels (DirScan probes the header): stereo() says it.
  int add(const char* name, bool is_dir, EntryTag tag = EntryTag::None, bool stereo = false) noexcept;

  unsigned count() const noexcept { return count_; }
  bool full() const noexcept { return full_; }
  // Bumped by every add() that inserted and by clear(): the owner redraws and re-fits the cursor when it moves.
  std::uint32_t generation() const noexcept { return generation_; }

  // An entry's name (NUL-terminated, never null; "" for a position outside the list), its length, its kind.
  const char* name(unsigned i) const noexcept { return i < count_ ? pool_ + entries_[i].off : ""; }
  unsigned name_length(unsigned i) const noexcept { return i < count_ ? entries_[i].len : 0u; }
  bool is_dir(unsigned i) const noexcept { return i < count_ && (entries_[i].flags & kDirFlag) != 0u; }
  bool stereo(unsigned i) const noexcept { return i < count_ && (entries_[i].flags & kStereoFlag) != 0u; }
  EntryTag tag(unsigned i) const noexcept { return i < count_ ? static_cast<EntryTag>((entries_[i].flags >> kTagShift) & kTagMask) : EntryTag::None; }

  // The stable identity of the entry at `i` (0 for a position outside the list), and where a key is now (kNone if it is not in the list).
  std::uint16_t key(unsigned i) const noexcept { return i < count_ ? entries_[i].off : std::uint16_t{0}; }
  int find_key(std::uint16_t key) const noexcept;
  // The entry with this name (ASCII case folded, as FAT does), kNone when there is none: how a name remembered elsewhere (the RECENT projects) is found in a listing.
  int find_name(const char* name) const noexcept;

  // The first entry of the next group after `from`; `from` itself when there is none (the last group). A position outside the list is
  // clamped into it; kNone for an empty list.
  int next_letter(int from) const noexcept;
  // The start of the group `from` is in when it is not at the start; otherwise the start of the group before it (0 in the first group).
  int prev_letter(int from) const noexcept;
  // The first entry whose name starts with `letter` (case-folded; a folder before a file of the same letter); kNone when none does.
  int find_letter(char letter) const noexcept;

  // The order, exposed so a test can state it independently: negative if (a_dir, a) sorts before (b_dir, b), 0 if they are equal.
  static int compare(bool a_dir, const char* a, bool b_dir, const char* b) noexcept;

 private:
  static constexpr std::uint8_t kDirFlag = 1;
  static constexpr unsigned kTagShift = 1;  // flags bits 1..2: the EntryTag
  static constexpr unsigned kTagMask = 3;
  static constexpr std::uint8_t kStereoFlag = 8;  // flags bit 3 (OS-LOADCH)
  struct Entry {
    std::uint16_t off;   // the name's place in pool_ (its key)
    std::uint8_t len;    // bytes, without the NUL (1..255)
    std::uint8_t flags;  // kDirFlag, the tag from kTagShift
  };
  static_assert(sizeof(Entry) == 4, "an entry is 4 bytes: 256 of them are 1 KB");

  unsigned group_of(unsigned i) const noexcept;  // (kind << 8) | folded first byte

  Entry entries_[kMaxEntries];
  char pool_[kPoolBytes];
  std::uint16_t pool_used_ = 0;
  std::uint16_t count_ = 0;
  bool full_ = false;
  std::uint32_t generation_ = 0;
};

// ASCII case folding of one byte: 'A'..'Z' to lower case, everything else (digits, symbols, UTF-8 bytes) unchanged.
constexpr unsigned char fold_ascii(unsigned char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c; }

}  // namespace o3sdk::app
