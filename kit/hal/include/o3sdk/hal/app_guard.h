// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The write guard of o3sdk's apps (docs/M2C-PLAN.md, section 3 and D2): hal/save_guard.h's table (track3r-os's save layer, kept identical) plus the
// rows of an app's own folder. The FatFs writer (platform/mk66/save) and the save job (app/atomic_save.h) answer through THIS guard, with the names the
// ported code already calls (classify, may_make_dir, may_create, may_create_marker, may_rename, may_remove, join, Place, kPathCap, kNameMax, detail), so
// the ported code and its tests run unchanged; the kit's files service forms only `Apps/<app>/...` and `Samples/` paths (tests/app_guard_test.cpp walks that).
//
// Paths are relative to the card's root with no leading '/', components separated by '/', FAT case rules (every fixed name matches in any case). The rows of
// save_guard.h are all here (save::classify answers for every path outside `Apps`); the new ones:
//
//   Apps                                      folder        make_dir
//   Apps/<app>                                folder        make_dir        <app>: valid_name() with no '.', at most kAppNameMax (15) bytes: the app's id from its build
//   Apps/<app>/<stem>.<ext>                   Target        an app's file: <stem> valid_name(), <ext> (after the LAST dot) on the allowlist kAppExts
//   Apps/<app>/<stem>.<ext>.bak               Backup        the previous version of it, kept by a replacing save
//   Apps/<app>/<project>                      folder        make_dir        a project folder: valid_name() with no '.' in it (see below)
//   Apps/<app>/<project>/<stem>.<ext>         Target        as above, one level down (nothing deeper)
//   Apps/<app>/<project>/<stem>.<ext>.bak     Backup
//   Apps/<app>/<project>/.deleted             Marker        DELETE (decision D4): an empty file, created new, never written to, never replaced, never removed; a listing hides the folder
//   Apps/<app>/system.o3s                     Target        the kit's settings for this app: an ordinary `<stem>.o3s`
//   Apps/<app>/_o3save.tmp, Apps/<app>/<project>/_o3save.tmp   Temp    the file a save is written to before it replaces anything
//
// kAppExts: o3a (an app's own data), o3s (settings), wav, mid, syx, txt, json, csv. Never ptf, hex, o3pj, tmod or anything else: no firmware or project of
// track3r-os can come from an app. A folder's name (<app>, <project>) has no '.', so no folder can look like a file ("notes.txt", "firmware.ptf", "a.o3a.bak"): a listing
// never has to guess what an entry is, and nothing that is not an allowlisted file can carry an extension in an app's folder.
//
// What each operation may touch is as in save_guard.h: make_dir a Folder; create a Temp only; rename Temp -> Target (it must not exist), Target -> its own
// Backup, Backup -> its own Target, in one folder; marker creates a Marker; remove a Temp or a Backup only (never a Target, a Marker, a folder or a file the
// user made). A project folder is never renamed. The longest path, "Apps/" + 15 + "/" + 31 + "/" + 31 + ".json.bak", is 93 bytes, 94 with the NUL.
#include <o3sdk/hal/save_guard.h>

#include <cstddef>

namespace o3sdk::hal::storage::app_guard {

// What the ported code names through its `guard` alias: the very declarations of save_guard.h (one value of kPathCap and of kNameMax, one Place).
using save::join;
using save::kBackupSuffix;
using save::kDeletedMarker;
using save::kNameMax;
using save::kPathCap;
using save::kSamplesDir;
using save::kTempName;
using save::kWavSuffix;
using save::Place;
using save::valid_name;

inline constexpr char kAppsDir[] = "Apps";
inline constexpr char kSystemFile[] = "system.o3s";  // the kit's settings for the app (decision D5)
inline constexpr std::size_t kAppNameMax = 15;       // bytes of <app>
// The extensions an app may write, lower case with no dot. Not ptf, hex, o3pj or tmod (nor anything that is not here).
inline constexpr const char* kAppExts[] = {"o3a", "o3s", "wav", "mid", "syx", "txt", "json", "csv"};
inline constexpr std::size_t kAppExtCount = sizeof kAppExts / sizeof kAppExts[0];

static_assert(kPathCap == 96 && kNameMax == 31, "the ported code's capacities are the guard's: one value");
static_assert(sizeof kAppsDir - 1 + 1 + kAppNameMax + 1 + kNameMax + 1 + kNameMax + sizeof ".json.bak" <= kPathCap,
              "the longest path of the Apps rows (\"Apps/\" + 15 + \"/\" + 31 + \"/\" + 31 + \".json.bak\", with the NUL) fits kPathCap");

namespace detail {

using save::detail::equal;
using save::detail::length;
using save::detail::lower;
using save::detail::Part;
using save::detail::same;
using save::detail::split;
using save::detail::stem_of;
using save::detail::valid_name;

inline constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// A valid_name with no '.' at all: a folder's name (<app>, <project>) can then never be taken for a file or a backup (those have a dot).
constexpr bool dotless_name(const Part& name) noexcept {
  for (std::size_t i = 0; i < name.n; ++i) {
    if (name.begin[i] == '.') {
      return false;
    }
  }
  return valid_name(name.begin, name.n);
}

// <app>: a dotless valid_name of at most kAppNameMax bytes.
constexpr bool app_name(const Part& name) noexcept { return name.n <= kAppNameMax && dotless_name(name); }

// "<stem>.<ext>": <stem> a valid_name (at least one byte), <ext> (what follows the LAST dot, any case) on kAppExts: "a.o3a.ptf" is a ".ptf" and refused.
constexpr bool file_name(const Part& name) noexcept {
  std::size_t dot = name.n;
  while (dot != 0 && name.begin[dot - 1] != '.') {
    --dot;
  }
  if (dot < 2) {  // no dot, or nothing before it
    return false;
  }
  const std::size_t stem = dot - 1;
  const std::size_t ext = name.n - dot;
  bool listed = false;
  for (std::size_t i = 0; i < kAppExtCount; ++i) {
    listed = listed || same(name.begin + dot, ext, kAppExts[i]);
  }
  return listed && valid_name(name.begin, stem);
}

// "<stem>.<ext>.bak": the Backup form of a file_name.
constexpr bool backup_name(const Part& name) noexcept {
  const std::size_t stem = stem_of(name, kBackupSuffix);
  return stem != kNone && file_name(Part{name.begin, stem});
}

// <project>: a dotless valid_name.
constexpr bool project_name(const Part& name) noexcept { return dotless_name(name); }

// What the last component of an app's folder is: a Temp, a file, a Backup of one, or (`folders`) a project folder.
constexpr Place entry(const Part& name, bool folders) noexcept {
  if (same(name.begin, name.n, kTempName)) {
    return Place::Temp;
  }
  if (file_name(name)) {
    return Place::Target;
  }
  if (backup_name(name)) {
    return Place::Backup;
  }
  return (folders && project_name(name)) ? Place::Folder : Place::None;
}

// The `Apps` rows: c[0] is "Apps" (in any case), n components.
constexpr Place apps_place(const Part (&c)[4], unsigned n) noexcept {
  if (n == 1) {
    return Place::Folder;
  }
  if (!app_name(c[1])) {
    return Place::None;
  }
  if (n == 2) {
    return Place::Folder;
  }
  if (n == 3) {  // Apps/<app>/<file>, its Backup or Temp, or a project folder
    return entry(c[2], true);
  }
  // n == 4: below a project folder, files only
  if (!project_name(c[2])) {
    return Place::None;
  }
  if (same(c[3].begin, c[3].n, kDeletedMarker)) {
    return Place::Marker;
  }
  return entry(c[3], false);
}

}  // namespace detail

// An extension an app may ask for: exactly one of kAppExts, lower case, no dot ("TXT", ".txt" and "" are not).
constexpr bool app_ext_ok(const char* ext) noexcept {
  if (ext == nullptr) {
    return false;
  }
  const std::size_t n = detail::length(ext);
  for (std::size_t i = 0; i < kAppExtCount; ++i) {
    const char* const e = kAppExts[i];
    if (n != detail::length(e)) {
      continue;
    }
    std::size_t k = 0;
    while (k < n && ext[k] == e[k]) {
      ++k;
    }
    if (k == n) {
      return true;
    }
  }
  return false;
}

// The app's id as a folder name: a valid_name with no '.', 1..kAppNameMax bytes.
constexpr bool app_name_ok(const char* name) noexcept { return name != nullptr && detail::app_name(detail::Part{name, detail::length(name)}); }
// A project folder's name: valid_name with no '.'.
constexpr bool project_name_ok(const char* name) noexcept { return name != nullptr && detail::project_name(detail::Part{name, detail::length(name)}); }
// An app's file name, "<stem>.<ext>" with the extension on the allowlist.
constexpr bool file_name_ok(const char* name) noexcept { return name != nullptr && detail::file_name(detail::Part{name, detail::length(name)}); }

// What `path` is to the guard (the tables of both headers); None for everything else.
constexpr Place classify(const char* path) noexcept {
  detail::Part c[4] = {};
  const unsigned n = detail::split(path, c);
  if (n != 0 && detail::same(c[0].begin, c[0].n, kAppsDir)) {
    return detail::apps_place(c, n);
  }
  return save::classify(path);
}

constexpr bool may_make_dir(const char* path) noexcept { return classify(path) == Place::Folder; }
constexpr bool may_create(const char* path) noexcept { return classify(path) == Place::Temp; }
constexpr bool may_create_marker(const char* path) noexcept { return classify(path) == Place::Marker; }
constexpr bool may_remove(const char* path) noexcept {
  const Place p = classify(path);
  return p == Place::Temp || p == Place::Backup;
}

// rename(from, to): Temp -> Target in one folder; Target -> its own Backup; Backup -> its own Target (as save::may_rename, over this classify).
constexpr bool may_rename(const char* from, const char* to) noexcept {
  detail::Part a[4] = {};
  detail::Part b[4] = {};
  const unsigned na = detail::split(from, a);
  const unsigned nb = detail::split(to, b);
  if (na < 2 || na != nb) {
    return false;
  }
  for (unsigned i = 0; i + 1 < na; ++i) {  // the same folder (every component but the last)
    if (!detail::equal(a[i], b[i])) {
      return false;
    }
  }
  const Place pf = classify(from);
  const Place pt = classify(to);
  const detail::Part& fa = a[na - 1];
  const detail::Part& fb = b[nb - 1];
  const auto is_backup_of = [](const detail::Part& backup, const detail::Part& target) {
    return backup.n == target.n + 4 && detail::equal(detail::Part{backup.begin, target.n}, target) && detail::same(backup.begin + target.n, 4, kBackupSuffix);
  };
  if (pf == Place::Temp && pt == Place::Target) {
    return true;
  }
  if (pf == Place::Target && pt == Place::Backup) {
    return is_backup_of(fb, fa);
  }
  if (pf == Place::Backup && pt == Place::Target) {
    return is_backup_of(fa, fb);
  }
  return false;
}

// Spot checks at compile time (tests/app_guard_test.cpp walks the table).
static_assert(classify("Apps") == Place::Folder && classify("Apps/hello") == Place::Folder && classify("Apps/hello/song one") == Place::Folder);
static_assert(classify("Apps/hello/system.o3s") == Place::Target && classify("Apps/hello/system.o3s.bak") == Place::Backup);
static_assert(classify("Apps/hello/song one/p1.o3a") == Place::Target && classify("Apps/hello/song one/.deleted") == Place::Marker);
static_assert(classify("Apps/hello/_o3save.tmp") == Place::Temp && classify("Apps/hello/song one/_o3save.tmp") == Place::Temp);
static_assert(classify("Apps/hello/firmware.ptf") == Place::None && classify("Apps/hello/a/b/c.o3a") == Place::None);
static_assert(classify("Apps/sixteen_chars_xx") == Place::None && classify("Projects/Song/project.o3pj") == Place::Target);

}  // namespace o3sdk::hal::storage::app_guard
