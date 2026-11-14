/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/content/Scenario.h"

#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "core/content/ClassicCities.h"
#include "core/content/JsonShape.h"
#include "core/content/LocationReader.h"
#include "core/content/Roster.h"
#include "core/init/NewGameConfig.h"
#include "core/state/GameRules.h"
#include "core/util/Constants.h"

namespace core::content {

namespace {

using json = nlohmann::json;
using core::events::MilestoneId;

constexpr std::uintmax_t max_file_bytes = 1024u * 1024u;

struct NamedMilestone {
  std::string_view name;
  MilestoneId id;
};

// scenario.schema.json $defs/supported_milestone: the milestones with a
// verified emitter (location-contract.md section 11.2).
constexpr std::array<NamedMilestone, 26> supported = {{
    {"OpenedFirstRestaurant", MilestoneId::OpenedFirstRestaurant},
    {"OpensSecondRestaurant", MilestoneId::OpensSecondRestaurant},
    {"HasFiveRestaurants", MilestoneId::HasFiveRestaurants},
    {"Sold1KPizzasPerDay", MilestoneId::Sold1KPizzasPerDay},
    {"Achieved10PctMarketShareInCity",
     MilestoneId::Achieved10PctMarketShareInCity},
    {"Achieved20PctMarketShareInCity",
     MilestoneId::Achieved20PctMarketShareInCity},
    {"Achieved50PctMarketShareInCity",
     MilestoneId::Achieved50PctMarketShareInCity},
    {"AwardedFirstStar", MilestoneId::AwardedFirstStar},
    {"FirstTwoStarRestaurant", MilestoneId::FirstTwoStarRestaurant},
    {"FirstThreeStarRestaurant", MilestoneId::FirstThreeStarRestaurant},
    {"AchievedTurnover10KPerDay", MilestoneId::AchievedTurnover10KPerDay},
    {"PizzaReachedNo1InCharts", MilestoneId::PizzaReachedNo1InCharts},
    {"AbleToMeet100PctDeliveries", MilestoneId::AbleToMeet100PctDeliveries},
    {"AverageServiceTimeLessThan4Minutes",
     MilestoneId::AverageServiceTimeLessThan4Minutes},
    {"HasAssetsOf100K", MilestoneId::HasAssetsOf100K},
    {"YouHaveAssetsOf500K", MilestoneId::YouHaveAssetsOf500K},
    {"YouHaveBecomeMillionaire", MilestoneId::YouHaveBecomeMillionaire},
    {"FullyTrainedMasterchef", MilestoneId::FullyTrainedMasterchef},
    {"FullyQualifiedWaiter", MilestoneId::FullyQualifiedWaiter},
    {"FirstPriceInPizzaCompetition", MilestoneId::FirstPriceInPizzaCompetition},
    {"Has25KWeeksProfitInOneRestaurant",
     MilestoneId::Has25KWeeksProfitInOneRestaurant},
    {"Has1000CustomersPerDayInOneRestaurant",
     MilestoneId::Has1000CustomersPerDayInOneRestaurant},
    {"PerfectDeliveriesFor1WeekAcross5Restaurants",
     MilestoneId::PerfectDeliveriesFor1WeekAcross5Restaurants},
    {"RepresentedInAllAreas", MilestoneId::RepresentedInAllAreas},
    {"TakesOutFirstLoan", MilestoneId::TakesOutFirstLoan},
    {"FinancialDifficultiesFirstTime",
     MilestoneId::FinancialDifficultiesFirstTime},
}};

bool avoidable(MilestoneId id) {
  return id == MilestoneId::TakesOutFirstLoan ||
         id == MilestoneId::FinancialDifficultiesFirstTime;
}

// Milestones not tagged with a city, which only make sense with one
// location (SCN-4).
bool city_scoped(MilestoneId id) {
  return id == MilestoneId::Achieved10PctMarketShareInCity ||
         id == MilestoneId::Achieved20PctMarketShareInCity ||
         id == MilestoneId::Achieved50PctMarketShareInCity ||
         id == MilestoneId::RepresentedInAllAreas;
}

// ^[a-z][a-z0-9-]{0,31}$
bool objective_id(std::string_view text) {
  return text.size() <= 32 && patterns::entity_id(text);
}

// ^[a-z][a-z0-9-]{1,31}:[a-z0-9][a-z0-9-]{1,47}$
bool costume_id(std::string_view text) {
  const auto colon = text.find(':');
  return colon != std::string_view::npos && text.size() - colon - 1 <= 48 &&
         ContentKey::is_valid(text);
}

class ScenarioParser {
public:
  explicit ScenarioParser(Findings& findings) : shape_(findings) {}

  std::shared_ptr<Scenario> parse(const json& doc) {
    auto out = std::make_shared<Scenario>();
    const std::string root;
    shape_.object(doc, root, {"preset_group", "presentation"},
                  {"format", "schema_version", "scenario_id", "revision",
                   "display", "roster", "start", "rules", "objectives",
                   "availability"});
    if (!doc.is_object()) {
      return nullptr;
    }
    if (const json* v = JsonShape::member(doc, "format")) {
      shape_.constant(*v, "/format", "pthd.scenario");
    }
    if (const json* v = JsonShape::member(doc, "schema_version")) {
      shape_.constant(*v, "/schema_version", "1.0.0");
    }
    if (auto id = shape_.text_at(doc, root, "scenario_id", 1, 96, false,
                                 patterns::content_key)) {
      out->id = *ContentKey::parse(*id);
    }
    out->revision = static_cast<int>(
        shape_.integer_at(doc, root, "revision", 1, 2147483647).value_or(1));
    shape_.text_at(doc, root, "preset_group", 1, 96, false,
                   patterns::content_key);
    if (const json* v = JsonShape::member(doc, "display")) {
      display(*v, *out);
    }
    if (const json* v = JsonShape::member(doc, "roster")) {
      roster(*v, *out);
    }
    if (const json* v = JsonShape::member(doc, "start")) {
      start(*v, *out);
    }
    if (const json* v = JsonShape::member(doc, "rules")) {
      rules(*v, *out);
    }
    if (const json* v = JsonShape::member(doc, "objectives")) {
      objectives(*v, *out);
    }
    if (const json* v = JsonShape::member(doc, "availability")) {
      const std::string p = "/availability";
      shape_.object(*v, p, {}, {"always_available", "calendar_lock"});
      if (const json* a = JsonShape::member(*v, "always_available");
          a && (!a->is_boolean() || !a->get<bool>())) {
        shape_.fail(p + "/always_available", "must be true");
      }
      if (const json* c = JsonShape::member(*v, "calendar_lock");
          c && (!c->is_boolean() || c->get<bool>())) {
        shape_.fail(p + "/calendar_lock", "must be false");
      }
    }
    if (const json* v = JsonShape::member(doc, "presentation")) {
      presentation(*v);
    }
    if (shape_.failed()) {
      return nullptr;
    }
    out->canonical_document = canonical_json(doc);
    out->sha256 = definition_sha256(doc);
    return out;
  }

private:
  using Ptr = const std::string&;

  static std::string at(Ptr p, std::string_view key) {
    return JsonShape::child(p, key);
  }
  static std::string at(Ptr p, std::size_t index) {
    return JsonShape::child(p, index);
  }

  void display(const json& v, Scenario& out) {
    const std::string p = "/display";
    shape_.object(
        v, p, {"subtitle", "difficulty_label"},
        {"title", "summary", "intro", "goal_summary", "victory_summary"});
    out.title = shape_.text_at(v, p, "title", 1, 40).value_or("");
    out.subtitle = shape_.text_at(v, p, "subtitle", 0, 60).value_or("");
    out.summary = shape_.text_at(v, p, "summary", 1, 300).value_or("");
    out.intro = shape_.text_at(v, p, "intro", 1, 600).value_or("");
    out.goal_summary =
        shape_.text_at(v, p, "goal_summary", 1, 300).value_or("");
    out.victory_summary =
        shape_.text_at(v, p, "victory_summary", 1, 600).value_or("");
    shape_.choice_at(v, p, "difficulty_label",
                     {"forgiving", "standard", "demanding", "long"});
  }

  std::optional<ContentKey> location_key(const json& v, Ptr p) {
    if (auto text = shape_.text(v, p, 1, 96, false, patterns::content_key)) {
      return ContentKey::parse(*text);
    }
    return std::nullopt;
  }

  void roster(const json& v, Scenario& out) {
    const std::string p = "/roster";
    if (!shape_.array(v, p, 1, 48)) {
      return;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
      const json& item = v[i];
      const std::string ip = at(p, i);
      shape_.object(item, ip, {}, {"location_key", "pin"});
      if (!item.is_object()) {
        continue;
      }
      ScenarioLocation location;
      if (const json* k = JsonShape::member(item, "location_key")) {
        if (auto key = location_key(*k, at(ip, "location_key"))) {
          location.key = *key;
        }
      }
      if (const json* pin = JsonShape::member(item, "pin")) {
        const std::string pp = at(ip, "pin");
        if (*pin == "classic") {
          location.pin = Pin::Classic;
        }
        else if (*pin == "current-published") {
          location.pin = Pin::CurrentPublished;
        }
        else if (pin->is_object()) {
          shape_.object(*pin, pp, {}, {"revision", "definition_sha256"});
          location.pin = Pin::Exact;
          location.revision = static_cast<int>(
              shape_.integer_at(*pin, pp, "revision", 1, 2147483647)
                  .value_or(0));
          location.definition_sha256 =
              shape_
                  .text_at(*pin, pp, "definition_sha256", 64, 64, false,
                           patterns::sha256)
                  .value_or("");
        }
        else {
          shape_.fail(pp, "not one of the allowed values");
        }
      }
      out.roster.push_back(std::move(location));
    }
  }

  void start(const json& v, Scenario& out) {
    const std::string p = "/start";
    shape_.object(v, p, {"quickstart_size"},
                  {"location_key", "game_mode", "difficulty"});
    if (!v.is_object()) {
      return;
    }
    if (const json* k = JsonShape::member(v, "location_key")) {
      if (auto key = location_key(*k, at(p, "location_key"))) {
        out.start_key = *key;
      }
    }
    if (auto mode =
            shape_.choice_at(v, p, "game_mode", {"freemode", "quickstart"})) {
      out.start =
          *mode == 0 ? ScenarioStart::FreeMode : ScenarioStart::QuickStart;
    }
    if (auto size =
            shape_.choice_at(v, p, "quickstart_size", {"small", "big"})) {
      out.quickstart_big = *size == 1;
    }
    else if (out.start == ScenarioStart::QuickStart &&
             !JsonShape::member(v, "quickstart_size")) {
      shape_.fail(at(p, "quickstart_size"), "missing member 'quickstart_size'");
    }
    out.difficulty = static_cast<int>(
        shape_.integer_at(v, p, "difficulty", 1, 5).value_or(1));
  }

  void rules(const json& v, Scenario& out) {
    const std::string p = "/rules";
    shape_.object(v, p, {}, {"win", "travel", "game_rules"});
    if (!v.is_object()) {
      return;
    }
    if (const json* win = JsonShape::member(v, "win")) {
      const std::string wp = at(p, "win");
      const json* mode = JsonShape::member(*win, "mode");
      if (!win->is_object() || !mode) {
        shape_.fail(wp, "expected a win rule");
      }
      else if (*mode == "none") {
        shape_.object(*win, wp, {}, {"mode"});
        out.win_mode = WinMode::None;
      }
      else if (*mode == "market_share") {
        shape_.object(*win, wp, {}, {"mode", "scope", "percent"});
        out.win_mode = WinMode::MarketShare;
        out.win_percent = static_cast<int>(
            shape_.integer_at(*win, wp, "percent", 1, 100).value_or(0));
        if (const json* scope = JsonShape::member(*win, "scope")) {
          const std::string sp = at(wp, "scope");
          if (*scope == "any_location") {
            out.win_scope = WinScope::AnyLocation;
          }
          else if (*scope == "all_locations") {
            out.win_scope = WinScope::AllLocations;
          }
          else if (scope->is_object()) {
            shape_.object(*scope, sp, {}, {"locations"});
            out.win_scope = WinScope::Listed;
            if (const json* list = JsonShape::member(*scope, "locations")) {
              const std::string lp = at(sp, "locations");
              if (shape_.array(*list, lp, 1, 32)) {
                std::set<std::string> seen;
                for (std::size_t i = 0; i < list->size(); ++i) {
                  if (auto key = location_key((*list)[i], at(lp, i))) {
                    if (!seen.insert(key->str()).second) {
                      shape_.fail(at(lp, i), "duplicate item");
                    }
                    out.win_locations.push_back(*key);
                  }
                }
              }
            }
          }
          else {
            shape_.fail(sp, "not one of the allowed values");
          }
        }
      }
      else {
        shape_.fail(at(wp, "mode"), "not one of the allowed values");
      }
    }
    if (const json* travel = JsonShape::member(v, "travel")) {
      const std::string tp = at(p, "travel");
      shape_.object(*travel, tp,
                    {"dollars_per_unit_milli", "bonus_route_cost_dollars"},
                    {"model"});
      if (auto model = shape_.choice_at(*travel, tp, "model",
                                        {"disabled", "geo_manhattan"})) {
        out.travel_disabled = *model == 0;
      }
      out.travel_dollars_per_degree_milli =
          shape_.integer_at(*travel, tp, "dollars_per_unit_milli", 0, 100000);
      out.bonus_route_cost_dollars = shape_.integer_at(
          *travel, tp, "bonus_route_cost_dollars", 0, 1000000);
    }
    if (const json* g = JsonShape::member(v, "game_rules")) {
      const std::string gp = at(p, "game_rules");
      shape_.object(
          *g, gp,
          {"fix_legacy_bugs", "mafia_shakedowns_target_all_restaurants",
           "demand_fulfillment_popularity", "pizza_judging", "cook_book",
           "police", "suppliers_remember_missed_payments"},
          {});
      auto& r = out.game_rules;
      r.fix_legacy_bugs = shape_.boolean_at(*g, gp, "fix_legacy_bugs");
      r.mafia_shakedowns_target_all_restaurants =
          shape_.boolean_at(*g, gp, "mafia_shakedowns_target_all_restaurants");
      r.demand_fulfillment_popularity =
          shape_.boolean_at(*g, gp, "demand_fulfillment_popularity");
      if (auto judging = shape_.choice_at(*g, gp, "pizza_judging",
                                          {"original", "relaxed"})) {
        r.pizza_judging =
            *judging == 0 ? PizzaJudging::Original : PizzaJudging::Relaxed;
      }
      r.cook_book = shape_.boolean_at(*g, gp, "cook_book");
      r.police = shape_.boolean_at(*g, gp, "police");
      r.suppliers_remember_missed_payments =
          shape_.boolean_at(*g, gp, "suppliers_remember_missed_payments");
    }
  }

  void objective_list(const json& v, Ptr p, std::size_t min_items,
                      std::size_t max_items, std::vector<Objective>& out) {
    if (!shape_.array(v, p, min_items, max_items)) {
      return;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
      const json& item = v[i];
      const std::string ip = at(p, i);
      shape_.object(item, ip, {"hint", "within_game_days"},
                    {"objective_id", "milestone", "polarity", "label"});
      if (!item.is_object()) {
        continue;
      }
      Objective objective;
      objective.id =
          shape_.text_at(item, ip, "objective_id", 1, 32, false, objective_id)
              .value_or("");
      if (auto polarity =
              shape_.choice_at(item, ip, "polarity", {"achieve", "avoid"})) {
        objective.polarity =
            *polarity == 0 ? Polarity::Achieve : Polarity::Avoid;
      }
      if (const json* m = JsonShape::member(item, "milestone")) {
        const std::string mp = at(ip, "milestone");
        std::optional<MilestoneId> id;
        if (m->is_string()) {
          id = supported_milestone(m->get_ref<const std::string&>());
        }
        if (!id) {
          shape_.fail(mp, "not a supported milestone");
        }
        else if (avoidable(*id) != (objective.polarity == Polarity::Avoid)) {
          shape_.fail(mp, objective.polarity == Polarity::Avoid
                              ? "this milestone cannot be avoided"
                              : "this milestone can only be avoided");
        }
        else {
          objective.milestone = *id;
        }
      }
      objective.label = shape_.text_at(item, ip, "label", 1, 80).value_or("");
      shape_.text_at(item, ip, "hint", 0, 200);
      if (const json* days = JsonShape::member(item, "within_game_days");
          days && !days->is_null()) {
        objective.within_game_days = static_cast<int>(
            shape_.integer(*days, at(ip, "within_game_days"), 1, 36500)
                .value_or(1));
      }
      out.push_back(std::move(objective));
    }
  }

  void objectives(const json& v, Scenario& out) {
    const std::string p = "/objectives";
    shape_.object(v, p, {}, {"goal", "optional", "after_goal"});
    if (!v.is_object()) {
      return;
    }
    if (const json* goal = JsonShape::member(v, "goal")) {
      const std::string gp = at(p, "goal");
      shape_.object(*goal, gp, {}, {"all_of"});
      if (const json* all = JsonShape::member(*goal, "all_of")) {
        objective_list(*all, at(gp, "all_of"), 0, 4, out.goal);
      }
    }
    if (const json* optional = JsonShape::member(v, "optional")) {
      objective_list(*optional, at(p, "optional"), 0, 4,
                     out.optional_objectives);
    }
    if (auto after = shape_.choice_at(v, p, "after_goal",
                                      {"continue-sandbox", "end-game"})) {
      out.after_goal =
          *after == 0 ? AfterGoal::ContinueSandbox : AfterGoal::EndGame;
    }
  }

  void presentation(const json& v) {
    const std::string p = "/presentation";
    shape_.object(v, p,
                  {"suggested_event_mode", "decoration_events",
                   "default_costumes", "host_character"},
                  {});
    if (!v.is_object()) {
      return;
    }
    shape_.choice_at(v, p, "suggested_event_mode", {"off", "in-game-calendar"});
    if (const json* events = JsonShape::member(v, "decoration_events")) {
      shape_.string_set(*events, at(p, "decoration_events"), 8, 1, 96, false,
                        patterns::content_key);
    }
    if (const json* costumes = JsonShape::member(v, "default_costumes")) {
      const std::string cp = at(p, "default_costumes");
      if (shape_.array(*costumes, cp, 0, 16)) {
        for (std::size_t i = 0; i < costumes->size(); ++i) {
          const json& c = (*costumes)[i];
          const std::string ip = at(cp, i);
          shape_.object(c, ip, {}, {"role", "costume_id"});
          shape_.choice_at(
              c, ip, "role",
              {"staff:waiter", "staff:chef", "staff:manager", "customer"});
          shape_.text_at(c, ip, "costume_id", 1, 96, false, costume_id);
        }
      }
    }
    shape_.text_at(v, p, "host_character", 1, 96, false, costume_id);
  }

  JsonShape shape_;
};

void scenario_rules(const Scenario& s, Findings& out) {
  JsonShape report(out, "SCN-1");
  std::set<std::string> keys;
  for (std::size_t i = 0; i < s.roster.size(); ++i) {
    const auto& location = s.roster[i];
    const std::string where = "/roster/" + std::to_string(i);
    if (!keys.insert(location.key.str()).second) {
      report.fail(where, "the location " + location.key.str() +
                             " is in the roster twice");
    }
    const bool classic = location.key.in_namespace(classic_namespace);
    if (classic != (location.pin == Pin::Classic)) {
      report.fail(where + "/pin", classic
                                      ? "an original city is pinned \"classic\""
                                      : "only an original city is pinned "
                                        "\"classic\"");
    }
    if (classic && !classic_city_id(location.key)) {
      report.fail(where + "/location_key",
                  location.key.str() + " is not an original city");
    }
  }
  if (!keys.contains(s.start_key.str())) {
    out.push_back(Finding{.rule = "SCN-2",
                          .severity = Severity::Error,
                          .pointer = "/start/location_key",
                          .message = "the start is not in the roster"});
  }
  if (s.goal.empty() && s.after_goal != AfterGoal::ContinueSandbox) {
    out.push_back(Finding{.rule = "SCN-3",
                          .severity = Severity::Error,
                          .pointer = "/objectives/after_goal",
                          .message = "a preset without a goal is a sandbox: "
                                     "it can only continue"});
  }
  if (s.after_goal == AfterGoal::ContinueSandbox &&
      s.win_mode != WinMode::None) {
    out.push_back(Finding{
        .rule = "SCN-3",
        .severity = Severity::Error,
        .pointer = "/rules/win",
        .message = "continuing as a sandbox needs win mode none: a win rule "
                   "ends the game"});
  }
  if (s.roster.size() > 1) {
    const auto check = [&](const std::vector<Objective>& list,
                           std::string_view where) {
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (city_scoped(list[i].milestone)) {
          out.push_back(Finding{
              .rule = "SCN-4",
              .severity = Severity::Error,
              .pointer = std::string(where) + "/" + std::to_string(i),
              .message = std::string(milestone_name(list[i].milestone)) +
                         " is not tied to a city; it needs a single "
                         "location"});
        }
      }
    };
    check(s.goal, "/objectives/goal/all_of");
    check(s.optional_objectives, "/objectives/optional");
    if (s.travel_disabled) {
      out.push_back(Finding{.rule = "SCN-5",
                            .severity = Severity::Error,
                            .pointer = "/rules/travel",
                            .message = "travel is disabled with several "
                                       "locations"});
    }
  }
  for (const ContentKey& key : s.win_locations) {
    if (!keys.contains(key.str())) {
      out.push_back(Finding{.rule = "SCN-2",
                            .severity = Severity::Error,
                            .pointer = "/rules/win/scope/locations",
                            .message = key.str() + " is not in the roster"});
    }
  }
}

} // namespace

std::optional<core::events::MilestoneId>
supported_milestone(std::string_view name) {
  for (const auto& m : supported) {
    if (m.name == name) {
      return m.id;
    }
  }
  return std::nullopt;
}

std::string_view milestone_name(core::events::MilestoneId id) {
  for (const auto& m : supported) {
    if (m.id == id) {
      return m.name;
    }
  }
  return "unsupported milestone";
}

ScenarioReadResult read_scenario(const nlohmann::json& document) {
  ScenarioReadResult result;
  ScenarioParser parser(result.findings);
  auto scenario = parser.parse(document);
  if (!scenario) {
    return result;
  }
  scenario_rules(*scenario, result.findings);
  if (!has_errors(result.findings)) {
    result.scenario = std::move(scenario);
  }
  return result;
}

ScenarioReadResult read_scenario_text(std::string_view text) {
  nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
  if (document.is_discarded()) {
    ScenarioReadResult result;
    result.findings.push_back(Finding{.rule = "SCHEMA-1",
                                      .severity = Severity::Error,
                                      .pointer = "",
                                      .message = "not a JSON document"});
    return result;
  }
  return read_scenario(document);
}

ScenarioReadResult read_scenario_file(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_file_bytes) {
    ScenarioReadResult result;
    result.findings.push_back(
        Finding{.rule = "SCHEMA-1",
                .severity = Severity::Error,
                .pointer = "",
                .message = ec ? "cannot read " + path.string()
                              : path.string() + " is too large"});
    return result;
  }
  std::ifstream in(path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
  return read_scenario_text(text);
}

std::optional<Roster> scenario_roster(const Scenario& scenario,
                                      const DefinitionLookup& lookup,
                                      Findings& findings) {
  std::vector<RosterEntry> entries;
  bool ok = true;
  for (std::size_t i = 0; i < scenario.roster.size(); ++i) {
    const auto& location = scenario.roster[i];
    const std::string where = "/roster/" + std::to_string(i);
    if (location.pin == Pin::Classic) {
      const auto id = classic_city_id(location.key);
      if (!id) {
        ok = false;
        continue;
      }
      entries.push_back(RosterEntry{.key = location.key,
                                    .source = RosterSource::Classic,
                                    .classic_number = id->value,
                                    .definition = nullptr});
      continue;
    }
    auto definition = lookup ? lookup(location.key) : nullptr;
    if (!definition || definition->key != location.key) {
      findings.push_back(
          Finding{.rule = "SCN-6",
                  .severity = Severity::Error,
                  .pointer = where,
                  .message = "no valid definition of " + location.key.str()});
      ok = false;
      continue;
    }
    if (location.pin == Pin::Exact &&
        (definition->revision != location.revision ||
         definition->sha256 != location.definition_sha256)) {
      findings.push_back(Finding{.rule = "SCN-6",
                                 .severity = Severity::Error,
                                 .pointer = where + "/pin",
                                 .message = "the definition of " +
                                            location.key.str() +
                                            " is not the pinned revision"});
      ok = false;
      continue;
    }
    entries.push_back(RosterEntry{.key = location.key,
                                  .source = RosterSource::Definition,
                                  .classic_number = std::nullopt,
                                  .definition = std::move(definition)});
  }
  if (!ok) {
    return std::nullopt;
  }
  auto roster = Roster::from_entries(std::move(entries));
  if (!roster) {
    findings.push_back(Finding{.rule = "SCN-1",
                               .severity = Severity::Error,
                               .pointer = "/roster",
                               .message = "the roster breaks a roster rule"});
  }
  return roster;
}

bool configure_new_game(const std::shared_ptr<const Scenario>& scenario,
                        const Roster& roster, init::Config& config,
                        const GameRules& base_rules, Findings& findings) {
  if (!scenario) {
    return false;
  }
  if (roster.size() > static_cast<std::size_t>(NUMBER_OF_CITIES)) {
    findings.push_back(
        Finding{.rule = "SCN-7",
                .severity = Severity::Error,
                .pointer = "/roster",
                .message = std::to_string(roster.size()) +
                           " locations; this build simulates at most " +
                           std::to_string(NUMBER_OF_CITIES) + " in one game"});
    return false;
  }
  const auto start = roster.find(scenario->start_key);
  if (!start) {
    findings.push_back(Finding{.rule = "SCN-2",
                               .severity = Severity::Error,
                               .pointer = "/start/location_key",
                               .message = "the start is not in the roster"});
    return false;
  }
  config.roster = roster;
  config.city_id = *start;
  config.scenario = scenario;
  config.mode = scenario->start == ScenarioStart::QuickStart
                    ? GameMode::QUICKSTART
                    : GameMode::FREEMODE;
  config.size = scenario->start == ScenarioStart::QuickStart
                    ? (scenario->quickstart_big ? init::RestaurantSize::BIG
                                                : init::RestaurantSize::SMALL)
                    : init::RestaurantSize::NONE;
  config.difficulty = static_cast<init::DifficultyLevel>(scenario->difficulty);
  config.mission.reset();

  GameRules rules = base_rules;
  const auto& r = scenario->game_rules;
  rules.fix_legacy_bugs = r.fix_legacy_bugs.value_or(rules.fix_legacy_bugs);
  rules.mafia_shakedowns_target_all_restaurants =
      r.mafia_shakedowns_target_all_restaurants.value_or(
          rules.mafia_shakedowns_target_all_restaurants);
  rules.demand_fulfillment_popularity =
      r.demand_fulfillment_popularity.value_or(
          rules.demand_fulfillment_popularity);
  rules.pizza_judging = r.pizza_judging.value_or(rules.pizza_judging);
  rules.cook_book = r.cook_book.value_or(rules.cook_book);
  rules.police = r.police.value_or(rules.police);
  rules.suppliers_remember_missed_payments =
      r.suppliers_remember_missed_payments.value_or(
          rules.suppliers_remember_missed_payments);
  config.rules = rules;
  return true;
}

} // namespace core::content
