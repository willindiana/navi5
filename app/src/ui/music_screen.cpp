/* Navi5 — glass music home/library.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/music_screen.h"
#include "ui/player_state.h"
#include <cmath>
#include <limits>
#include "app/i18n.h"
#include "gfx/art.h"
#include "nuvio_input.h"
#include <algorithm>
#include <cstdio>

namespace ui {
namespace {
// Shared six-column browse geometry; Home, library grids and artist albums align.
constexpr int kGridColumns = 6;
constexpr float kGridCell = 286, kGridCard = 278, kGridTitle = 254;
constexpr float kArtistAlbumRow = 304, kArtistAlbumArt = kArtistAlbumRow - 70;
const char *heading(music::View view)
{
    using music::View;
    switch (view) {
    case View::Home: return T("Hjem");
    case View::Artists: return T("Artister");
    case View::Albums: case View::ArtistAlbums: return T("Album");
    case View::Playlists: return T("Spillelister");
    default: return T("Spor");
    }
}
}
namespace {
const std::vector<std::string> &search_keys(bool symbols)
{
    static const std::vector<std::string> keys{
        "a","b","c","d","e","f","g", "h","i","j","k","l","m","n",
        "o","p","q","r","s","t","u", "v","w","x","y","z","0","1",
        "2","3","4","5","6","7","8", "9"," ","⌫","×","#+="};
    static const std::vector<std::string> special{
        "-","'","æ","ø","å","é","ü", "ö","&","."," ","⌫","×","ABC"};
    return symbols ? special : keys;
}
}
bool MusicScreen::search_backspace(bool fullscreen)
{
    if (fullscreen || state_.request().view != music::View::Search) return false;
    auto value = state_.query();
    if (!value.empty()) {
        size_t last = value.size() - 1;
        while (last && (static_cast<unsigned char>(value[last]) & 0xc0) == 0x80) --last;
        value.resize(last);
    }
    state_.query(value, search_now_);
    return true;
}
Action MusicScreen::search_input(uint32_t pressed)
{
    const auto &keys = search_keys(search_symbols_);
    if (pressed & NUVIO_BTN_CIRCLE) {
        if (search_symbols_) { search_symbols_ = false; search_key_ = int(search_keys(false).size()) - 1; }
        else if (search_results_) search_results_ = false;
        else { state_.back(); tabs_focused_ = true; search_button_ = false; }
        return {};
    }
    if (search_results_) {
        if (pressed & NUVIO_BTN_LEFT) search_results_ = false;
        else if (pressed & NUVIO_BTN_UP) { if (!state_.move(-1)) tabs_focused_ = search_button_ = true; }
        else if (pressed & NUVIO_BTN_DOWN) { if (!state_.move(1)) m_bump = true; }
        else if (pressed & NUVIO_BTN_CROSS) {
            if (state_.status() == music::Status::Error) state_.retry();
            else { state_.open(); album_play_ = true; }
        }
        return {};
    }
    if (pressed & NUVIO_BTN_UP) { if (search_key_ < 7) tabs_focused_ = search_button_ = true; else search_key_ -= 7; }
    else if (pressed & NUVIO_BTN_DOWN) {
        if (size_t(search_key_ / 7 + 1) * 7 < keys.size()) search_key_ = std::min(search_key_ + 7, int(keys.size()) - 1);
        else m_bump = true;
    }
    else if (pressed & NUVIO_BTN_LEFT) { if (search_key_ % 7) --search_key_; else m_bump = true; }
    else if (pressed & NUVIO_BTN_RIGHT) {
        if (search_key_ % 7 < 6 && size_t(search_key_ + 1) < keys.size()) ++search_key_;
        else if (!state_.page().items.empty() || state_.status() == music::Status::Error) search_results_ = true;
        else m_bump = true;
    }
    else if (pressed & NUVIO_BTN_CROSS) {
        auto value = state_.query();
        if (size_t(search_key_) == keys.size() - 1) {
            search_symbols_ = !search_symbols_;
            search_key_ = search_symbols_ ? 0 : int(search_keys(false).size()) - 1;
            return {};
        }
        if (size_t(search_key_) == keys.size() - 2) value.clear();
        else if (size_t(search_key_) == keys.size() - 3) { search_backspace(); return {}; }
        else value += keys[search_key_];
        if (!state_.query(value, search_now_)) m_bump = true;
    }
    return {};
}
void MusicScreen::draw_search(float dt)
{
    gfx::text(112, 246, T("Søk"), {gfx::Bold, 36}, kText);
    glass_panel({100, 278, 620, 74}, 20);
    gfx::text(124, 326, state_.query().empty() ? T("Skriv for å søke") : state_.query(), {gfx::Medium, 28, 572}, kText);
    const auto &keys = search_keys(search_symbols_);
    for (size_t i = 0; i < keys.size(); ++i) {
        const float x = 100 + (i % 7) * 88, y = 380 + (i / 7) * 68;
        if (m_focused && !tabs_focused_ && !search_results_ && size_t(search_key_) == i) {
            row_drop_.to({x, y, 80, 60}, int(i)); row_drop_.draw(dt, 1, &animating_, 15);
        }
        const auto label = i == keys.size() - 4 ? T("Mellomrom") : i == keys.size() - 3 ? T("Tilbaketast") :
            i == keys.size() - 2 ? T("Tøm") : i == keys.size() - 1 && !search_symbols_ ? T("Symboler") : keys[i];
        gfx::text(x + 40, y + 40, label, {gfx::SemiBold, i >= keys.size() - 4 ? 13.f : 27.f, 76}, kText, 1);
    }
    const auto &page = state_.page();
    const auto status = state_.status();
    if (status != music::Status::Ready || (page.items.empty() && !state_.query().empty())) {
        gfx::text(810, 345, status == music::Status::Error ? T("Kunne ikke hente musikken") :
            status == music::Status::Loading ? T("Søker …") : state_.query().empty() ? T("Skriv for å søke") : T("Ingen treff"), {gfx::Medium, 28, 970}, kText2);
        if (status == music::Status::Error) {
            if (m_focused && !tabs_focused_ && search_results_) {
                row_drop_.to({800, 302, 988, 110}, 49); row_drop_.draw(dt, 1, &animating_, 16);
            }
            draw_pad_hints(810, 392, {{PadButton::Cross, T("Prøv igjen")}}, 0, 28);
        }
    }
    float selected_y = 0, total = 0;
    music::Kind previous = music::Kind::Artist;
    for (size_t i = 0; i < page.items.size(); ++i) {
        if (i == 0 || page.items[i].kind != previous) { total += 48; previous = page.items[i].kind; }
        if (i == state_.selected()) selected_y = total;
        total += 76;
    }
    // Compact playback occupies y=950..1050 even while paused; keep hints above it.
    const float results_height = live_ ? 610.f : 698.f;
    scroll_.to(std::min(std::max(0.f, selected_y - (results_height - 76) / 2), std::max(0.f, total - results_height)));
    if (scroll_.step(dt)) animating_ = true;
    gfx::push_scissor({790, 278, 1010, results_height});
    float y = 278 - scroll_.value;
    previous = music::Kind::Artist;
    for (size_t i = 0; i < page.items.size(); ++i) {
        const auto &item = page.items[i];
        if (i == 0 || item.kind != previous) {
            gfx::text(810, y + 32, item.kind == music::Kind::Track ? T("Spor") : item.kind == music::Kind::Artist ? T("Artister") : item.kind == music::Kind::Album ? T("Album") : T("Spillelister"), {gfx::Bold, 26}, kText);
            y += 48; previous = item.kind;
        }
        if (m_focused && !tabs_focused_ && search_results_ && i == state_.selected()) {
            row_drop_.to({800, y, 988, 72}, int(i) + 49); row_drop_.draw(dt, 1, &animating_, 16);
        }
        art::draw({812, y + 8, 56, 56}, item.artwork_url, "", 720, 720, 8);
        gfx::text(886, y + 30, item.title, {gfx::SemiBold, 25, 870}, kText);
        gfx::text(886, y + 58, item.subtitle, {gfx::Medium, 20, 870}, kText2);
        y += 76;
    }
    gfx::pop_scissor();
    draw_pad_hints(100, live_ ? 922 : 1010, {{PadButton::Cross, T("Velg")}, {PadButton::Circle, T("Tilbake")}});
}
void MusicScreen::select_home()
{
    const auto &sections = state_.page().sections;
    if (home_row_ < 0 || size_t(home_row_) >= sections.size()) return;
    const auto &section = sections[home_row_];
    if (section.count) {
        home_column_ = std::min(home_column_, section.count - 1);
        state_.move(int(section.begin + home_column_) - int(state_.selected()));
    }
}
Action MusicScreen::input(uint32_t pressed)
{
    if ((pressed & NUVIO_BTN_SQUARE) && search_backspace()) return {};
    if (!m_focused) return {};
    if (pressed & (NUVIO_BTN_L1 | NUVIO_BTN_R1)) {
        const int current = search_button_ || state_.request().view == music::View::Search ? 4 : state_.tab_index();
        const int next = (current + ((pressed & NUVIO_BTN_R1) ? 1 : 4)) % 5;
        if (next == 4) state_.search();
        else state_.tab(next);
        tabs_focused_ = true;
        search_button_ = next == 4;
        artist_action_ = 0;
        album_play_ = true; album_shuffle_ = false;
        return {};
    }
    if (!tabs_focused_ && state_.request().view == music::View::Search) return search_input(pressed);
    if (tabs_focused_ && search_button_) {
        if (pressed & NUVIO_BTN_LEFT) { state_.tab(3); search_button_ = false; }
        else if (pressed & (NUVIO_BTN_CROSS | NUVIO_BTN_DOWN)) {
            state_.search(); tabs_focused_ = false; search_results_ = false;
        } else if (pressed & NUVIO_BTN_CIRCLE) {
            if (state_.request().view == music::View::Search) state_.back();
            search_button_ = false;
        } // Up at header stays on Search.
        return {};
    }
    if (state_.continuation_error() && (pressed & NUVIO_BTN_CROSS)) { state_.retry(); return {}; }
    const auto view = state_.request().view;
    const bool gallery = view == music::View::Home || view == music::View::Albums || view == music::View::ArtistAlbums || view == music::View::Artists || view == music::View::Playlists;
    const bool album = view == music::View::AlbumTracks || view == music::View::PlaylistTracks;
    const int columns = kGridColumns;
    const bool home = state_.request().view == music::View::Home;
    if (home && !state_.page().sections.empty()) {
        if (tabs_focused_ && (pressed & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS))) {
            tabs_focused_ = false; home_row_ = live_ ? -1 : 0; select_home(); return {};
        }
        if (!tabs_focused_ && (pressed & (NUVIO_BTN_UP | NUVIO_BTN_DOWN | NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT | NUVIO_BTN_CROSS))) {
            if (pressed & NUVIO_BTN_UP) {
                if (home_row_ > (live_ ? -1 : 0)) --home_row_; else tabs_focused_ = true;
            } else if (pressed & NUVIO_BTN_DOWN) {
                if (home_row_ < 1) ++home_row_; else m_bump = true;
            } else if (pressed & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
                if (home_row_ >= 0) {
                    const auto count = state_.page().sections[home_row_].count;
                    if ((pressed & NUVIO_BTN_LEFT) && home_column_) --home_column_;
                    else if ((pressed & NUVIO_BTN_RIGHT) && home_column_ + 1 < count) ++home_column_;
                    else m_bump = true;
                }
            } else if (home_row_ < 0) open_player_ = live_;
            else {
                const auto &section = state_.page().sections[home_row_];
                if (!section.ok) state_.retry();
                else if (section.count) { select_home(); state_.open(); album_play_ = true; album_shuffle_ = false; }
            }
            select_home();
            return {};
        }
    }
    if (view == music::View::ArtistAlbums && artist_context_ != state_.context()) {
        artist_context_ = state_.context();
        if (artist_action_ >= 2) artist_action_ = 0;
        artist_song_ = 0; artist_expanded_ = false;
    }
    if (!tabs_focused_ && view == music::View::ArtistAlbums &&
        state_.status() == music::Status::Ready) {
        const auto count = std::min(state_.page().top_songs.size(), artist_expanded_ ? size_t(20) : size_t(5));
        const bool more = !artist_expanded_ && state_.page().top_songs.size() > 5;
        artist_song_ = count ? std::min(artist_song_, count - 1) : 0;
        if (artist_action_ == 3 && !more) artist_action_ = count ? 2 : 0;
        if (artist_action_ == 2 && !count) artist_action_ = 0;
        if (artist_action_ >= 0) {
            if (pressed & NUVIO_BTN_CROSS) {
                if (artist_action_ < 2) state_.play_artist(artist_action_ == 1);
                else if (artist_action_ == 2) state_.play_top_song(artist_song_);
                else { artist_expanded_ = true; artist_action_ = 2; artist_song_ = 5; }
                return {};
            }
            if (pressed & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
                if (artist_action_ < 2) artist_action_ = pressed & NUVIO_BTN_RIGHT ? 1 : 0;
                else m_bump = true;
                return {};
            }
            if (pressed & NUVIO_BTN_DOWN) {
                if (artist_action_ < 2 && count) { artist_action_ = 2; artist_song_ = 0; }
                else if (artist_action_ == 2 && artist_song_ + 1 < count) ++artist_song_;
                else if (artist_action_ == 2 && more) artist_action_ = 3;
                else if (!state_.page().items.empty()) artist_action_ = -1;
                else m_bump = true;
                return {};
            }
            if (pressed & NUVIO_BTN_UP) {
                if (artist_action_ < 2) tabs_focused_ = true;
                else if (artist_action_ == 3) { artist_action_ = 2; artist_song_ = count - 1; }
                else if (artist_song_) --artist_song_;
                else artist_action_ = 0;
                return {};
            }
        } else if ((pressed & NUVIO_BTN_UP) && state_.absolute() < size_t(kGridColumns)) {
            artist_action_ = more ? 3 : count ? 2 : 0;
            if (count) artist_song_ = count - 1;
            return {};
        }
    }
    if (!tabs_focused_ && album && state_.status() == music::Status::Ready && !state_.page().items.empty()) {
        if (album_play_ && (pressed & NUVIO_BTN_LEFT)) { album_shuffle_ = false; return {}; }
        if (album_play_ && (pressed & NUVIO_BTN_RIGHT)) { album_shuffle_ = true; return {}; }
        if (album_play_ && (pressed & NUVIO_BTN_CROSS)) { state_.play_album(album_shuffle_); return {}; }
        if (album_play_ && (pressed & NUVIO_BTN_DOWN)) { album_play_ = false; return {}; }
        if (album_play_ && (pressed & NUVIO_BTN_UP)) { tabs_focused_ = true; return {}; }
        if (!album_play_ && state_.absolute() == 0 && (pressed & NUVIO_BTN_UP)) { album_play_ = true; album_shuffle_ = false; return {}; }
    }
    if (pressed & NUVIO_BTN_CIRCLE) {
        if (!state_.back()) return {Action::ToNav};
        tabs_focused_ = false;
    } else if (pressed & NUVIO_BTN_TRIANGLE) {
        return {Action::ToNav};
    } else if (tabs_focused_) {
        if (pressed & NUVIO_BTN_UP) return {}; // Header is topmost focus boundary.
        if (pressed & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
            const int tab = state_.tab_index() + ((pressed & NUVIO_BTN_RIGHT) ? 1 : -1);
            if (tab > 3) search_button_ = true;
            else if (tab < 0) m_bump = true;
            else state_.tab(tab);
        }
        if (pressed & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS)) tabs_focused_ = false;
    } else if (gallery && (pressed & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT))) {
        const int delta = pressed & NUVIO_BTN_RIGHT ? 1 : -1;
        const auto occurrence = state_.absolute();
        if ((delta < 0 && occurrence % columns == 0) || (delta > 0 && occurrence % columns == size_t(columns - 1)) || !state_.move(delta)) m_bump = true;
    } else if (pressed & NUVIO_BTN_UP) {
        if (!state_.move(gallery ? -columns : -1)) tabs_focused_ = true;
    } else if (pressed & NUVIO_BTN_DOWN) {
        const auto &page = state_.page();
        const bool more = !page.next_cursor.empty();
        const size_t at = state_.absolute(), end = page.start_index + page.items.size();
        const bool next_row = (at / columns + 1) * columns < end;
        const int delta = !gallery ? 1 : more ? columns : next_row ? int(std::min<size_t>(columns, end - 1 - at)) : 0;
        if (delta <= 0 || !state_.move(delta)) m_bump = true;
    } else if (pressed & NUVIO_BTN_CROSS) {
        if (state_.status() == music::Status::Error) state_.retry();
        else if (state_.page().items.empty() && !state_.page().next_cursor.empty()) state_.next();
        else {
            const auto token = state_.request().token;
            state_.open();
            if (state_.request().token != token) {
                album_play_ = true; album_shuffle_ = false;
                if (state_.request().view == music::View::ArtistAlbums) artist_action_ = 0;
            }
        }
    }
    return {};
}
void MusicScreen::draw(double now, float dt)
{
    search_now_ = now;
    animating_ = false;
    if (drawn_token_ != state_.context()) {
        drawn_token_ = state_.context();
        scroll_.snap(state_.request().view == music::View::Home ? home_scroll_ : 0);
        row_drop_.hide();
        snap_ = true;
    }
    // Parent owns backdrop, branding and frame lifecycle. No enclosing browse panel.
    // Header: wordmark, then tab pill, on pill label baseline (pill y + 48).
    const gfx::TextStyle brand{gfx::Bold, 30};
    gfx::text(100, 106, T("Navi5"), brand, kText);
    const bool searching = state_.request().view == music::View::Search;
    const float pill_x = 100 + gfx::text_width(T("Navi5"), brand) + 32;
    const float search_x = pill_x + 16 + pill_bar(pill_x, 58, {T("Hjem"), T("Artister"), T("Album"), T("Spillelister")},
             searching || search_button_ ? -1 : state_.tab_index(), m_focused && tabs_focused_ && !search_button_, tabs_drop_, dt, 1, &animating_);
    // Active Search gets pane emphasis; keyboard focus gets the same drop as tabs.
    glass_panel({search_x, 58, 76, 76}, 38, searching ? .7f : .35f, false);
    if (m_focused && tabs_focused_ && search_button_) {
        tabs_drop_.to({search_x + 6, 64, 64, 64}, 4);
        tabs_drop_.draw(dt, 1, &animating_, 32);
    }
    for (int i = 0; i < 48; ++i) {
        const float angle = float(i) * 6.2831853f / 48;
        gfx::fill({search_x + 33 + std::cos(angle) * 12, 89 + std::sin(angle) * 12, 3, 3}, kText, 1);
    }
    for (int i = 0; i < 13; ++i) gfx::fill({search_x + 43.f + i, 99.f + i, 4, 4}, kText, 2);
    if (searching) {
        draw_search(dt);
        animating_ = animating_ || art::animating();
        return;
    }
    state_.approach(); // Browse/detail only; search returned above.
    const auto &page = state_.page();
    const auto view = state_.request().view;
    const bool detail = view == music::View::AlbumTracks || view == music::View::PlaylistTracks;
    const bool grid = view == music::View::Albums || view == music::View::Artists || view == music::View::Playlists;
    const float footer_y = 888.f;
    gfx::text(detail ? 166 : 112, detail ? 306 : grid ? 194 : view == music::View::Home ? 222 : 246, !page.detail.title.empty() ? page.detail.title : state_.title().empty() ? heading(view) : state_.title(),
              {gfx::Bold, 36, 1490}, kText);
    if (detail) {
        art::draw({174, 390, 480, 480}, page.detail.artwork_url, "", 720, 720, 18);
        std::string metadata = page.detail.creator;
        auto append = [&](const std::string &value) { if (!value.empty()) { if (!metadata.empty()) metadata += " · "; metadata += value; } };
        if (page.detail.year > 0) append(std::to_string(page.detail.year));
        append(page.detail.genre);
        if (page.detail.count >= 0) append(TN(page.detail.count, "%d spor", "%d spor"));
        if (page.detail.duration > 0) {
            char duration[32];
            std::snprintf(duration, sizeof duration, "%d:%02d", page.detail.duration / 60, page.detail.duration % 60);
            append(duration);
        }
        gfx::text(166, 347, metadata, {gfx::Medium, 22, 1560}, kText2);
    } else if (view != music::View::Home && view != music::View::ArtistAlbums) gfx::text(112, grid ? 231 : 287, heading(view), {gfx::Medium, 22}, kText2);
    const auto status = state_.status();
    if (view == music::View::Home && !page.sections.empty()) {
        select_home();
        constexpr float top = 238, height = 634, row_h = 360;
        const float hero_h = live_ ? 210.f : 0.f;
        const float target = home_row_ < 0 ? 0.f : hero_h + home_row_ * row_h;
        scroll_.to(target);
        if (scroll_.step(dt)) animating_ = true;
        home_scroll_ = scroll_.value;
        gfx::push_scissor({96, top, 1728, height});
        if (live_) {
            const float y = top - scroll_.value;
            if (m_focused && !tabs_focused_ && home_row_ < 0) {
                row_drop_.to({100, y, 1712, 194}, -1);
                row_drop_.draw(dt, 1, &animating_, 18);
            }
            art::draw({120, y + 16, 160, 160}, session_art_, "", 720, 720, 12);
            gfx::text(316, y + 35, T("Fortsett å lytte"), {gfx::Medium, 22}, kText2);
            gfx::text(316, y + 83, session_title_.empty() ? T("Uten tittel") : session_title_, {gfx::Bold, 38, 1320}, kText);
            gfx::text(316, y + 121, session_artist_, {gfx::Medium, 25, 1320}, kText2);
            gfx::text(316, y + 164, paused_ ? T("Fortsett") : T("Åpne spiller"), {gfx::SemiBold, 25}, kText);
        }
        for (size_t s = 0; s < page.sections.size(); ++s) {
            const auto &section = page.sections[s];
            const float y = top + hero_h + s * row_h - scroll_.value;
            gfx::text(116, y + 32, s == 0 ? T("Nylig spilt") : T("Nylig lagt til"), {gfx::Bold, 30}, kText);
            const bool focused = m_focused && !tabs_focused_ && home_row_ == int(s);
            if (focused) {
                row_drop_.to({100 + float(section.count ? home_column_ : 0) * 286, y + 44, section.count ? 278.f : 1712.f, 304}, int(s * 6 + home_column_));
                row_drop_.draw(dt, 1, &animating_, 18);
            }
            if (!section.count) {
                gfx::text(116, y + 120, !section.ok ? T("Kunne ikke hente musikken") : s == 0 ? T("Ingen nylig spilte kontekster") : T("Ingen musikk her ennå"), {gfx::Medium, 27}, kText2);
                if (!section.ok) gfx::text(116, y + 165, T("Prøv igjen"), {gfx::SemiBold, 25}, kText);
            }
            for (size_t i = 0; i < section.count; ++i) {
                const auto &item = page.items[section.begin + i];
                const float x = 116 + float(i) * kGridCell;
                const gfx::Rect cover{x, y + 56, 224, 224};
                if (item.artwork_url.empty()) draw_title_card(cover, first_letter(item.title), static_cast<int>(i), 12, 1);
                else art::draw(cover, item.artwork_url, "", 720, 720, 12);
                gfx::text(x, y + 309, item.title.empty() ? T("Uten tittel") : item.title, {gfx::SemiBold, 25, kGridTitle}, kText);
                gfx::text(x, y + 335, item.subtitle, {gfx::Medium, 20, kGridTitle}, kText2);
            }
        }
        if (!m_focused || tabs_focused_) row_drop_.hide();
        gfx::pop_scissor();
    } else if (view == music::View::ArtistAlbums && status == music::Status::Ready) {
        if (m_focused && !tabs_focused_ && artist_action_ >= 0 && artist_action_ < 2)
            play_drop_.to({100.f + artist_action_ * 100, 302, 88, 64}, artist_action_);
        else play_drop_.hide();
        play_drop_.draw(dt, 1, &animating_, 22);
        draw_music_icon(MusicIcon::Play, 144, 334, kText);
        draw_music_icon(MusicIcon::Shuffle, 244, 334, kText);
        const size_t count = std::min(page.top_songs.size(), artist_expanded_ ? size_t(20) : size_t(5));
        const bool more = !artist_expanded_ && page.top_songs.size() > 5;
        const float album_top = 60 + std::max(size_t(1), count) * 70.f + (more ? 60 : 0) + 54;
        const size_t base = page.start_index, at = state_.absolute();
        float selected_y = 0;
        if (artist_action_ == 2) selected_y = 54 + artist_song_ * 70.f;
        else if (artist_action_ == 3) selected_y = 60 + count * 70.f;
        else if (artist_action_ < 0) selected_y = album_top + float(at / kGridColumns) * kArtistAlbumRow;
        const float content_height = album_top + float((base + page.items.size() + kGridColumns - 1) / kGridColumns) * kArtistAlbumRow;
        const float artist_target = std::clamp(selected_y - 100, 0.f, page.next_cursor.empty() ? std::max(0.f, content_height - 488) : std::max(0.f, selected_y - 100));
        if (snap_) scroll_.snap(artist_target), snap_ = false;
        scroll_.to(artist_target);
        if (scroll_.step(dt)) animating_ = true;
        const float top = 384 - scroll_.value;
        gfx::push_scissor({96, 384, 1728, 488});
        gfx::text(116, top + 32, T("Topplåter"), {gfx::Bold, 30}, kText);
        if (!count) gfx::text(116, top + 100, T("Topplåter utilgjengelig"), {gfx::Medium, 26}, kText2);
        gfx::Rect focus{};
        if (artist_action_ == 2) focus = {100, top + 54 + artist_song_ * 70.f, 1712, 66};
        else if (artist_action_ == 3) focus = {100, top + 60 + count * 70.f, 1712, 56};
        else if (artist_action_ < 0 && !page.items.empty()) focus = {100 + float(at % kGridColumns) * kGridCell, top + album_top + float(at / kGridColumns) * kArtistAlbumRow + 4, kGridCard, kArtistAlbumRow - 8};
        if (m_focused && !tabs_focused_ && (artist_action_ < 0 || artist_action_ >= 2)) row_drop_.to(focus, int(artist_action_ == 2 ? artist_song_ : artist_action_ == 3 ? 21 : 22 + at));
        else row_drop_.hide();
        row_drop_.draw(dt, 1, &animating_, 18);
        for (size_t i = 0; i < count; ++i) {
            const float y = top + 54 + i * 70;
            if (y + 70 < 384 || y > 872) continue;
            const auto &song = page.top_songs[i];
            art::draw({116, y + 5, 56, 56}, song.artwork_url, "", 720, 720, 8);
            gfx::text(194, y + 29, song.title, {gfx::SemiBold, 26, 1450}, kText);
            gfx::text(194, y + 56, song.subtitle, {gfx::Medium, 20, 1450}, kText2);
        }
        if (more) gfx::text(124, top + 100 + count * 70.f, T("Mer"), {gfx::Bold, 26}, kText);
        gfx::text(116, top + album_top - 16, T("Album"), {gfx::Bold, 30}, kText);
        for (size_t i = 0; i < page.items.size(); ++i) {
            const size_t n = base + i; // Absolute occurrence.
            const float x = 116 + float(n % kGridColumns) * kGridCell, y = top + album_top + float(n / kGridColumns) * kArtistAlbumRow;
            if (y + kArtistAlbumRow < 384 || y > 872) continue;
            const auto &item = page.items[i];
            art::draw({x, y + 10, kArtistAlbumArt, kArtistAlbumArt}, item.artwork_url, "", 720, 720, 12);
            gfx::text(x, y + kArtistAlbumArt + 38, item.title.empty() ? T("Uten tittel") : item.title, {gfx::SemiBold, 25, kGridTitle}, kText);
            gfx::text(x, y + kArtistAlbumArt + 64, item.subtitle, {gfx::Medium, 20, kGridTitle}, kText2);
        }
        gfx::pop_scissor();
    } else if (status != music::Status::Ready || page.items.empty()) {
        const char *message = status == music::Status::Error ? T("Kunne ikke hente musikken") :
            status != music::Status::Ready ? T("Henter biblioteket …") :
            page.next_cursor.empty() ? T("Ingen musikk her ennå") : state_.continuation_error() ? T("Kunne ikke hente flere.") :
            state_.at_ceiling() ? T("Grensen for rulling er nådd") : T("Henter flere …");
        gfx::text(960, 555, message, {gfx::SemiBold, 32, 1320}, kText, 1);
        if (status == music::Status::Error) {
            if (m_focused && !tabs_focused_) row_drop_.to({740, 606, 440, 68}, 0);
            else row_drop_.hide();
            row_drop_.draw(dt, 1, &animating_, 24);
            gfx::text(960, 649, T("Prøv igjen"), {gfx::Bold, 26}, kText, 1);
        }
    } else if (view == music::View::Home || view == music::View::Albums || view == music::View::ArtistAlbums || view == music::View::Artists || view == music::View::Playlists) {
        // Absolute occurrences, so evicting window pages never jumps.
        const size_t columns = kGridColumns, base = page.start_index, at = state_.absolute();
        const float top = 252.f, cell = kGridCell;
        // Reserve compact-player space before first playback; cards never reflow.
        const float row_h = 304.f;
        const float height = row_h * 2;
        const float artwork = row_h - 70.f;
        const float card_width = kGridCard, title_width = kGridTitle;
        const float target = std::max(0.f, float(at / columns + 1) * row_h - height);
        if (snap_) scroll_.snap(target), snap_ = false;
        scroll_.to(target);
        if (scroll_.step(dt)) animating_ = true;
        gfx::push_scissor({96, top, 1728, height});
        if (m_focused && !tabs_focused_) row_drop_.to({100 + float(at % columns) * cell, top + float(at / columns) * row_h - scroll_.value + 4.f, card_width, row_h - 8.f}, int(at));
        else row_drop_.hide();
        row_drop_.draw(dt, 1, &animating_, 18);
        for (size_t i = 0; i < page.items.size(); ++i) {
            const size_t n = base + i;
            const float x = 116 + float(n % columns) * cell, y = top + float(n / columns) * row_h - scroll_.value;
            if (y + row_h <= top || y >= top + height) continue;
            const auto &item = page.items[i];
            art::draw({x, y + 10, artwork, artwork}, item.artwork_url, "", 720, 720, 12);
            gfx::text(x, y + artwork + 38, item.title.empty() ? T("Uten tittel") : item.title, {gfx::SemiBold, 25, title_width}, kText);
            const auto caption = (view == music::View::Artists || view == music::View::Playlists)
                ? (item.count >= 0 ? (view == music::View::Artists ? TN(item.count, "%d album", "%d album") : TN(item.count, "%d spor", "%d spor")) : "") : item.subtitle;
            gfx::text(x, y + artwork + 64, caption, {gfx::Medium, 20, title_width}, kText2);
        }
        // Truthful tail: loading, retryable failure, explicit ceiling or end.
        const float tail = top + float((base + page.items.size() + columns - 1) / columns) * row_h - scroll_.value;
        const char *status_line = state_.continuation_error() ? T("Kunne ikke hente flere.") :
            state_.busy() ? T("Henter flere …") : state_.at_ceiling() ? T("Grensen for rulling er nådd") : nullptr;
        if (status_line && tail < top + height) gfx::text(960, std::max(tail, top) + 48, status_line, {gfx::SemiBold, 26, 1320}, kText2, 1);
        gfx::pop_scissor();
    } else {
        const bool album = detail;
        const float left = album ? 770.f : 154.f, width = album ? 996.f : 1612.f;
        if (album) {
            if (m_focused && !tabs_focused_ && album_play_) play_drop_.to({left + (album_shuffle_ ? 112.f : 0.f), 372, 88, 64}, album_shuffle_ ? -2 : -1);
            else play_drop_.hide();
            play_drop_.draw(dt, 1, &animating_, 22);
            draw_music_icon(MusicIcon::Play, left + 44, 404, kText);
            draw_music_icon(MusicIcon::Shuffle, left + 156, 404, kText);
        }
        const float top = album ? 448.f : 380.f, height = album ? 424.f : 492.f, row_h = 82;
        const size_t base = page.start_index, end = base + page.items.size();
        const float target = std::max(0.f, static_cast<float>(state_.absolute()) * row_h - 2 * row_h);
        const float limit = page.next_cursor.empty() ? std::max(0.f, end * row_h - height) : target;
        if (snap_) scroll_.snap(std::min(target, limit)), snap_ = false;
        scroll_.to(std::min(target, limit));
        if (scroll_.step(dt)) animating_ = true;
        gfx::push_scissor({left, top, width, height});
        gfx::push_fade_mask({left, top, width, height}, edge_fade(scroll_.value, 24),
            edge_fade(end * row_h - height - scroll_.value, 24));
        const float selected_y = top + state_.absolute() * row_h - scroll_.value;
        if (m_focused && !tabs_focused_ && !(album && album_play_))
            row_drop_.to({left + 4, selected_y + 3, width - 8, 76}, static_cast<int>(state_.absolute()), 0, -scroll_.value);
        else row_drop_.hide();
        row_drop_.draw(dt, 1, &animating_, 22);
        for (size_t i = 0; i < page.items.size(); ++i) {
            const float y = top + (base + i) * row_h - scroll_.value;
            if (y + row_h < top || y > top + height) continue;
            const auto &item = page.items[i];
            const bool focused = m_focused && !tabs_focused_ && i == state_.selected();
            const gfx::Rect art_rect{left + 20, y + 9, 62, 62};
            if (item.artwork_url.empty()) draw_title_card(art_rect, first_letter(item.title), static_cast<int>(i), 12, 1);
            else art::draw(art_rect, item.artwork_url, "", 720, 720, 12);
            marquee_text(left + 104, y + 34, item.title.empty() ? T("Uten tittel") : item.title,
                          {gfx::SemiBold, 27, width - 250}, kText, focused);
            gfx::text(left + 104, y + 62, item.subtitle, {gfx::Medium, 21, width - 250}, kText2);
            if (item.duration > 0) {
                char duration[32];
                std::snprintf(duration, sizeof duration, "%d:%02d", item.duration / 60, item.duration % 60);
                gfx::text(1724, y + 46, duration, {gfx::Medium, 23}, kText2, 2);
            }
        }
        gfx::pop_fade_mask();
        gfx::pop_scissor();
    }
    std::vector<PadHint> hints{{PadButton::Cross, status == music::Status::Error || state_.continuation_error() ? T("Prøv igjen") :
        page.items.empty() && !page.next_cursor.empty() ? T("Hent flere") :
        view == music::View::ArtistAlbums && artist_action_ >= 0 && !tabs_focused_
            ? (artist_action_ == 3 ? T("Mer") : artist_action_ == 2 ? T("Spill av") : artist_action_ == 1 ? T("Bland") : T("Spill alle")) :
        (view == music::View::AlbumTracks || view == music::View::PlaylistTracks) ? (album_play_ && album_shuffle_ ? T("Bland") : T("Spill av")) : T("Åpne")},
        {PadButton::Circle, T("Tilbake")}};
    draw_pad_hints(100, footer_y + 34, hints);
    animating_ = art::animating() || marquee_take_animating() || animating_;
}
namespace {
void transport_icon(float x, float y, int direction, bool paused, uint32_t color)
{
    if (!direction && !paused) {
        gfx::fill({x - 14, y - 19, 9, 38}, color, 2);
        gfx::fill({x + 5, y - 19, 9, 38}, color, 2);
        return;
    }
    // Native filled geometry: no font glyph assumptions or GPU texture ownership.
    const float sign = direction < 0 ? -1.f : 1.f;
    for (int row = -20; row < 20; ++row) {
        const float width = 30.f * (1.f - std::abs(row + .5f) / 20.f);
        gfx::fill({sign < 0 ? x + 12 - width : x - 12, y + row, width, 1}, color);
    }
    if (direction) gfx::fill({direction < 0 ? x - 22 : x + 22, y - 20, 5, 40}, color, 1);
}
void shuffle_icon(float x, float y, uint32_t color)
{
    for (int i = 0; i < 28; ++i) {
        const float offset = i < 7 ? -9.f : i > 20 ? 9.f : (i - 7) * 18.f / 14.f - 9;
        gfx::fill({x + i, y + offset - 2, 2, 4}, color, 1);
        gfx::fill({x + i, y - offset - 2, 2, 4}, color, 1);
    }
    for (int i = 0; i < 7; ++i)
        for (int sign : {-1, 1})
            gfx::fill({x + 23 + i, y + sign * 9 - (6.f - i), 1, 2.f * (6 - i) + 1}, color);
}
std::string playback_time(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds > std::numeric_limits<int>::max()) return "--:--";
    const int value = static_cast<int>(seconds);
    char text[32];
    std::snprintf(text, sizeof text, "%d:%02d", value / 60, value % 60);
    return text;
}
}
void draw_music_icon(MusicIcon icon, float x, float y, uint32_t color)
{
    if (icon == MusicIcon::Shuffle) shuffle_icon(x - 15, y, color);
    else transport_icon(x, y, icon == MusicIcon::Previous ? -1 : icon == MusicIcon::Next ? 1 : 0,
                        icon == MusicIcon::Play, color);
}
const char *player_control_label(const PlaybackPresentation &p, int focus)
{
    switch (focus) {
    case 0: return T("Forrige spor");
    case 1: return p.paused ? T("Fortsett") : T("Pause");
    case 2: return T("Neste spor");
    case 4: return p.shuffle_pending ? T("Bland: endrer …") : p.shuffle_enabled ? T("Bland: på") : T("Bland: av");
    default: return p.seeking ? T("Søker / bufrer …") : p.seek_failed ? T("Kunne ikke søke. Prøv igjen.") :
        !p.seekable ? T("Søking utilgjengelig") : T("Venstre/høyre: 10 s   Cross: søk   Opp: kontroller");
    }
}
void draw_mini_player_progress(double position, double duration)
{
    // Below metadata/hints, right of artwork; stays inside compact glass and TV-safe bounds.
    gfx::fill({250, 1038, 1500, 3}, 0x55ffffff, 1.5f);
    if (std::isfinite(duration) && duration > 0 && std::isfinite(position)) {
        const float fraction = static_cast<float>(std::clamp(position / duration, 0.0, 1.0));
        if (fraction > 0) gfx::fill({250, 1038, 1500 * fraction, 3}, kText, 1.5f);
    }
}
void draw_music_player(const PlaybackPresentation &p, int focus, float controls_alpha,
                       float dt, Drop &transport_drop)
{
    gfx::fill({0, 0, gfx::W, gfx::H}, 0xff15131b);
    if (const auto *photo = art::get(p.portrait, 1920, 1080)) {
        gfx::image({0, 0, gfx::W, gfx::H}, photo, 1.f, 0, true);
        gfx::fill({0, 0, gfx::W, gfx::H}, 0x9515131b);
    } else if (!p.artwork.empty()) {
        art::draw({0, 0, gfx::W, gfx::H}, p.artwork, "", 720, 720, 0);
        gfx::backdrop_blur({0, 0, gfx::W, gfx::H}, 0, 48);
        gfx::fill({0, 0, gfx::W, gfx::H}, 0xda15131b);
    }
    gfx::fill_vgradient({0, 320, gfx::W, 760}, 0x0015131b, 0xff15131b);
    const float a = p.preview ? 1.f : (std::isfinite(controls_alpha) ? std::clamp(controls_alpha, 0.f, 1.f) : 1.f);
    const float slide = 104.f * (1.f - a);
    art::draw({120, 510 + slide, 300, 300}, p.artwork, "", 720, 720, 8);
    gfx::text(460, 697 + slide, p.title.empty() ? T("Uten tittel") : p.title, {gfx::Bold, 64, 1320}, kText);
    gfx::text(460, 759 + slide, p.artist, {gfx::Medium, 34, 1320}, kText2);

    // Preview never changes playback until Cross; unknown duration stays read-only.
    const double position = p.preview && std::isfinite(p.target) && p.target >= 0 ? p.target : p.position;
    const bool known_duration = std::isfinite(p.duration) && p.duration > 0;
    const bool known_position = std::isfinite(p.position) && p.position >= 0;
    gfx::fill({120, 992, 1680, 6}, 0x55ffffff, 3);
    if (known_duration && known_position) {
        const float fraction = static_cast<float>(std::clamp(position / p.duration, 0.0, 1.0));
        if (fraction > 0) gfx::fill({120, 992, 1680 * fraction, 6}, kText, 3);
    }
    if (a > .01f) {
        gfx::text(120, 1024, playback_time(position), {gfx::Medium, 23}, alpha(kText2, a));
        gfx::text(1800, 1024, known_duration ? playback_time(p.duration) : "--:--", {gfx::Medium, 23}, alpha(kText2, a), 2);
        focus = std::clamp(focus, 0, 4);
        if (focus == 3) transport_drop.hide();
        else if (focus == 4) transport_drop.to({456, 838, 88, 80}, focus);
        else transport_drop.to({120.f + focus * 112.f, 838, 88, 80}, focus);
        if (focus == 3) {
            const float fraction = known_duration && known_position ?
                static_cast<float>(std::clamp(position / p.duration, 0.0, 1.0)) : 0.f;
            const float x = 120 + 1680 * fraction;
            gfx::fill({x - 7, 980, 14, 30}, alpha(kText, a), 7);
        }
        bool moving = false;
        if (focus != 3) transport_drop.draw(dt, a, &moving, 24);
        for (int i = 0; i < 3; ++i)
            draw_music_icon(i == 0 ? MusicIcon::Previous : i == 2 ? MusicIcon::Next :
                            p.paused ? MusicIcon::Play : MusicIcon::Pause,
                            164 + i * 112.f, 878, alpha(kText, a));
        // Solid inset plate distinguishes enabled without color; outer glass remains focus-only.
        // Pending retains confirmed state until the toggle completes.
        if (p.shuffle_enabled) gfx::fill({472, 854, 56, 48}, alpha(0xff354038, a), 12);
        draw_music_icon(MusicIcon::Shuffle, 500, 878, alpha(p.shuffle_enabled ? 0xffa8bfa8 : kText, a));
        if (p.preview)
            draw_pad_hints(960, 1016, {{PadButton::Cross, T("Utfør søk")}, {PadButton::Circle, T("Avbryt søk")}}, 1, 28, a);
    }
    // One playback status slot: seek/buffering already arrives in p.status from main.
    // Separate bounded columns keep concurrent shuffle feedback from touching it.
    if (!p.error) {
        const std::string status = p.seeking || p.seek_failed ? player_control_label(p, 3) : p.status;
        if (!status.empty() && status != T("Spiller") && status != T("Pause"))
            gfx::text(1800, 950, status, {gfx::Medium, 23, 800}, kText2, 2);
    }
    if (p.shuffle_error) gfx::text(120, 950, T("Kunne ikke endre blanding. Prøv igjen."), {gfx::Medium, 23, 800}, kText);
    else if (p.shuffle_pending) gfx::text(120, 950, T("Bland: endrer …"), {gfx::Medium, 23, 800}, kText);
    if (p.error) gfx::text(120, 450, p.status, {gfx::Medium, 25, 1680}, kText);
    if (a > .99f && known_duration && p.duration <= std::numeric_limits<int>::max() &&
        show_up_next(p.position, static_cast<int>(p.duration), !p.next_title.empty(), false, p.playing)) {
        glass_panel({1300, 795, 500, 120}, 24);
        gfx::text(1330, 837, T("Neste spor"), {gfx::Medium, 22}, kText2);
        gfx::text(1330, 887, p.next_title, {gfx::SemiBold, 30, 440}, kText);
    }
}
} // namespace ui
