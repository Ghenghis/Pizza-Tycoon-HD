/*
 * SPDX-FileCopyrightText: 2010 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "engine/SDLSession.h"
#include "engine/SDL_include.h"

#include "audio/AudioManager.h"
#include "core/util/ConfigManager.h"
#include "core/util/GameContext.h"
#include "engine/DisplayConstants.h"
#include "engine/SDLContext.h"
#include "engine/input/GlobalActionHandler.h"
#include "engine/music/MusicSystem.h"
#include "engine/record/Recorder.h"
#include "engine/ui/CursorManager.h"
#include "engine/ui/InteractionManager.h"
#include "engine/ui/PointerTracker.h"
#include "resources/BlitmapManager.h"
#include "resources/BobManager.h"
#include "resources/FontManager.h"
#include "resources/VgaManager.h"
#include "views/base/View.h"
#include "views/helpers//OverlayManager.h"

class ConsoleLayer;
class Renderer;
class DebugConsole;
class TutorialLayer;

class Game {
public:
  struct LaunchOptions {
    std::string config_path;              // empty = use platform default
    std::optional<std::string> game_path; // --game-path
    std::optional<std::string>
        viewer_target; // positional <file|fonts|pal|char>
    // "console <...>", one entry per argv word
    std::optional<std::vector<std::string>> console_args;
    std::optional<std::string> exec_script;   // --exec <file>, then exit
    std::optional<uint32_t> rng_seed;         // --seed <n>
    std::optional<bool> crt_mode;             // --crt to force CRT mode
    std::optional<std::string> vga_viewer;    // --vga <path>
    std::optional<std::string> bob_viewer;    // --bob <path>
    std::optional<std::string> card_text;     // --card <text>
    std::optional<int> hd_scale;              // --hd-scale <1-8>
    std::optional<std::string> hd_pack;       // --hd-pack <dir>
    std::optional<std::string> export_assets; // --export-assets <dir>
  };

  Game(LaunchOptions opts);
  ~Game();
  // Process exit code; non-zero when a --exec script had a command fail.
  int run();

  bool capture_screenshot(GameState& gs, const std::string& path);
  // Renders the current screen `frames` times as fast as possible, with the
  // simulation running and every layer redrawn each frame (the worst case),
  // and describes the frame times. Empty when there is nothing to render.
  std::string benchmark_frames(GameState& gs, int frames);
  bool write_screen_as_vga(GameState& gs, const std::string& path);

  // Recording (record start|stop|status|clip).
  [[nodiscard]] record::Recorder* recorder() { return recorder_.get(); }
  // Whether frames come from the software path (no GPU readback): the only
  // one recorded so far.
  [[nodiscard]] static bool records_classic();
  // Batch mode: records seconds of the current screen, with the simulation
  // and the sound running in real time (with music: an original song), and
  // returns how it went. Empty when there is nothing to render.
  std::string record_clip(GameState& gs, double seconds,
                          const std::string& folder, bool music);

private:
  struct LoopState {
    OverlayManager* overlays = nullptr;
    GameState& game_state;
    GameContext& context;
    bool screenshot_requested = false;
    bool fade_on_next_transition = false;
    bool skip_tick_during_fade_in = false;
    bool redraw_requested = false;
    // The GPU's render targets (1) or the whole device (2) were reset.
    int render_reset = 0;
  };

  bool init(int display_height, const GameOptions& options);
  // F7 (compare false) and F8 (compare true).
  void toggle_look(LoopState& loop_state, bool compare);
  // After a frame is drawn: copies it for a recording when one is due.
  void record_frame(Renderer& renderer);
  Renderable* change_state(GameState& gs);
  void apply_fullscreen(bool enabled);
  void load_game_data();
  bool enforce_aspect_ratio(SDL_Window*, int, int);
  void handle_events(LoopState& loop_state);
  void handle_resize_event(const SDL_Event& event, LoopState& loop_state);
  ScreenPoint global_cursor_pos_clamped() const;
  static int get_display_height(bool crt_mode);
  SDL_Texture* create_display_texture(int display_height) const;

  // SDLSession needs to be declared first so it's destroyed last:
  SDLSession sdl_session_;

  bool window_resizing_in_progress_{false};
  int display_height_{DisplayConstants::SCREEN_HEIGHT};
  std::optional<bool> crt_mode_override_;
  std::optional<int> hd_scale_override_;
  std::optional<std::string> hd_pack_override_;
  std::optional<std::string> export_assets_dir_;
  // Applies hd_scale and finds the HD pack folder. Before init().
  void configure_hd(const GameOptions& options) const;
  // hd_scale 0 (Auto): from the primary screen's height in pixels.
  [[nodiscard]] static int auto_hd_scale();
  // --export-assets: writes the PNGs and manifest, returns the exit code.
  int run_asset_export(const std::string& game_path) const;
  std::unique_ptr<BobManager> bob_manager_;
  std::unique_ptr<FontManager> font_manager_;
  std::unique_ptr<VgaManager> vga_manager_;
  std::unique_ptr<BlitmapManager> blitmap_manager_;
  std::unique_ptr<CursorManager> cursor_manager_;
  std::unique_ptr<TooltipManager> tooltip_manager_;
  std::unique_ptr<views::ui::InteractionManager> interaction_manager_;
  std::unique_ptr<views::ui::PointerTracker> pointer_;
  std::unique_ptr<input::InputManager> input_manager_{nullptr};
  std::unique_ptr<audio::AudioManager> audio_manager_;
  // After the AudioManager, so it is destroyed first.
  std::unique_ptr<music::MusicSystem> music_;
  // After the AudioManager too: it reads its tap.
  std::unique_ptr<record::Recorder> recorder_;
  std::unique_ptr<View> current_view_{nullptr};
  std::unique_ptr<GlobalActionHandler> global_actions_;
  TutorialLayer* tutorial_layer_{nullptr};
  GameContext context;
  SDLContext sdl_;
  SDL_Surface_unique_ptr screen_;
  Surface surface_;
  bool prev_relative_mouse_mode_ = false;
  // Display only (F7 / F8): the 1994 look, and the two looks side by side.
  bool classic_look_ = false;
  bool compare_looks_ = false;
  bool warp_pointer_on_resize_ = false;
  ScreenPoint pre_relative_mouse_pos_;
  std::string filename_;
  std::string config_path_;
  std::optional<std::string> game_path_override_;
  std::optional<std::vector<std::string>> startup_console_args_;
  std::optional<std::string> exec_script_;
  std::optional<uint32_t> rng_seed_;

  // Returns process exit code.
  int run_exec_script(DebugConsole& console, GameState& state) const;
  std::optional<std::string> vga_viewer_path_;
  std::optional<std::string> bob_viewer_path_;
  std::optional<std::string> card_text_;
};
