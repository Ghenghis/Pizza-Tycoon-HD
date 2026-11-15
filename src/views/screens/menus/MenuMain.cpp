/*
 * SPDX-FileCopyrightText: 2010 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/menus/MenuMain.h"
#include "core/save/SaveGameManager.h"
#include "core/state/GameState.h"
#include "engine/DisplayConstants.h"
#include "engine/Logger.h"
#include "engine/PlatformPaths.h"
#include "engine/Version.h"
#include "resources/Font.h"
#include "views/screens/menus/MenuTheme.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/AlertPopup.h"
#include "views/ui/popups/ConfirmPopup.h"
#include "views/ui/widgets/LabelMenuItem.h"
#include "views/ui/widgets/PositionMarkerMenuItem.h"
#include "views/ui/widgets/SpacerMenuItem.h"
#include "views/ui/widgets/TextMenuItem.h"

using namespace views::ui::menu;

MenuMain::MenuMain(GameContext& context, GameState& state)
    : Menu(context, state, ViewPoint{0, 210}, -8) {
  constexpr int space_between_lines = 24;
  add_menu_item(std::make_unique<TextMenuItem>(
      context, "Old Game",
      [&state] { state.ui().push<ScreenTag>(ScreenTag::Kind::MenuGameLoad); },
      face, style, sel_style, item_align));
  add_menu_item(std::make_unique<SpacerMenuItem>(context, space_between_lines));

  add_menu_item(std::make_unique<TextMenuItem>(
      context, "New Game",
      [&state] { state.ui().push<ScreenTag>(ScreenTag::Kind::MenuNewGame); },
      face, style, sel_style, item_align));
  add_menu_item(std::make_unique<SpacerMenuItem>(context, space_between_lines));

  add_menu_item(std::make_unique<TextMenuItem>(
      context, "High Scores",
      [&state] { state.ui().push<ScreenTag>(ScreenTag::Kind::MenuHighScores); },
      face, style, sel_style, item_align));

  add_menu_item(std::make_unique<SpacerMenuItem>(context, space_between_lines));

  add_menu_item(std::make_unique<TextMenuItem>(
      context, "Options",
      [&state] { state.ui().push<ScreenTag>(ScreenTag::Kind::MenuOptions); },
      face, style, sel_style, item_align));
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 276, 164));
  add_menu_item(std::make_unique<TextMenuItem>(
      context, "Exit", [&state] { state.request_exit(); }, FontFace::RUBY,
      style, sel_style, item_align));

  // Debug info in small text in the corner:
  ViewPoint debug_info{10, 170};
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, debug_info.x,
                                                         debug_info.y));
  TextStyle ts{
      .color = 11,
  };
  {
    std::string version_str = std::string(game_meta::PRODUCT) + " " +
                              game_meta::VERSION + " (Pizza Legacy " +
                              game_meta::UPSTREAM_VERSION + ")";
#ifndef NDEBUG
    version_str += " (DEBUG)";
#endif
    add_menu_item(std::make_unique<LabelMenuItem>(
        context, std::move(version_str), FontFace::MINI, ts));
  }
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, debug_info.x,
                                                         debug_info.y + 10));
  ts.color = 13;
  const std::string& base = platform_paths::base_dir();

  add_menu_item(std::make_unique<LabelMenuItem>(
      context, "User directory: " + base, FontFace::MINI, ts));

  offer_resume();
}

void MenuMain::offer_resume() {
  const std::optional<save::ResumeOffer> offer =
      save::SaveGameManager::take_resume_offer(state_, context_);
  if (!offer) {
    return;
  }
  context_.popup->origin(ViewPoint{26, 40});
  context_.popup->create<ConfirmPopup>(
      compat::format("Resume where you left off: day {}, {}?", offer->day,
                     offer->time),
      [this, offer = *offer](bool resume) {
        if (!resume) {
          Logger::info("Not resuming the last run");
          return;
        }
        std::string error;
        std::string notice;
        if (!save::SaveGameManager::load_version(state_, context_,
                                                 offer.slot_file,
                                                 offer.sequence, &error,
                                                 &notice)) {
          context_.popup->create<AlertPopup>(error);
        }
        else if (!notice.empty()) {
          context_.popup->create<AlertPopup>(notice);
        }
      },
      nullptr, "Resume", "Not now");
}

Surface MenuMain::background() const {
  return context_.vga->get("gfx/menu").clone();
}
