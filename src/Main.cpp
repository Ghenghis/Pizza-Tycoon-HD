/*
 * SPDX-FileCopyrightText: 2010 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/Game.h"
#include "engine/SDL_include.h"
#include "engine/Version.h"
#include "engine/gfx/HdImage.h"

// After the engine headers: windows.h defines macros (TRANSPARENT, ...) that
// would clash with engine names.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// On laptops with two GPUs, ask the NVIDIA and AMD drivers for the fast one:
// they look for these exported names in the program itself.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace { // anonymous
[[noreturn]] void version() {
  std::cout << "Version: " << game_meta::PRODUCT << " " << game_meta::VERSION
            << " (Pizza Legacy " << game_meta::UPSTREAM_VERSION << ")\n";
  std::cout << "Copyright: 2026 Cowomaly\n";
  std::cout << "License: GPL-3.0-or-later\n";
  std::cout << "Website: https://pizzalegacy.nl/\n";
  std::cout << "Repository: https://codeberg.org/cowomaly/pizzalegacy/\n";
  std::exit(0);
}

[[noreturn]] void usage(const char* prog, int code) {
  std::cerr
      << "Usage:\n"
      << "  " << prog << "                        # normal boot (Intro)\n"
      << "  " << prog << " <file|fonts|sounds|pal|char>  # open viewer\n"
      << "  " << prog
      << " console <cmd...>       # run one console line on startup\n"
      << "Options:\n"
      << "  --config <file>                        # config path (default: "
         "platform pref dir)\n"
      << "  --game-path <dir>                      # override game data dir\n"
      << "  --exec <file>                          # run console commands "
         "from a file, then exit\n"
      << "  --seed <n>                             # specify the RNG seed\n"
      << "  --vga <path>                           # open VGA viewer with "
         "file\n"
      << "  --bob <path>                           # open BOB viewer with "
         "file\n"
      << "  --card <text>                          # show a title card, '|' "
         "breaks a line\n"
      << "  --crt                                  # force CRT aspect ratio "
         "(4:3)\n"
      << "  --hd-scale <1-8>                       # HD render scale (3 = "
         "1920x1200, 6 = 3840x2400)\n"
      << "  --hd-pack <dir>                        # folder of HD "
         "replacement art\n"
      << "  --export-assets <dir>                  # write every original "
         "picture as PNG + manifest.json, then exit\n"
      << "  -h, --help\n";
  std::exit(code);
}
} // namespace

static Game::LaunchOptions parse_args(int argc, char* argv[]) {
  Game::LaunchOptions opts;
  bool seen_positional = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      usage(argv[0], 0);
    }
    if (arg == "-v" || arg == "--version") {
      version();
    }
    else if (arg == "--") {
      // Everything after '--' is the first positional (viewer target).
      if (i + 1 < argc) {
        opts.viewer_target = std::string(argv[i + 1]);
      }
      break;
    }
    else if (arg == "--config") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.config_path = argv[++i];
    }
    else if (arg == "--game-path") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.game_path = std::string(argv[++i]);
    }
    else if (arg == "--exec") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.exec_script = std::string(argv[++i]);
    }
    else if (arg == "--seed") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.rng_seed =
          static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    }
    else if (arg == "--crt") {
      opts.crt_mode = true;
    }
    else if (arg == "--vga") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.vga_viewer = argv[++i];
    }
    else if (arg == "--bob") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.bob_viewer = argv[++i];
    }
    else if (arg == "--hd-scale") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      const std::string value = argv[++i];
      int scale = 0;
      auto [ptr, ec] =
          std::from_chars(value.data(), value.data() + value.size(), scale);
      if (ec != std::errc{} || ptr != value.data() + value.size() ||
          scale < 1 || scale > gfx::max_hd_scale) {
        std::cerr << "error: --hd-scale needs a whole number from 1 to "
                  << gfx::max_hd_scale << "\n";
        usage(argv[0], 2);
      }
      opts.hd_scale = scale;
    }
    else if (arg == "--hd-pack") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.hd_pack = std::string(argv[++i]);
    }
    else if (arg == "--export-assets") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.export_assets = std::string(argv[++i]);
    }
    else if (arg == "--card") {
      if (i + 1 >= argc) {
        usage(argv[0], 2);
      }
      opts.card_text = argv[++i];
    }
    else if (arg == "console") {
      // The rest is the console command, one argument per word as the shell
      // split it.
      std::vector<std::string> args;
      for (int j = i + 1; j < argc; ++j) {
        args.emplace_back(argv[j]);
      }
      if (args.empty()) {
        std::cerr << "error: 'console' requires a command.\n";
        usage(argv[0], 2);
      }
      opts.console_args = std::move(args);
      // 'console' subcommand consumes the rest; ignore any other positionals.
      break;
    }
    else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "error: unknown option: " << arg << "\n";
      usage(argv[0], 2);
    }
    else {
      // Positional argument -> viewer target (keep only the first one).
      if (!seen_positional) {
        opts.viewer_target = arg;
        seen_positional = true;
      }
      else {
        std::cerr << "warning: extra positional ignored: " << arg << "\n";
      }
    }
  }

  return opts;
}

#ifdef _WIN32
// The Windows build is a GUI program, so no console window opens next to the
// game. When it is started from a terminal (exports, --help, scripted runs),
// it borrows that terminal for its output. Output that the parent already
// redirected (to a file or a pipe) is left alone.
static void attach_parent_console() {
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE) {
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  FILE* stream = nullptr;
  if (freopen_s(&stream, "CONOUT$", "w", stdout) == 0 &&
      freopen_s(&stream, "CONOUT$", "w", stderr) == 0) {
    std::cout.clear();
    std::cerr.clear();
  }
}
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
  attach_parent_console();
#endif

  Game::LaunchOptions opts = parse_args(argc, argv);

  auto game = std::make_unique<Game>(std::move(opts));
  return game->run();
}
