/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/menus/MenuNorthPole.h"

#include <string>

#include "core/content/Findings.h"
#include "core/content/ShippedScenarios.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "engine/Logger.h"
#include "views/screens/menus/MenuTheme.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/AlertPopup.h"
#include "views/ui/widgets/PositionMarkerMenuItem.h"
#include "views/ui/widgets/SpacerMenuItem.h"
#include "views/ui/widgets/TextMenuItem.h"

using namespace views::ui::menu;

MenuNorthPole::MenuNorthPole(GameContext& context, GameState& state)
    : Menu(context, state, ViewPoint{0, 130}, 0) {
  constexpr int space_between_lines = 14;

  for (const auto& preset : core::content::north_pole_presets) {
    add_menu_item(std::make_unique<TextMenuItem>(
        context, std::string(preset.menu_label),
        [this, preset] {
          Logger::debug("New game of {}", preset.file);
          state_.begin_initialization();
          core::content::Findings findings;
          if (!core::content::prepare_shipped_scenario(
                  preset.file, state_.initializer(), state_.options().rules,
                  findings)) {
            Logger::warn("Cannot start {}:\n{}", preset.file,
                         core::content::describe(findings));
            context_.popup->origin(ViewPoint{26, 40});
            context_.popup->create<AlertPopup>(
                "This preset is missing or damaged in the data folder.");
            return;
          }
          state_.ui().push<ScreenTag>(ScreenTag::Kind::CharacterSelection);
        },
        face, style, sel_style, item_align));
    add_menu_item(
        std::make_unique<SpacerMenuItem>(context, space_between_lines));
  }

  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 270, 244));
  add_menu_item(std::make_unique<TextMenuItem>(
      context, "Back", [this] { state_.ui().pop(); }, FontFace::RUBY, style,
      sel_style, item_align));
}

Surface MenuNorthPole::background() const {
  return default_background(std::string(core::content::north_pole_menu_label));
}
