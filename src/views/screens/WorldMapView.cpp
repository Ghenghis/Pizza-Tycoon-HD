/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/WorldMapView.h"

#include <algorithm>
#include <array>
#include <string>

#include "core/character/Player.h"
#include "core/city/City.h"
#include "core/city/Property.h"
#include "core/finance/Ledger.h"
#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "engine/DisplayConstants.h"
#include "engine/FormatCompat.h"
#include "engine/gfx/Draw.h"
#include "engine/gfx/Surface.h"
#include "engine/ui/InteractionEvent.h"
#include "resources/FontManager.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/ConfirmPopup.h"

namespace {

using core::content::WorldBrowser;
using core::content::WorldEntry;

// The colours of EuroMap's popups (palette 2), so the flat frames read like
// the original map's boxes.
const PaletteHandle palette{2};
constexpr uint8_t background = 24;
constexpr uint8_t light = 23;
constexpr uint8_t dark = 32;
constexpr uint8_t text = 20;
constexpr uint8_t highlight = 255;

constexpr ViewRect map_frame{16, 56, 360, 270};
constexpr int list_x = 392;
constexpr int list_y = 60;
constexpr int row_height = 20;
constexpr int marker_size = 8;

#if defined(__ANDROID__)
constexpr std::size_t page_size = core::content::phone_page_size;
#else
constexpr std::size_t page_size = core::content::desktop_page_size;
#endif

std::string view_label(std::size_t view) {
  if (view == WorldBrowser::bonus_view) {
    return "Bonus routes";
  }
  return std::string(core::content::regions[view].label);
}

// Short tab names; the full name is the title.
constexpr std::array<std::string_view, WorldBrowser::view_count> tab_labels = {
    "N.America", "Lat.Am.", "Europe",  "N.Afr/W.As", "S.Africa",
    "C/S Asia",  "E/SE As", "Oceania", "Bonus"};

bool clicked(const views::ui::InteractionEvent& ev) {
  return ev.type == views::ui::InteractionEvent::Type::Click &&
         ev.button == views::ui::MouseButton::Left;
}

} // namespace

WorldMapView::WorldMapView(GameContext& context, GameState& state,
                           const EuroMapState& request)
    : View(context, state),
      browser_(core::content::roster_world_entries(state.roster()), page_size),
      rates_(core::content::travel_rates(state.scenario())),
      mode_(request.mode), category_(request.category),
      browse_only_(request.browse_only) {
  closes_at_once_ = !browse_only_ && !state_.travel_enabled();
  if (closes_at_once_) {
    return;
  }
  // Open on the region of the place the player is in, with it selected, or
  // on the view asked for.
  if (const auto* current = here()) {
    browser_.show(current->region.value_or(WorldBrowser::bonus_view));
    browser_.select(current->key);
  }
  if (request.world_view) {
    browser_.show(static_cast<std::size_t>(*request.world_view));
  }

  for (std::size_t v = 0; v < WorldBrowser::view_count; ++v) {
    const ViewRect tab{8 + static_cast<int>(v) * 69, 30, 66, 18};
    interactions_.create_persistent_item(
        tab, compat::format("worldmap.region.{}", v),
        [this, v](views::ui::InteractionEvent ev) {
          if (clicked(ev)) {
            browser_.show(v);
            mark_dirty();
          }
        });
  }
  const auto button = [this](ViewRect r, std::string id, auto action) {
    interactions_.create_persistent_item(
        r, id, [this, action](views::ui::InteractionEvent ev) {
          if (clicked(ev)) {
            action();
            mark_dirty();
          }
        });
  };
  button(ViewRect{list_x, 312, 54, 18}, "worldmap.previous",
         [this] { browser_.previous(); });
  button(ViewRect{list_x + 178, 312, 54, 18}, "worldmap.next",
         [this] { browser_.next(); });
  button(ViewRect{list_x + 60, 312, 24, 18}, "worldmap.page_previous",
         [this] { browser_.page(browser_.page() - 1); });
  button(ViewRect{list_x + 148, 312, 24, 18}, "worldmap.page_next",
         [this] { browser_.page(browser_.page() + 1); });
  if (!browse_only_) {
    button(ViewRect{list_x, 350, 110, 22}, "worldmap.travel",
           [this] { act(); });
  }
  button(ViewRect{list_x + 122, 350, 110, 22}, "worldmap.back",
         [this] { state_.ui().pop(); });
}

const WorldEntry* WorldMapView::here() const {
  for (const auto& e : browser_.entries()) {
    if (e.city == state_.active_city_id()) {
      return &e;
    }
  }
  return nullptr;
}

void WorldMapView::tick() {
  if (closes_at_once_) {
    closes_at_once_ = false;
    state_.ui().pop();
  }
}

void WorldMapView::act() {
  if (exporting()) {
    const auto* target = browser_.selected();
    const auto* from = here();
    if (!target || !from || !target->city) {
      return;
    }
    if (target->key == from->key) {
      // As EuroMap: choosing the city the depot is in goes back.
      state_.ui().pop();
      return;
    }
    if (export_refusal()) {
      return;
    }
    export_to(*target->city,
              core::content::travel_cost(*from, *target, rates_,
                                         state_.difficulty_level()));
    return;
  }
  ask_to_travel();
}

bool WorldMapView::has_depot_in(core::CityId city_id) const {
  // As EuroMap::player_has_depot_in_city.
  if (!state_.is_city_loaded(city_id)) {
    return false;
  }
  const auto* city = state_.city(city_id);
  const auto* player = state_.active_player();
  if (!city || !player) {
    return false;
  }
  for (const auto* property : city->properties()) {
    if (property->type() == PropertyType::DEPOT) {
      const auto owner = property->owner_or_tenant();
      if (owner && *owner == player->id()) {
        return true;
      }
    }
  }
  return false;
}

std::optional<std::string> WorldMapView::export_refusal() const {
  const auto* target = browser_.selected();
  if (!target || !target->city) {
    return std::string("Choose a place of this game.");
  }
  if (!has_depot_in(*target->city)) {
    return "You do not have a central depot in " + target->name + ".";
  }
  const auto* city = state_.city(*target->city);
  if (category_ && state_.rules().fix_legacy_bugs && city &&
      city->trade_embargo_counter(*category_)) {
    return target->name + " is under embargo!";
  }
  return std::nullopt;
}

void WorldMapView::export_to(core::CityId target, Money cost) {
  // As EuroMap's export box: the export screen takes it from here.
  if (category_) {
    state_.ui().replace_top<DepotExportState>(*category_, target, cost);
  }
}

void WorldMapView::ask_to_travel() {
  const auto* target = browser_.selected();
  const auto* from = here();
  if (!target || !from || !target->city || target->key == from->key) {
    return;
  }
  const Money cost = core::content::travel_cost(*from, *target, rates_,
                                                state_.difficulty_level());
  context_.popup->origin(ViewPoint{26, 40});
  context_.popup->create<ConfirmPopup>(
      compat::format("Travel to {} for {:w}?", target->name, cost),
      [this, city = *target->city, cost](bool agreed) {
        if (agreed && state_.spend_cash(MonthlyCategory::Others, cost)) {
          travel_to(city);
        }
      });
}

void WorldMapView::travel_to(core::CityId target) {
  // As EuroMap::attempt_flight: the fare is paid, no time passes.
  if (auto* player = state_.active_player()) {
    player->on_city_travel(target,
                           state_.time().current_date().week_day_index());
  }
  state_.pending_screen_charge(0);
  state_.ui().push<CityViewState>(target);
}

void WorldMapView::render(const RenderContext& ctx) {
  interactions_.begin_frame();
  auto& s = *ctx.surface;
  s.palette(palette);
  s.fill(background);
  auto& font = *context_.font;

  font.render_text_onto(s, view_label(browser_.view()),
                        ViewPoint{DisplayConstants::SCREEN_WIDTH / 2, 8},
                        FontFace::ASTANDAR, text, TextAnchor::Middle);

  for (std::size_t v = 0; v < WorldBrowser::view_count; ++v) {
    const ViewRect tab{8 + static_cast<int>(v) * 69, 30, 66, 18};
    gfx::draw::beveled_box(s, tab, v == browser_.view() ? dark : light,
                           v == browser_.view() ? light : dark, 1);
    const bool empty = browser_.count_in(v) == 0;
    font.render_text_onto(s, tab_labels[v], ViewPoint{tab.x + 33, tab.y + 4},
                          FontFace::MINI, empty ? light : text,
                          TextAnchor::Middle);
  }

  // The overview: a flat frame with the places at their coordinates.
  gfx::draw::beveled_box(s, map_frame, dark, light, 2);
  const auto* chosen = browser_.selected();
  const auto* current = here();
  for (const auto& marker : browser_.markers()) {
    const int x = map_frame.x + 4 + (marker.at.x * (map_frame.w - 8) / 10000);
    const int y = map_frame.y + 4 + (marker.at.y * (map_frame.h - 8) / 10000);
    const bool picked = std::find(marker.members.begin(), marker.members.end(),
                                  chosen) != marker.members.end();
    const ViewRect dot{x - (marker_size / 2), y - (marker_size / 2),
                       marker_size, marker_size};
    s.fill_rect(dot, picked ? highlight : text);
    const std::string label =
        marker.members.size() == 1
            ? marker.members.front()->name
            : std::to_string(marker.members.size()) + " places";
    font.render_text_onto(s, label, ViewPoint{x + 6, y - 4}, FontFace::MINI,
                          marker.members.front() == current ? highlight : text);
    const auto* first = marker.members.front();
    interactions_.create_frame_item(
        dot.inset(-4, -4),
        compat::format("worldmap.marker.{}", first->key.str()),
        [this, first](views::ui::InteractionEvent ev) {
          if (clicked(ev)) {
            browser_.select(first->key);
            mark_dirty();
          }
        });
  }
  if (browser_.view() == WorldBrowser::bonus_view) {
    font.render_text_onto(s, "Bonus routes have no place on the globe.",
                          ViewPoint{map_frame.x + 12, map_frame.y + 12},
                          FontFace::SUPERSMA, text);
  }

  // The list, one page of it.
  int y = list_y;
  const auto rows = browser_.page_entries();
  if (rows.empty()) {
    font.render_text_onto(s, "No place of this game here.",
                          ViewPoint{list_x, y}, FontFace::SUPERSMA, text);
  }
  for (const auto* e : rows) {
    const ViewRect row{list_x - 2, y - 2, 236, row_height - 2};
    if (e == chosen) {
      s.fill_rect(row, light);
    }
    const std::string name = e == current ? e->name + " (here)" : e->name;
    font.render_text_onto(s, name, ViewPoint{list_x, y}, FontFace::SUPERSMA,
                          e == chosen ? highlight : text);
    interactions_.create_frame_item(
        row, compat::format("worldmap.place.{}", e->key.str()),
        [this, e](views::ui::InteractionEvent ev) {
          if (clicked(ev)) {
            browser_.select(e->key);
            mark_dirty();
          }
        });
    y += row_height;
  }

  // Previous/next and the page.
  font.render_text_onto(s, "< Prev", ViewPoint{list_x, 314}, FontFace::SUPERSMA,
                        browser_.can_previous() ? text : light);
  font.render_text_onto(s, "Next >", ViewPoint{list_x + 178, 314},
                        FontFace::SUPERSMA, browser_.can_next() ? text : light);
  if (browser_.paging_enabled()) {
    font.render_text_onto(s, "<", ViewPoint{list_x + 66, 314},
                          FontFace::SUPERSMA, text);
    font.render_text_onto(s, ">", ViewPoint{list_x + 154, 314},
                          FontFace::SUPERSMA, text);
  }
  font.render_text_onto(
      s, compat::format("Page {} of {}", browser_.page(), browser_.pages()),
      ViewPoint{list_x + 116, 314}, FontFace::SUPERSMA, text,
      TextAnchor::Middle);

  // What the selection costs, and the buttons.
  bool can_act = !browse_only_ && chosen && current && chosen->city &&
                 chosen->key != current->key;
  if (can_act && exporting()) {
    if (const auto refusal = export_refusal()) {
      font.render_text_onto(s, *refusal, ViewPoint{list_x, 334},
                            FontFace::SUPERSMA, text);
      can_act = false;
    }
  }
  if (can_act) {
    const Money cost = core::content::travel_cost(*current, *chosen, rates_,
                                                  state_.difficulty_level());
    const std::string fare = exporting()
                                 ? compat::format("Export will cost {:w}", cost)
                                 : compat::format("Fare: {:w}", cost);
    font.render_text_onto(s, fare, ViewPoint{list_x, 334}, FontFace::SUPERSMA,
                          text);
  }
  if (!browse_only_) {
    gfx::draw::beveled_box(s, ViewRect{list_x, 350, 110, 22}, light, dark, 1);
    font.render_text_onto(s, exporting() ? "Export" : "Travel",
                          ViewPoint{list_x + 55, 354}, FontFace::SUPERSMA,
                          can_act ? text : light, TextAnchor::Middle);
  }
  gfx::draw::beveled_box(s, ViewRect{list_x + 122, 350, 110, 22}, light, dark,
                         1);
  font.render_text_onto(s, "Back", ViewPoint{list_x + 177, 354},
                        FontFace::SUPERSMA, text, TextAnchor::Middle);
}
