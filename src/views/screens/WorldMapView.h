/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>

#include "core/content/WorldMap.h"
#include "core/food/IngredientCategoryType.h"
#include "core/state/UIState.h"
#include "engine/DebugLabel.h"
#include "views/base/View.h"

// The Expanded world map (contract step 9): travel between the places of a
// game whose roster is not the original ten. Eight regional overviews plus the
// bonus routes, the places' markers in a flat frame, a paged list beside it,
// previous/next through the list, and travel only after a confirmation.
// Browsing changes nothing but this view: it keeps the clock stopped, charges
// nothing and draws no random number. Classic games keep EuroMap. No map art
// is drawn: generated overview art is not admitted yet.
class WorldMapView : public View, public debug::AutoLabel<WorldMapView> {
public:
  // Travel (MapMode::TRAVEL) or a depot export of one ingredient category
  // (MapMode::DEPOT), as EuroMapState asks.
  WorldMapView(GameContext& context, GameState& state,
               const EuroMapState& request);
  void tick() override;
  void render(const RenderContext& ctx) override;
  ClockRate clock_rate() const override { return ClockRate::Stopped; }

private:
  void act();
  void ask_to_travel();
  void travel_to(core::CityId target);
  void export_to(core::CityId target, Money cost);
  // Why the selected place takes no export, or nullopt when it does.
  [[nodiscard]] std::optional<std::string> export_refusal() const;
  [[nodiscard]] bool has_depot_in(core::CityId city) const;
  [[nodiscard]] bool exporting() const { return mode_ == MapMode::DEPOT; }
  [[nodiscard]] const core::content::WorldEntry* here() const;

  core::content::WorldBrowser browser_;
  core::content::TravelRates rates_;
  MapMode mode_;
  std::optional<IngredientCategory> category_;
  bool browse_only_;
  // Travel is off in this game: the map closes as soon as it opens.
  bool closes_at_once_{false};
};
