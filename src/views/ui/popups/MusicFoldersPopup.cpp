/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/ui/popups/MusicFoldersPopup.h"

#include <algorithm>

#include "engine/FormatCompat.h"
#include "views/ui/PopupManager.h"
#include "views/ui/popups/TextInputPopup.h"

namespace {

constexpr int pad = BasePopup::popup_border + 2;
constexpr int row_h = 22;
constexpr int first_row_y = 32;
constexpr int row_step = 27;
constexpr int message_y = 196;
constexpr int bottom_y = 214;
constexpr uint8_t text_color = 10;
constexpr int label_w = 150;
constexpr int state_x = pad + label_w + 4;
constexpr int buttons_x = 262;
constexpr auto refresh_every = std::chrono::milliseconds(250);
constexpr int max_label_chars = 40;

int row_y(int slot) { return first_row_y + (slot * row_step); }

std::string shown_name(const music::MusicSystem::FolderRow& r) {
  if (!r.folder.label.empty()) {
    return r.folder.label;
  }
  const std::string own = music::utf8_path(r.resolved.filename());
  return own.empty() ? r.folder.path : own;
}

} // namespace

MusicFoldersPopup::RowButtons
MusicFoldersPopup::make_row(MusicFoldersPopup& self, int slot) {
  const int y = row_y(slot);
  const auto at = static_cast<std::size_t>(slot);
  MusicFoldersPopup* me = &self;
  // Each button acts on the folder shown in its row when it is clicked.
  auto on_row = [me, at](auto fn) {
    return [me, at, fn] {
      if (const auto* r = me->row(at)) {
        fn(*r);
        me->refresh();
      }
    };
  };
  RowButtons b{
      .enabled = PopupButton(
          {buttons_x, y, 44, row_h}, "On", on_row([me](const auto& r) {
            me->music_->folders().set_enabled(r.folder.id, !r.folder.enabled);
          })),
      .subfolders = PopupButton({buttons_x + 48, y, 66, row_h}, "Sub: On",
                                on_row([me](const auto& r) {
                                  me->music_->folders().set_include_subfolders(
                                      r.folder.id,
                                      !r.folder.include_subfolders);
                                })),
      .rename = PopupButton({buttons_x + 118, y, 60, row_h}, "Rename",
                            [me, at] { me->rename(at); }),
      .locate = PopupButton({buttons_x + 182, y, 54, row_h}, "Locate",
                            on_row([me](const auto& r) {
                              me->music_->browse_folder(r.folder.id);
                              me->message_ =
                                  "Choose where " + shown_name(r) + " is now";
                            })),
      .remove = PopupButton(
          {buttons_x + 240, y, 46, row_h}, "Del", on_row([me](const auto& r) {
            me->message_ = "Removed " + shown_name(r) + "; Save keeps it so";
            me->music_->folders().remove(r.folder.id);
          })),
  };
  const std::string n = std::to_string(slot);
  b.enabled.id("music_folder_on_" + n);
  b.subfolders.id("music_folder_sub_" + n);
  b.rename.id("music_folder_rename_" + n);
  b.locate.id("music_folder_locate_" + n);
  b.remove.id("music_folder_remove_" + n);
  b.enabled.tooltip("Play this folder's music (On) or leave it out (Off)");
  b.subfolders.tooltip("Look in the folders inside it too, or only in this "
                       "one");
  b.rename.tooltip("The name shown in the game; the folder keeps its own. "
                   "Empty: its own name.");
  b.locate.tooltip("The folder has moved: point to its new place. Its "
                   "collections and playlists stay.");
  b.remove.tooltip("Take the folder off the list; the files stay where "
                   "they are");
  return b;
}

MusicFoldersPopup::MusicFoldersPopup(const GameContext& ctx,
                                     std::function<void()> on_close)
    : BasePopup(ctx), music_(ctx.music), popups_(ctx.popup),
      on_close_(std::move(on_close)),
      row_buttons_{make_row(*this, 0), make_row(*this, 1), make_row(*this, 2),
                   make_row(*this, 3), make_row(*this, 4), make_row(*this, 5)},
      add_({pad, bottom_y, 100, row_h}, "Add Folder...",
           [this] {
             music_->browse_folder();
             message_ = "Choose a folder with music in it";
             refresh();
           }),
      rescan_({pad + 104, bottom_y, 64, row_h}, "Rescan",
              [this] {
                music_->library().rescan();
                message_ = "Looking at the saved folders again";
                refresh();
              }),
      reset_({pad + 172, bottom_y, 60, row_h}, "Reset",
             [this] {
               music_->folders().reset();
               page_ = 0;
               message_ = "The suggested folders; Save keeps them";
               refresh();
             }),
      page_prev_({pad + 240, bottom_y, 26, row_h}, "<",
                 [this] {
                   page_ = page_ > 0 ? page_ - 1 : pages() - 1;
                   refresh();
                 }),
      page_next_({pad + 270, bottom_y, 26, row_h}, ">",
                 [this] {
                   page_ = (page_ + 1) % pages();
                   refresh();
                 }),
      cancel_({width - pad - 144, bottom_y, 70, row_h}, "Cancel",
              [this] { close(false); }),
      save_({width - pad - 70, bottom_y, 70, row_h}, "Save",
            [this] { close(true); }) {
  add_.id("music_folders_add");
  rescan_.id("music_folders_rescan");
  reset_.id("music_folders_reset");
  page_prev_.id("music_folders_page_prev");
  page_next_.id("music_folders_page_next");
  cancel_.id("music_folders_cancel");
  save_.id("music_folders_save");
  add_.tooltip("Add a folder of any name from anywhere on this computer");
  rescan_.tooltip("Look for new, changed or removed files in the saved "
                  "folders");
  reset_.tooltip("Go back to the suggested folders: your Music folder and "
                 "the game's Xmas folder");
  page_prev_.tooltip("Previous page of folders");
  page_next_.tooltip("Next page of folders");
  cancel_.tooltip("Close and drop the changes (Esc)");
  save_.tooltip("Keep the changes and look for music in them (Enter)");

  add_shortcut(input::Action::Exit, &cancel_);
  add_shortcut(input::Action::SubmitInput, &save_);
  add_shortcut(input::Action::PageUp, &page_prev_);
  add_shortcut(input::Action::PageDown, &page_next_);

  seen_folder_message_ = music_->folder_message();
  refresh();
}

std::size_t MusicFoldersPopup::pages() const {
  return std::max<std::size_t>(1, (rows_.size() + rows_per_page - 1) /
                                      rows_per_page);
}

const music::MusicSystem::FolderRow*
MusicFoldersPopup::row(std::size_t slot) const {
  const std::size_t i = (page_ * rows_per_page) + slot;
  return i < rows_.size() ? &rows_[i] : nullptr;
}

void MusicFoldersPopup::refresh() {
  rows_ = music_->folder_rows();
  page_ = std::min(page_, pages() - 1);
  if (music_->folder_message() != seen_folder_message_) {
    seen_folder_message_ = music_->folder_message();
    message_ = seen_folder_message_;
  }
  for (std::size_t slot = 0; slot < rows_per_page; ++slot) {
    if (const auto* r = row(slot)) {
      RowButtons& b = row_buttons_[slot];
      b.enabled.text(r->folder.enabled ? "On" : "Off");
      b.subfolders.text(r->folder.include_subfolders ? "Sub: On" : "Sub: Off");
    }
  }
  next_refresh_ = std::chrono::steady_clock::now() + refresh_every;
  mark_dirty();
}

void MusicFoldersPopup::tick() {
  if (std::chrono::steady_clock::now() >= next_refresh_) {
    refresh();
  }
}

void MusicFoldersPopup::close(bool save) {
  if (save) {
    if (const music::SaveResult saved = music_->save_folders(); !saved) {
      message_ = "Not saved: " + saved.error;
      refresh();
      return;
    }
  }
  else {
    music_->folders().cancel();
  }
  if (on_close_) {
    on_close_();
  }
  finished();
}

void MusicFoldersPopup::rename(std::size_t slot) {
  const auto* r = row(slot);
  if (!r || !popups_) {
    return;
  }
  // The draft stays in the music system, so this screen opens again as it
  // was, with the new name.
  music::MusicSystem* music = music_;
  PopupManager* popups = popups_;
  std::function<void()> on_close = on_close_;
  const std::string id = r->folder.id;
  popups_->create<TextInputPopup>(
      "Name for " + shown_name(*r) + " (empty: the folder's own name)",
      max_label_chars,
      [music, popups, on_close, id](std::string label) {
        music->folders().relabel(id, std::move(label));
        popups->create<MusicFoldersPopup>(on_close);
      },
      r->folder.label);
  finished();
}

void MusicFoldersPopup::render(Surface& surface, ViewPoint origin) {
  auto s = popup_surface(width, height);
  const int inner_w = width - (2 * pad);

  font_->render_line_onto(s, "Music Folders", {pad, pad + 1},
                          FontFace::ASTANDAR, text_color);
  std::string head =
      compat::format("{} folder{}", rows_.size(), rows_.size() == 1 ? "" : "s");
  if (pages() > 1) {
    head += compat::format(", page {} of {}", page_ + 1, pages());
  }
  if (music_->folders().dirty()) {
    head += "  -  unsaved changes";
  }
  font_->render_line_onto(s, head, {width - pad, pad + 2}, FontFace::PIZ_NUMB,
                          text_color, TextAnchor::End);

  if (rows_.empty()) {
    font_->render_line_onto(s,
                            "No folders: Add Folder... chooses one, Reset "
                            "brings back the suggested ones",
                            {pad, row_y(0) + 6}, FontFace::PIZ_NUMB,
                            text_color);
  }
  for (std::size_t slot = 0; slot < rows_per_page; ++slot) {
    const auto* r = row(slot);
    if (!r) {
      break;
    }
    const int y = row_y(static_cast<int>(slot));
    font_->render_line_onto(
        s,
        font_->truncate_to_width(shown_name(*r), FontFace::ASTANDAR, label_w),
        {pad, y + 5}, FontFace::ASTANDAR, text_color);
    std::string state;
    if (!r->available) {
      state = "missing";
    }
    else if (!music_->folders().saved(r->folder.id)) {
      state = "new";
    }
    else {
      state =
          compat::format("{} track{}", r->tracks, r->tracks == 1 ? "" : "s");
    }
    font_->render_line_onto(s, state, {state_x, y + 6}, FontFace::PIZ_NUMB,
                            text_color);
    RowButtons& b = row_buttons_[slot];
    b.enabled.render_with(*this, s);
    b.subfolders.render_with(*this, s);
    b.rename.render_with(*this, s);
    b.locate.render_with(*this, s);
    b.remove.render_with(*this, s);
  }

  font_->render_line_onto(
      s, font_->truncate_to_width(message_, FontFace::PIZ_NUMB, inner_w),
      {pad, message_y}, FontFace::PIZ_NUMB, text_color);

  add_.render_with(*this, s);
  rescan_.render_with(*this, s);
  reset_.render_with(*this, s);
  if (pages() > 1) {
    page_prev_.render_with(*this, s);
    page_next_.render_with(*this, s);
  }
  cancel_.render_with(*this, s);
  save_.render_with(*this, s);

  surface.blit_raw(s, origin);
}

void MusicFoldersPopup::add_interactions(views::ui::InteractionContext ictx,
                                         ViewPoint origin) {
  for (std::size_t slot = 0; slot < rows_per_page; ++slot) {
    const auto* r = row(slot);
    if (!r) {
      break;
    }
    RowButtons& b = row_buttons_[slot];
    b.enabled.add_interactions(ictx, origin);
    b.subfolders.add_interactions(ictx, origin);
    b.rename.add_interactions(ictx, origin);
    b.locate.add_interactions(ictx, origin);
    b.remove.add_interactions(ictx, origin);

    // The path itself, on the name and the count.
    const std::string where = r->available ? music::utf8_path(r->resolved)
                                           : music::utf8_path(r->resolved) +
                                                 " is not there: Locate "
                                                 "points to its new place";
    ictx.layers.create_frame_item(
        add_origin(ViewRect{pad, row_y(static_cast<int>(slot)),
                            buttons_x - pad - 4, row_h},
                   origin),
        "music_folder_name_" + std::to_string(slot),
        [](const views::ui::InteractionEvent&) {},
        PopupButton::tooltip_options(where));
  }
  add_.add_interactions(ictx, origin);
  rescan_.add_interactions(ictx, origin);
  reset_.add_interactions(ictx, origin);
  if (pages() > 1) {
    page_prev_.add_interactions(ictx, origin);
    page_next_.add_interactions(ictx, origin);
  }
  cancel_.add_interactions(ictx, origin);
  save_.add_interactions(ictx, origin);
}
