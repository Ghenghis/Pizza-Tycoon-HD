/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/screens/menus/MenuRecovery.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "core/state/GameState.h"
#include "core/state/UIState.h"
#include "engine/Logger.h"
#include "resources/FontManager.h"
#include "views/screens/menus/MenuTheme.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/AlertPopup.h"
#include "views/ui/popups/ConfirmPopup.h"
#include "views/ui/widgets/PositionMarkerMenuItem.h"
#include "views/ui/widgets/TextMenuItem.h"

using namespace views::ui::menu;

namespace {

constexpr FontFace dense_face = FontFace::CSTANDAR;
constexpr uint8_t plain_color = 240;
constexpr uint8_t chosen_color = 29;
constexpr uint8_t focus_color = 1;
constexpr uint8_t damaged_color = 246;
constexpr uint8_t disabled_color = 246;

// Where things go, from the menu's origin.
constexpr ViewPoint origin{20, 56};
constexpr int list_width = 600;
constexpr int list_height = 190; // the rows of one page
constexpr int detail_lines = 3;

// One line of the current page. The row it shows changes with the page; an
// empty line is skipped by the keyboard.
class RecoveryRowItem : public MenuItem {
public:
  RecoveryRowItem(GameContext& gc, std::shared_ptr<save::RecoveryBrowser> b,
                  std::size_t index, int height)
      : MenuItem(gc, dense_face), browser_(std::move(b)), index_(index),
        line_height_(height) {
    help_text("A save or one of its versions. Enter or click chooses it; "
              "Left and Right turn the page. Then Load, Restore or Undo "
              "below.");
  }

  void draw(Surface& screen, ViewPoint p, bool is_selected) override {
    const save::RecoveryRow* row = browser_->row_on_page(index_);
    if (row == nullptr) {
      return;
    }
    uint8_t color = row->damaged ? damaged_color : plain_color;
    if (browser_->is_chosen(index_)) {
      color = chosen_color;
    }
    if (is_selected) {
      color = focus_color;
    }
    const std::string text =
        (browser_->is_chosen(index_) ? "> " : "  ") + row->text;
    context_.font->render_text_onto(
        screen, text, p, font_face_,
        TextStyle{.color = color, .align = TextAnchor::Start});
    x_pos_ = p.x;
  }
  void handle_action() override { browser_->choose(index_); }
  Size measure() const override { return {list_width, line_height_}; }
  bool focusable() const override {
    return browser_->row_on_page(index_) != nullptr;
  }
  bool interactive() const override { return focusable(); }
  bool on_adjust(AdjustAxis axis, int delta) override {
    if (axis != AdjustAxis::Horizontal) {
      return false;
    }
    if (delta > 0) {
      browser_->next_page();
    }
    else {
      browser_->previous_page();
    }
    return true;
  }

private:
  std::shared_ptr<save::RecoveryBrowser> browser_;
  std::size_t index_;
  int line_height_;
};

// Text that changes as the player browses, wrapped to a box.
class LiveTextItem : public MenuItem {
public:
  LiveTextItem(GameContext& gc, std::function<std::string()> text, Size size)
      : MenuItem(gc, dense_face), text_of_(std::move(text)), size_(size) {}

  void draw(Surface& screen, ViewPoint p, bool /*is_selected*/) override {
    context_.font->render_into_rect(screen, text_of_(),
                                    {p.x, p.y, size_.w, 0}, font_face_,
                                    TextStyle{.color = plain_color});
  }
  void handle_action() override {}
  Size measure() const override { return size_; }
  bool focusable() const override { return false; }
  bool interactive() const override { return false; }

private:
  std::function<std::string()> text_of_;
  Size size_;
};

// A button that is gray, and only explains itself, while it can't act.
class ActionItem : public MenuItem {
public:
  ActionItem(GameContext& gc, std::string label, std::string help,
             std::function<bool()> enabled, std::function<void()> action)
      : MenuItem(gc, dense_face, std::move(label)),
        enabled_(std::move(enabled)), action_(std::move(action)) {
    help_text(std::move(help));
  }

  void draw(Surface& screen, ViewPoint p, bool is_selected) override {
    const uint8_t color = !enabled_()   ? disabled_color
                          : is_selected ? chosen_color
                                        : plain_color;
    const RenderedTextMetrics m = context_.font->render_text_onto(
        screen, text_, p, font_face_,
        TextStyle{.color = color, .align = TextAnchor::Start});
    width_ = m.width;
    height_ = m.height;
    x_pos_ = m.render_x;
  }
  void handle_action() override {
    if (enabled_()) {
      action_();
    }
  }
  Size measure() const override {
    const RenderedTextMetrics m = context_.font->size_text(text_, font_face_);
    return {m.width, m.height};
  }

private:
  std::function<bool()> enabled_;
  std::function<void()> action_;
};

int dense_line_height(GameContext& context) {
  return context.font->size_text("Ag", dense_face).height + 2;
}

} // namespace

MenuRecovery::MenuRecovery(GameContext& context, GameState& state)
    : Menu(context, state, origin, 0), row_height_(dense_line_height(context)),
      browser_(std::make_shared<save::RecoveryBrowser>(static_cast<std::size_t>(
          std::max(4, list_height / std::max(1, row_height_))))),
      busy_cursor_(context) {
  const std::size_t rows = browser_->rows_per_page();
  for (std::size_t i = 0; i < rows; ++i) {
    add_menu_item(std::make_unique<PositionMarkerMenuItem>(
        context, 0, static_cast<int>(i) * row_height_));
    add_menu_item(std::make_unique<RecoveryRowItem>(context, browser_, i,
                                                    row_height_));
  }

  int y = static_cast<int>(rows) * row_height_ + 4;
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 0, y));
  add_menu_item(std::make_unique<LiveTextItem>(
      context, [b = browser_] { return b->summary(); },
      Size{list_width, row_height_}));

  y += row_height_ + 2;
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 0, y));
  add_menu_item(std::make_unique<LiveTextItem>(
      context,
      [b = browser_]() -> std::string {
        const save::RecoveryRow* row = b->chosen();
        return row ? row->detail
                   : "Choose a save or a version to see what it is.";
      },
      Size{list_width, row_height_ * detail_lines}));

  using save::RecoveryAction;
  y += row_height_ * detail_lines + 6;
  const auto action = [&](int x, std::string label, std::string help,
                          RecoveryAction what) {
    add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, x, y));
    add_menu_item(std::make_unique<ActionItem>(
        context, std::move(label), std::move(help),
        [b = browser_, what] { return b->can(what); },
        [this, what] { ask(what); }));
  };
  action(0, "Load this version",
         "Loads the chosen version, or the chosen save if it keeps none. The "
         "game in progress is kept as a checkpoint first. It asks before.",
         RecoveryAction::Load);
  action(170, "Restore as a new save",
         "Copies the chosen working version into a new save of its own, "
         "after checking it again. The save it came from stays as it is.",
         RecoveryAction::Branch);
  action(380, "Undo last restore",
         "Makes the chosen save load the version it loaded before its last "
         "restore. No version is deleted. It asks before.",
         RecoveryAction::Undo);

  y += row_height_ + 6;
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 0, y));
  add_menu_item(std::make_unique<ActionItem>(
      context, "< Previous page", "Shows the page before this one.",
      [b = browser_] { return b->page() > 0; },
      [this] {
        browser_->previous_page();
        mark_dirty();
      }));
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 170, y));
  add_menu_item(std::make_unique<ActionItem>(
      context, "Next page >", "Shows the page after this one.",
      [b = browser_] { return b->page() + 1 < b->page_count(); },
      [this] {
        browser_->next_page();
        mark_dirty();
      }));
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 380, y));
  add_menu_item(std::make_unique<ActionItem>(
      context, "Read the folder again",
      "Lists the saves again, as they are on the disk now.",
      [] { return true; },
      [this] {
        browser_->refresh();
        mark_dirty();
      }));

  // Last, so the help text goes below it.
  add_menu_item(std::make_unique<PositionMarkerMenuItem>(context, 520, y + 30));
  auto& back = add_menu_item(std::make_unique<TextMenuItem>(
      context, "Back", [this] { state_.ui().pop(); }, FontFace::RUBY, style,
      sel_style));
  back.help_text("Goes back to the menu before, changing nothing.");
}

void MenuRecovery::ask(save::RecoveryAction action) {
  context_.popup->origin(ViewPoint{26, 40});
  context_.popup->create<ConfirmPopup>(browser_->confirmation(action),
                                       [this, action](bool agree) {
                                         if (agree) {
                                           run(action);
                                         }
                                       });
}

void MenuRecovery::run(save::RecoveryAction action) {
  busy_cursor_.run(
      [this, action] {
        const save::RecoveryBrowser::Outcome outcome =
            browser_->perform(action, state_, context_);
        Logger::info("Recovery Center: {}",
                     outcome.message.empty() ? "done" : outcome.message);
        if (!outcome.message.empty()) {
          context_.popup->create<AlertPopup>(outcome.message);
        }
      },
      action == save::RecoveryAction::Load ? "Loading..." : "Working...");
  mark_dirty();
}

void MenuRecovery::tick() {
  Menu::tick();
  busy_cursor_.tick();
}

void MenuRecovery::render(const RenderContext& ctx) {
  Menu::render(ctx);
  busy_cursor_.render_onto(*ctx.surface);
}

Surface MenuRecovery::background() const {
  return default_background("Recovery Center");
}
