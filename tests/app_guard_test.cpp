// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal/app_guard.h (docs/M2C-PLAN.md, section 3 and D2): the guard the ported save code answers through. It is save_guard.h's table (track3r-os, identical) plus the `Apps/` rows
// of an app's own folder. This walks the new rows (every extension of the allowlist at both depths, the lengths, the hostile names, the rules of each operation), proves the old
// rows are the old guard's answers and the new ones are not the old guard's (no path in both tables), and walks every path of a small grammar to show what the guard lets
// through below `Apps` and nowhere else: the paths a files service built on `join()` and the name checks can form are `Apps/<app>/...` and the ones of save_guard.h
// (`Samples/` among them), and no extension but the allowlist's comes out of an app's folder.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3sdk/hal/app_guard.h>
#include <o3sdk/hal/save_guard.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace ag = o3sdk::hal::storage::app_guard;
namespace sv = o3sdk::hal::storage::save;
using ag::Place;

namespace {

std::string x(std::size_t n) { return std::string(n, 'x'); }

struct Row {
  const char* path;
  Place place;
};

}  // namespace

TEST_CASE("app guard: the capacities are the old guard's, the Apps rows fit") {
  static_assert(ag::kPathCap == sv::kPathCap && ag::kNameMax == sv::kNameMax);
  CHECK(ag::kPathCap == 96u);
  CHECK(ag::kNameMax == 31u);
  CHECK(ag::kAppNameMax == 15u);
  // The longest path of the new rows: "Apps/" + 15 + "/" + 31 + "/" + 31 + ".json.bak" is 93 bytes, 94 with the NUL.
  const std::string widest = "Apps/" + x(15) + "/" + x(31) + "/" + x(31) + ".json.bak";
  CHECK(widest.size() + 1u == 94u);
  CHECK(widest.size() < ag::kPathCap);
  CHECK(ag::classify(widest.c_str()) == Place::Backup);
  char out[ag::kPathCap];
  CHECK(ag::join(out, sizeof out, widest.c_str()));
  CHECK(std::string(out) == widest);
  CHECK(ag::classify(("Apps/" + x(15) + "/" + x(31) + "/" + x(32) + ".json.bak").c_str()) == Place::None);
  CHECK(ag::classify(("Apps/" + x(16)).c_str()) == Place::None);
  CHECK(ag::classify(("Apps/" + x(15)).c_str()) == Place::Folder);
}

TEST_CASE("app guard: the Apps rows") {
  const Row rows[] = {
      {"Apps", Place::Folder},
      {"apps", Place::Folder},
      {"APPS", Place::Folder},
      {"Apps/hello", Place::Folder},
      {"Apps/o3-303", Place::Folder},
      {"Apps/hello/song one", Place::Folder},
      {"Apps/hello/system.o3s", Place::Target},
      {"Apps/hello/system.o3s.bak", Place::Backup},
      {"Apps/hello/pattern 01.o3a", Place::Target},
      {"Apps/hello/pattern 01.o3a.bak", Place::Backup},
      {"Apps/hello/song one/pattern 01.o3a", Place::Target},
      {"Apps/hello/song one/pattern 01.o3a.bak", Place::Backup},
      {"Apps/hello/song one/.deleted", Place::Marker},
      {"Apps/hello/_o3save.tmp", Place::Temp},
      {"Apps/hello/song one/_o3save.tmp", Place::Temp},
      {"APPS/HELLO/SONG ONE/PATTERN 01.O3A", Place::Target},
      {"Apps/hello/a.b.txt", Place::Target},  // the extension is what follows the LAST dot
      // not the guard's
      {"Apps/", Place::None},
      {"/Apps", Place::None},
      {"Apps/hello/", Place::None},
      {"Apps//hello", Place::None},
      {"Apps/hello//x.o3a", Place::None},
      {"Apps/hello/song one/deeper/x.o3a", Place::None},
      {"Apps/hello/song one/deeper", Place::None},
      {"Apps/hello/song one/sub/_o3save.tmp", Place::None},
      {"Apps/hello/.deleted", Place::None},  // a marker belongs to a project folder
      {"Apps/hello/song one/.deleted.bak", Place::None},
      {"Apps/hello/song one/.deleted/x.o3a", Place::None},
      {"Apps/.deleted", Place::None},
      {"Apps/_o3save.tmp", Place::None},
      {"Apps/x.o3a", Place::None},  // no app folder in between: Apps holds folders only, and an app's name has no dot
      {"Apps/hello/.o3a", Place::None},
      {"Apps/hello/o3a", Place::Folder},  // (a project folder named like an extension, without its dot)
      {"Apps/hello/noext", Place::Folder},
      {"Apps/hello/x.bak", Place::None},  // ".bak" alone is not an allowlisted file
      {"Apps/hello/x.o3a.bak.bak", Place::None},
      {"Apps/hello/x.o3a.bak2", Place::None},
      {"Apps/hello/x.o3a ", Place::None},
      {"Apps/hello/x.o3a.", Place::None},
      {"Apps/hello/.hidden.txt", Place::None},
      {"Apps/hello/trail .txt", Place::None},
      {"Apps/hello/a..b.txt", Place::None},
      {"Apps/hello/con.txt", Place::None},
      {"Apps/hello/NUL.o3a", Place::None},
      {"Apps/hello/lpt1.csv", Place::None},
      {"Apps/hello/song one/com9.json", Place::None},
      {"Apps/con", Place::None},
      {"Apps/hello/aux", Place::None},
      {"Apps/hello/song.one", Place::None},  // a project folder has no dot
      {"Apps/hello/song one/x.o3a/y.o3a", Place::None},
      {"Apps/he llo/x.o3a", Place::Target},  // (a space is a valid_name character)
      {"Apps/hello/a:b.txt", Place::None},
      {"Apps/hello/a\\b.txt", Place::None},
      {"Apps/hello/a*b.txt", Place::None},
      {"Apps/../x.o3a", Place::None},
      {"Apps/hello/../x.o3a", Place::None},
      {"Apps/hello/./x.o3a", Place::None},
  };
  for (const Row& r : rows) {
    INFO(std::string(r.path));
    CHECK(ag::classify(r.path) == r.place);
  }
  CHECK(ag::classify(nullptr) == Place::None);
  CHECK(ag::classify("") == Place::None);
}

TEST_CASE("app guard: the extension allowlist, at both depths") {
  const char* const listed[] = {"o3a", "o3s", "wav", "mid", "syx", "txt", "json", "csv"};
  CHECK(ag::kAppExtCount == 8u);
  for (const char* e : listed) {
    INFO(std::string(e));
    CHECK(ag::app_ext_ok(e));
    for (const char* dir : {"Apps/hello/", "Apps/hello/song one/"}) {
      const std::string file = std::string(dir) + "x." + e;
      CHECK(ag::classify(file.c_str()) == Place::Target);
      CHECK(ag::classify((file + ".bak").c_str()) == Place::Backup);
      CHECK(ag::may_rename(file.c_str(), (file + ".bak").c_str()));
      CHECK(ag::may_rename((file + ".bak").c_str(), file.c_str()));
      CHECK(ag::may_remove((file + ".bak").c_str()));
      CHECK_FALSE(ag::may_remove(file.c_str()));
      CHECK_FALSE(ag::may_create(file.c_str()));
      CHECK_FALSE(ag::may_make_dir(file.c_str()));
      std::string upper = file;
      for (char& c : upper) {
        c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
      }
      CHECK(ag::classify(upper.c_str()) == Place::Target);  // a name that is already on the card matches in any case
    }
  }
  // Never firmware, a project, a module or anything that is not on the list.
  const char* const refused[] = {"ptf", "hex", "o3pj", "tmod", "bin", "elf", "exe", "mt", "mtp", "pti", "bmp", "o3st", "o3rc", "bak", "tmp", "o3", "o3aa", "wavx", "js", "c", ""};
  for (const char* e : refused) {
    INFO(std::string(e));
    CHECK_FALSE(ag::app_ext_ok(e));
    const std::string file = std::string("Apps/hello/x.") + e;
    CHECK(ag::classify(file.c_str()) != Place::Target);
    CHECK(ag::classify(file.c_str()) != Place::Backup);
    CHECK(ag::classify((file + ".bak").c_str()) == Place::None);
    const std::string deep = std::string("Apps/hello/song one/x.") + e;
    CHECK(ag::classify(deep.c_str()) == Place::None);
  }
  CHECK_FALSE(ag::app_ext_ok(nullptr));
  CHECK_FALSE(ag::app_ext_ok("TXT"));
  CHECK_FALSE(ag::app_ext_ok(".txt"));
  CHECK_FALSE(ag::app_ext_ok("txt "));
  CHECK(ag::classify("Apps/hello/x.o3a.ptf") == Place::None);  // the extension is the last one
  CHECK(ag::classify("Apps/hello/x.ptf.o3a") == Place::Target);  // (a stem that contains a dot is still a stem)
  CHECK(ag::classify("Apps/hello/firmware.ptf") == Place::None);
  CHECK(ag::classify("Apps/hello/song one/app.hex") == Place::None);
  CHECK(ag::classify("Apps/hello/song one/project.o3pj") == Place::None);
  CHECK(ag::classify("Apps/hello/song one/project.o3pj.bak") == Place::None);
}

TEST_CASE("app guard: names and lengths") {
  CHECK(ag::app_name_ok("hello"));
  CHECK(ag::app_name_ok(x(15).c_str()));
  CHECK_FALSE(ag::app_name_ok(x(16).c_str()));
  CHECK_FALSE(ag::app_name_ok(""));
  CHECK_FALSE(ag::app_name_ok(nullptr));
  CHECK_FALSE(ag::app_name_ok("a/b"));
  CHECK_FALSE(ag::app_name_ok("nul"));
  CHECK_FALSE(ag::app_name_ok(".x"));
  CHECK_FALSE(ag::app_name_ok("o3.303"));
  CHECK(ag::project_name_ok("song one"));
  CHECK(ag::project_name_ok(x(31).c_str()));
  CHECK_FALSE(ag::project_name_ok(x(32).c_str()));
  CHECK_FALSE(ag::project_name_ok("song.one"));
  CHECK_FALSE(ag::project_name_ok("x.o3a"));
  CHECK_FALSE(ag::project_name_ok("x.o3a.bak"));
  CHECK_FALSE(ag::project_name_ok(".deleted"));
  CHECK_FALSE(ag::project_name_ok("_o3save.tmp"));
  CHECK_FALSE(ag::project_name_ok(nullptr));
  CHECK(ag::file_name_ok("system.o3s"));
  CHECK(ag::file_name_ok((x(31) + ".json").c_str()));
  CHECK_FALSE(ag::file_name_ok((x(32) + ".json").c_str()));
  CHECK_FALSE(ag::file_name_ok("system"));
  CHECK_FALSE(ag::file_name_ok("system.o3s.bak"));  // a Backup is never asked for by an app
  CHECK_FALSE(ag::file_name_ok("a.ptf"));
  CHECK_FALSE(ag::file_name_ok(nullptr));
  CHECK(std::strcmp(ag::kSystemFile, "system.o3s") == 0);
  CHECK(ag::classify("Apps/hello/system.o3s") == Place::Target);
  CHECK(ag::file_name_ok(ag::kSystemFile));
  // Every folder a kit-formed path names passes its own check, and the guard agrees with it.
  CHECK(ag::classify((std::string("Apps/") + x(15) + "/" + x(31)).c_str()) == Place::Folder);
  CHECK(ag::classify((std::string("Apps/") + x(15) + "/" + x(32)).c_str()) == Place::None);
  CHECK(ag::classify((std::string("Apps/") + x(15) + "/" + x(31) + "/" + x(31) + ".csv").c_str()) == Place::Target);
  CHECK(ag::classify((std::string("Apps/") + x(15) + "/" + x(31) + "/" + x(32) + ".csv").c_str()) == Place::None);
}

TEST_CASE("app guard: the old rows are the old guard's, the new rows are not") {
  const char* const old_paths[] = {
      "Projects",
      "Projects/Song",
      "Projects/Song/project.o3pj",
      "Projects/Song/project.o3pj.bak",
      "Projects/Song/.deleted",
      "Projects/Song/_o3save.tmp",
      "Projects/Song/samples",
      "Projects/Song/samples/kick.wav",
      "Projects/Song/samples/kick.wav.bak",
      "Samples",
      "Samples/REC_001.wav",
      "Samples/REC_001.wav.bak",
      "Samples/_o3save.tmp",
      "instruments/a.pti",
      "Settings/device.o3st",
      "Settings/device.o3st.bak",
      "Settings/recent.o3rc",
      "Export/Song/Song_Song0001.wav",
      "Export/Song OG/project.mt",
      "Export/Song OG/patterns/pattern_01.mtp",
      "Screenshots/SHOT_0001.bmp",
      "AppExport/x/y.txt",
      "AppExport/x/y.ptf",
      "Firmware/app.ptf",
      "Firmware",
      "Other/x.txt",
      "x.txt",
      "",
  };
  for (const char* p : old_paths) {
    INFO(std::string(p));
    CHECK(ag::classify(p) == sv::classify(p));
    CHECK(ag::may_make_dir(p) == sv::may_make_dir(p));
    CHECK(ag::may_create(p) == sv::may_create(p));
    CHECK(ag::may_create_marker(p) == sv::may_create_marker(p));
    CHECK(ag::may_remove(p) == sv::may_remove(p));
    for (const char* q : old_paths) {
      CHECK(ag::may_rename(p, q) == sv::may_rename(p, q));
    }
  }
  // No path is in both tables: the old guard knows nothing below Apps, and the new rows answer nothing outside it but the old guard's answer.
  const char* const new_paths[] = {"Apps", "Apps/hello", "Apps/hello/x.o3a", "Apps/hello/x.o3a.bak", "Apps/hello/p", "Apps/hello/p/x.txt", "Apps/hello/p/.deleted", "Apps/hello/_o3save.tmp"};
  for (const char* p : new_paths) {
    INFO(std::string(p));
    CHECK(sv::classify(p) == Place::None);
    CHECK(ag::classify(p) != Place::None);
  }
  // The update path's folder is its own guard's: neither of these knows it.
  CHECK(ag::classify("Firmware") == Place::None);
  CHECK(ag::classify("Firmware/app.ptf") == Place::None);
  CHECK(ag::classify("Firmware/Apps") == Place::None);
  // A file of the app's folder cannot be a Samples or a Projects row and the other way round.
  CHECK(ag::classify("Apps/Samples/x.wav") == Place::Target);  // (an app that is called Samples is an app folder)
  CHECK(ag::classify("Samples/x.wav") == Place::Target);
  CHECK(ag::classify("Samples/Apps") == Place::None);
  CHECK(ag::classify("Projects/Apps/project.o3pj") == Place::Target);  // (a project called Apps, the old guard's)
}

TEST_CASE("app guard: what each operation may touch") {
  const char* const folder_a = "Apps/hello/song one";
  const char* const file_a = "Apps/hello/song one/p1.o3a";
  const char* const bak_a = "Apps/hello/song one/p1.o3a.bak";
  const char* const tmp_a = "Apps/hello/song one/_o3save.tmp";
  const char* const marker_a = "Apps/hello/song one/.deleted";
  const char* const file_b = "Apps/hello/p1.o3a";
  const char* const tmp_b = "Apps/hello/_o3save.tmp";
  // make_dir: a Folder only
  CHECK(ag::may_make_dir("Apps"));
  CHECK(ag::may_make_dir("Apps/hello"));
  CHECK(ag::may_make_dir(folder_a));
  CHECK_FALSE(ag::may_make_dir(file_a));
  CHECK_FALSE(ag::may_make_dir(bak_a));
  CHECK_FALSE(ag::may_make_dir(tmp_a));
  CHECK_FALSE(ag::may_make_dir(marker_a));
  CHECK_FALSE(ag::may_make_dir("Apps/hello/song.one"));
  CHECK_FALSE(ag::may_make_dir("Apps/hello/a/b"));
  // create: a Temp only; the marker through its own call
  CHECK(ag::may_create(tmp_a));
  CHECK(ag::may_create(tmp_b));
  CHECK_FALSE(ag::may_create(file_a));
  CHECK_FALSE(ag::may_create(bak_a));
  CHECK_FALSE(ag::may_create(marker_a));
  CHECK_FALSE(ag::may_create_marker(tmp_a));
  CHECK(ag::may_create_marker(marker_a));
  CHECK_FALSE(ag::may_create_marker("Apps/hello/.deleted"));
  CHECK_FALSE(ag::may_create_marker(folder_a));
  // remove: a Temp or a Backup, never a Target, a Marker or a folder
  CHECK(ag::may_remove(tmp_a));
  CHECK(ag::may_remove(bak_a));
  CHECK_FALSE(ag::may_remove(file_a));
  CHECK_FALSE(ag::may_remove(marker_a));
  CHECK_FALSE(ag::may_remove(folder_a));
  CHECK_FALSE(ag::may_remove("Apps/hello"));
  CHECK_FALSE(ag::may_remove("Apps"));
  // rename: Temp -> Target, Target -> its own Backup, Backup -> its own Target, in one folder
  CHECK(ag::may_rename(tmp_a, file_a));
  CHECK(ag::may_rename(file_a, bak_a));
  CHECK(ag::may_rename(bak_a, file_a));
  CHECK(ag::may_rename(tmp_b, file_b));
  CHECK_FALSE(ag::may_rename(tmp_a, bak_a));  // a Temp never becomes a Backup
  CHECK_FALSE(ag::may_rename(file_a, tmp_a));
  CHECK_FALSE(ag::may_rename(bak_a, tmp_a));
  CHECK_FALSE(ag::may_rename(file_a, "Apps/hello/song one/p2.o3a"));  // a Target to another Target
  CHECK_FALSE(ag::may_rename(file_a, "Apps/hello/song one/p2.o3a.bak"));  // another file's Backup
  CHECK_FALSE(ag::may_rename("Apps/hello/song one/p2.o3a.bak", file_a));
  CHECK_FALSE(ag::may_rename(tmp_a, file_b));  // across folders
  CHECK_FALSE(ag::may_rename(file_a, "Apps/hello/p1.o3a.bak"));
  CHECK_FALSE(ag::may_rename(tmp_a, "Apps/other/song one/p1.o3a"));
  CHECK_FALSE(ag::may_rename(folder_a, "Apps/hello/song two"));  // a project folder is never renamed
  CHECK_FALSE(ag::may_rename("Apps/hello", "Apps/hello2"));
  CHECK_FALSE(ag::may_rename(marker_a, file_a));
  CHECK_FALSE(ag::may_rename(tmp_a, marker_a));
  CHECK_FALSE(ag::may_rename(tmp_a, "Apps/hello/song one/firmware.ptf"));
  CHECK_FALSE(ag::may_rename(tmp_a, "Firmware/p1.o3a"));
  CHECK_FALSE(ag::may_rename(nullptr, file_a));
  CHECK_FALSE(ag::may_rename(tmp_a, nullptr));
  CHECK_FALSE(ag::may_rename(tmp_a, tmp_a));
  CHECK_FALSE(ag::may_rename("Apps", "Apps"));
  // a rename in any case matches the same file
  CHECK(ag::may_rename("APPS/HELLO/SONG ONE/_O3SAVE.TMP", "apps/hello/song one/P1.O3A"));
}

TEST_CASE("app guard: a path of the join helper is a path of the guard") {
  char dir[ag::kPathCap];
  char path[ag::kPathCap];
  // The shape a files service builds: join("Apps", app, folder) then join(dir, name), then the backup and the temp beside it.
  CHECK(ag::join(dir, sizeof dir, ag::kAppsDir, "hello", "song one"));
  CHECK(std::string(dir) == "Apps/hello/song one");
  CHECK(ag::classify(dir) == Place::Folder);
  CHECK(ag::join(path, sizeof path, dir, "p1.o3a"));
  CHECK(ag::classify(path) == Place::Target);
  CHECK(ag::join(path, sizeof path, dir, ag::kTempName));
  CHECK(ag::classify(path) == Place::Temp);
  CHECK(ag::join(dir, sizeof dir, ag::kAppsDir, "hello"));
  CHECK(ag::join(path, sizeof path, dir, ag::kSystemFile));
  CHECK(ag::classify(path) == Place::Target);
  char tiny[8];
  CHECK_FALSE(ag::join(tiny, sizeof tiny, ag::kAppsDir, "hello"));
  CHECK(tiny[0] == '\0');
  CHECK(ag::join(dir, sizeof dir, ag::kSamplesDir));
  CHECK(ag::classify(dir) == Place::Folder);
  CHECK(ag::join(path, sizeof path, dir, "REC_001.wav"));
  CHECK(ag::classify(path) == Place::Target);
}

namespace {

// The grammar the walk below draws its components from: every shape the guard distinguishes, in both cases, plus the hostile ones.
const char* const kTokens[] = {
    "Apps",      "apps",      "APPS",         "hello",     "o3-303",    "x",          "song one", "song.one",   "Samples",       "Projects", "Settings",
    "Firmware",  "AppExport", "Screenshots",  "x.o3a",     "X.O3A",     "x.o3a.bak",  "x.txt",    "x.ptf",      "x.hex",         "x.o3pj",   "x.json.bak",
    "x.wav",     "x.bak",     ".deleted",     "_o3save.tmp", "system.o3s", "con",      "con.txt",  "..",         ".",             "",         "a b",
    "xxxxxxxxxxxxxxxx", "project.o3pj", "device.o3st", "samples", "a.pti", "x.mid",
};
constexpr std::size_t kTokenCount = sizeof kTokens / sizeof kTokens[0];

char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// `a` equals the NUL-terminated `b` in any case.
bool lower_eq(const std::string& a, const char* b) {
  if (a.size() != std::strlen(b)) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lc(a[i]) != lc(b[i])) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> parts_of(const std::string& path) {
  std::vector<std::string> out;
  std::size_t from = 0;
  for (;;) {
    const std::size_t slash = path.find('/', from);
    out.push_back(path.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
    if (slash == std::string::npos) {
      return out;
    }
    from = slash + 1;
  }
}

std::string ext_of(const std::string& name) {
  const std::size_t dot = name.rfind('.');
  return dot == std::string::npos ? std::string() : name.substr(dot + 1);
}

bool listed_ext(const std::string& ext) {
  for (std::size_t i = 0; i < ag::kAppExtCount; ++i) {
    if (lower_eq(ext, ag::kAppExts[i])) {
      return true;
    }
  }
  return false;
}

// What the walk asserts of one path the guard classified under Apps.
void check_apps_path(const std::string& path, Place place) {
  const std::vector<std::string> c = parts_of(path);
  INFO(path);
  NEED(!c.empty() && lower_eq(c[0], "Apps"));
  CHECK(c.size() <= 4u);
  CHECK(path.size() + 1u <= ag::kPathCap);
  for (const std::string& part : c) {
    CHECK(!part.empty());
    CHECK(part != ".");
    CHECK(part != "..");
  }
  if (c.size() >= 2u) {
    CHECK(c[1].size() <= ag::kAppNameMax);
    CHECK(ag::app_name_ok(c[1].c_str()));
  }
  switch (place) {
    case Place::Folder:
      CHECK(c.size() <= 3u);
      if (c.size() == 3u) {
        CHECK(ag::project_name_ok(c[2].c_str()));
      }
      break;
    case Place::Temp:
      CHECK(c.size() >= 3u);
      CHECK(lower_eq(c.back(), ag::kTempName));
      break;
    case Place::Marker:
      CHECK(c.size() == 4u);
      CHECK(lower_eq(c.back(), ag::kDeletedMarker));
      break;
    case Place::Target:
    case Place::Backup: {
      CHECK(c.size() >= 3u);
      std::string name = c.back();
      if (place == Place::Backup) {
        NEED(name.size() > 4u);
        CHECK(lower_eq(name.substr(name.size() - 4u), ag::kBackupSuffix));
        name.resize(name.size() - 4u);
      }
      CHECK(listed_ext(ext_of(name)));
      CHECK(ag::file_name_ok(name.c_str()));
      CHECK(!lower_eq(ext_of(name), "ptf"));
      CHECK(!lower_eq(ext_of(name), "hex"));
      CHECK(!lower_eq(ext_of(name), "o3pj"));
      if (c.size() == 4u) {
        CHECK(ag::project_name_ok(c[2].c_str()));
      }
      break;
    }
    case Place::None:
      CHECK(false);
      break;
  }
}

void check_operations(const std::string& path) {
  const Place place = ag::classify(path.c_str());
  CHECK(ag::may_make_dir(path.c_str()) == (place == Place::Folder));
  CHECK(ag::may_create(path.c_str()) == (place == Place::Temp));
  CHECK(ag::may_create_marker(path.c_str()) == (place == Place::Marker));
  CHECK(ag::may_remove(path.c_str()) == (place == Place::Temp || place == Place::Backup));
}

}  // namespace

TEST_CASE("app guard: every path of a small grammar, up to four components") {
  // About forty tokens, one to four components: nearly three million paths. What the guard lets through under Apps is what the table says and nothing more.
  std::size_t apps_paths = 0;
  std::size_t other_paths = 0;
  std::string path;
  const std::size_t last = kTokenCount;
  for (std::size_t depth = 1; depth <= 4; ++depth) {
    std::size_t idx[4] = {0, 0, 0, 0};
    for (;;) {
      path.clear();
      for (std::size_t k = 0; k < depth; ++k) {
        if (k != 0) {
          path.push_back('/');
        }
        path += kTokens[idx[k]];
      }
      const Place place = ag::classify(path.c_str());
      if (place != Place::None || lower_eq(parts_of(path)[0], "Apps")) {
        const bool under_apps = lower_eq(parts_of(path)[0], "Apps");
        if (under_apps && place != Place::None) {
          ++apps_paths;
          check_apps_path(path, place);
        } else if (!under_apps) {
          ++other_paths;
          CHECK(place == sv::classify(path.c_str()));  // outside Apps the answer is the old guard's, byte for byte
        }
        check_operations(path);
      } else {
        CHECK(sv::classify(path.c_str()) == Place::None);
      }
      std::size_t k = depth;
      while (k != 0 && ++idx[k - 1] == last) {
        idx[k - 1] = 0;
        --k;
      }
      if (k == 0) {
        break;
      }
    }
  }
  CHECK(apps_paths > 1000u);
  CHECK(other_paths > 400u);
}
