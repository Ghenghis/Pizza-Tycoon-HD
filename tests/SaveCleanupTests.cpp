/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// What writes cut short by a crash leave behind: AtomicFile's temporary files.
// The game clears them at startup once they are old enough that no write can
// still be using them, and nothing else.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "TestSupport.h"
#include "core/save/SaveGameManager.h"
#include "core/util/AtomicFile.h"
#include "engine/Logger.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using save::SaveGameManager;
using test_support::TempDir;

namespace {

// A file last written `age` ago.
void make_file(const fs::path& path, std::chrono::hours age) {
  {
    std::ofstream out(path, std::ios::binary);
    out << "x";
    REQUIRE(out.good());
  }
  fs::last_write_time(path, fs::file_time_type::clock::now() - age);
}

std::set<std::string> names_in(const fs::path& dir) {
  std::set<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    names.insert(entry.path().filename().string());
  }
  return names;
}

// Whether one of the logged lines holds every one of the texts.
bool logged(const std::vector<std::string>& lines,
            std::initializer_list<std::string_view> texts) {
  return std::ranges::any_of(lines, [&texts](const std::string& line) {
    return std::ranges::all_of(texts, [&line](std::string_view text) {
      return line.find(text) != std::string::npos;
    });
  });
}

} // namespace

TEST_CASE("Only AtomicFile's temporary names count as leftovers",
          "[save][cleanup]") {
  for (const char* name :
       {"game.plsave.tmp-0123456789ab", "settings.conf.tmp-ffffffffffff",
        "a.tmp-000000000000"}) {
    INFO(name);
    CHECK(util::is_temp_file_name(name));
  }
  for (const char* name :
       {"", ".tmp-0123456789ab", "game.plsave", "game.plsave.tmp-",
        "game.plsave.tmp-0123456789AB", "game.plsave.tmp-0123456789a",
        "game.plsave.tmp-0123456789abc", "game.plsave.tmp-0123456789ag",
        "game.plsave.tmp_0123456789ab", "x.TMP-0123456789ab"}) {
    INFO(name);
    CHECK_FALSE(util::is_temp_file_name(name));
  }

  // The names AtomicFile really gives match.
  TempDir dir;
  util::AtomicFile out(dir.path() / "game.plsave");
  REQUIRE(out.open());
  CHECK(util::is_temp_file_name(out.temp_path().filename().string()));
}

TEST_CASE("Leftovers past the age go and everything else stays",
          "[save][cleanup]") {
  TempDir dir;
  const fs::path& folder = dir.path();
  make_file(folder / "game.plsave.tmp-0123456789ab", 2h);
  // Young enough to be a write still going on.
  make_file(folder / "settings.conf.tmp-abcdefabcdef", 0h);
  // Old, but not AtomicFile's.
  make_file(folder / "game.plsave", 2h);
  make_file(folder / "notes.tmp-0123456789AB", 2h);
  make_file(folder / "game.plsave.tmp-123", 2h);
  // A folder with the name, and a leftover one level down.
  fs::create_directory(folder / "folder.tmp-0123456789ab");
  std::error_code ignored;
  fs::last_write_time(folder / "folder.tmp-0123456789ab",
                      fs::file_time_type::clock::now() - 2h, ignored);
  fs::create_directory(folder / "sub");
  make_file(folder / "sub" / "a.plsave.tmp-0123456789ab", 2h);
  std::set<std::string> kept = {
      "settings.conf.tmp-abcdefabcdef", "game.plsave",
      "notes.tmp-0123456789AB",         "game.plsave.tmp-123",
      "folder.tmp-0123456789ab",        "sub"};
#ifndef _WIN32
  // A link with the name is not a temporary file.
  fs::create_symlink(folder / "game.plsave", folder / "link.tmp-0123456789ab");
  kept.insert("link.tmp-0123456789ab");
#endif

  const util::StaleTempFiles result = util::remove_stale_temp_files(folder, 1h);
  REQUIRE(result.removed.size() == 1);
  CHECK(result.removed[0].filename() == "game.plsave.tmp-0123456789ab");
  CHECK(result.failed.empty());
  CHECK(names_in(folder) == kept);
  CHECK(fs::exists(folder / "sub" / "a.plsave.tmp-0123456789ab"));

  // A folder that isn't there has nothing to clear.
  const util::StaleTempFiles missing =
      util::remove_stale_temp_files(folder / "missing", 1h);
  CHECK(missing.removed.empty());
  CHECK(missing.failed.empty());
}

TEST_CASE("Clearing leftovers at startup logs every file it removes",
          "[save][cleanup]") {
  TempDir dir;
  const fs::path saves = dir.path() / "saves";
  fs::create_directory(saves);
  make_file(saves / "AUTOSAV1.plsave.tmp-0123456789ab", 3h);
  make_file(saves / "highscore.dat.tmp-00000000beef", 2h);
  make_file(dir.path() / "settings.conf.tmp-fedcba987654", 2h);
  make_file(dir.path() / "bindings.conf.tmp-0f0f0f0f0f0f", 0h);
  make_file(saves / "AUTOSAV1.plsave", 3h);

  std::vector<std::string> lines;
  const Logger::ScopedSink sink(Logger::add_sink(
      [&lines](LogLevel /*level*/, const std::string& line) {
        lines.push_back(line);
      },
      LogLevel::Info));

  // The settings and bindings files share a folder, and "saves/" is "saves":
  // each folder is looked at once.
  const int removed = SaveGameManager::remove_stale_temp_files(
      {dir.path() / "saves/", dir.path(), dir.path(), dir.path() / "missing"},
      save::STALE_TEMP_FILE_AGE);
  CHECK(removed == 3);
  for (const char* name :
       {"AUTOSAV1.plsave.tmp-0123456789ab", "highscore.dat.tmp-00000000beef",
        "settings.conf.tmp-fedcba987654"}) {
    INFO(name);
    CHECK(logged(lines, {"Removed ", name}));
  }
  CHECK_FALSE(logged(lines, {"bindings.conf.tmp-0f0f0f0f0f0f"}));
  CHECK_FALSE(logged(lines, {"Could not clear"}));

  CHECK(names_in(saves) == std::set<std::string>{"AUTOSAV1.plsave"});
  CHECK(names_in(dir.path()) ==
        std::set<std::string>{"saves", "bindings.conf.tmp-0f0f0f0f0f0f"});
}
