/* Navi5 — actual MusicScreen input regression checks; renderer stays offline.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-private-field" // Drop renderer stub does not use GPU spring fields.
#endif
#include "ui/music_screen.h"
#include "ui/player_state.h"
#include "gfx/art.h"
#include "nuvio_input.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

static std::vector<std::string> drawn_text, drawn_art;
static std::vector<gfx::Rect> fills, panels, scissors, drops, artwork_rects;
static std::vector<ui::PadHint> hints, hint_calls;
static float hints_y = 0;
static int pill_selected = -1;
static bool pill_focused = false;
struct PaintedFill { gfx::Rect rect; uint32_t color; float radius; };
static std::vector<PaintedFill> painted_fills;
// Link real screen/input code without GPU, artwork workers or platform fonts.
#include "app/i18n.h"
extern "C" int sceSystemServiceParamGetInt(int, int *) { return -1; }
namespace gfx {
void fill(const Rect &r, uint32_t color, float radius) { fills.push_back(r); painted_fills.push_back({r, color, radius}); }
void fill_vgradient(const Rect &, uint32_t, uint32_t, float) {}
void image(const Rect &, const Texture *, float, float, bool) {}
int backdrop_blur(const Rect &, float, float, float, float) { return 0; }
struct Placed { std::string text; float x, y, size, max_w; };
static std::vector<Placed> placed;
float text(float x, float y, const std::string &text, const TextStyle &style, uint32_t, int) {
    drawn_text.push_back(text); placed.push_back({text, x, y, style.size, style.max_w}); return 0;
}
float text_width(const std::string &text, const TextStyle &style) { return text.size() * style.size * .6f; }
void push_scissor(const Rect &r) { scissors.push_back(r); }
void pop_scissor() {}
void push_fade_mask(const Rect &, float, float, float, float) {}
void pop_fade_mask() {}
}
namespace art {
const gfx::Texture *get(const std::string &, int, int) { return nullptr; }
void draw(const gfx::Rect &r, const std::string &url, const std::string &, int, int, float, float, uint32_t) { drawn_art.push_back(url); artwork_rects.push_back(r); }
bool animating() { return false; }
}
namespace ui {
uint32_t alpha(uint32_t color, float) { return color; }
void Drop::to(const gfx::Rect &r, int, float, float) { drops.push_back(r); }
void Drop::draw(float, float, bool *, float) {}
void glass_panel(const gfx::Rect &r, float, float, bool, float) { panels.push_back(r); }
float pill_bar(float x, float y, const std::vector<std::string> &labels, int on, bool focused, Drop &, float, float, bool *, float, float, float) {
    pill_selected = on; pill_focused = focused;
    assert(y == 58 && labels.size() == 4);
    panels.push_back({x, y, 720, 76}); return 720; // Stub pill width; real width is font-measured.
}
void draw_title_card(const gfx::Rect &, const std::string &, int, float, float) {}
float marquee_text(float, float, const std::string &, const gfx::TextStyle &, uint32_t, bool, int) { return 0; }
bool marquee_take_animating() { return false; }
float draw_pad_hints(float, float y, const std::vector<PadHint> &value, int, float, float) { hints = value; hint_calls.insert(hint_calls.end(), value.begin(), value.end()); hints_y = y; return 0; }
}
using namespace ui::music;
static Item item(Kind kind, const char *id) { return {kind, id, "Fixture", "", "", 0}; }
static void complete(State &state, Page page, int line = __builtin_LINE())
{
    const auto request = state.take_request();
    if (!request || !state.complete(request->token, std::move(page))) { std::cerr << "complete failed line " << line << "\n"; std::abort(); }
}
int main()
{
    // Read-only compact progress: finite, bounded, independent of paused/playing state.
    const auto mini_progress = [](double position, double duration, float width) {
        fills.clear();
        ui::draw_mini_player_progress(position, duration);
        assert(fills.size() == (width > 0 ? 2 : 1));
        const auto &rail = fills.front();
        assert(rail.x == 250 && rail.y == 1038 && rail.w == 1500 && rail.h == 3);
        assert(rail.x > 158 + 70 && rail.y > 1018 + 26 / 2); // artwork/hint clearance
        assert(rail.x >= 140 && rail.y >= 950 && rail.x + rail.w <= 1780 && rail.y + rail.h <= 1050);
        if (width > 0) {
            const auto &fill = fills.back();
            assert(fill.x == rail.x && fill.y == rail.y && fill.h == rail.h);
            assert(std::isfinite(fill.w) && fill.w == width && fill.w <= rail.w);
        }
    };
    mini_progress(30, 120, 375);
    mini_progress(30, 120, 375); // Paused snapshot retains fraction.
    mini_progress(-1, 120, 0);
    mini_progress(0, 120, 0);
    mini_progress(240, 120, 1500);
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()})
        mini_progress(30, invalid, 0);
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        mini_progress(invalid, 120, 0);
    fills.clear();
    for (int n : {0, 1, 2}) {
        i18n::set_choice(i18n::Choice::English);
        assert(TN(n, "%d spor", "%d spor") == std::to_string(n) + (n == 1 ? " track" : " tracks"));
        assert(TN(n, "%d album", "%d album") == std::to_string(n) + (n == 1 ? " album" : " albums"));
        i18n::set_choice(i18n::Choice::Norwegian);
        assert(TN(n, "%d spor", "%d spor") == std::to_string(n) + " spor");
        assert(TN(n, "%d album", "%d album") == std::to_string(n) + " album");
    }
    ui::MusicScreen screen;
    auto &state = screen.state();
    screen.activate();
    complete(state, {{item(Kind::Album, "album")}, ""});
    screen.input(NUVIO_BTN_DOWN); // tabs to gallery
    screen.input(NUVIO_BTN_CROSS); // enter album, focus Play album
    complete(state, {{item(Kind::Track, "one"), item(Kind::Track, "two"), item(Kind::Track, "three")}, ""});
    screen.input(NUVIO_BTN_CROSS);
    auto play = state.take_play_intent();
    assert(play && play->song_id.empty() && play->context_id == "album");
    screen.input(NUVIO_BTN_DOWN); // Play album to first track
    screen.input(NUVIO_BTN_DOWN); // second track
    assert(state.selected() == 1);
    const auto token = state.request().token;
    for (int i = 0; i < 2; ++i) {
        screen.input(NUVIO_BTN_CROSS);
        play = state.take_play_intent();
        assert(play && play->song_id == "two"); // repeated Cross must not play whole album
        assert(state.selected() == 1 && state.request().token == token);
    }
    screen.input(NUVIO_BTN_DOWN);
    assert(state.selected() == 2); // track focus still owns direction after playback
    screen.input(NUVIO_BTN_UP);
    screen.input(NUVIO_BTN_UP);
    assert(state.selected() == 0);
    screen.input(NUVIO_BTN_UP); // first track to Play album
    assert(state.move(2)); // retained nonzero row while Play album owns focus
    assert(screen.input(NUVIO_BTN_UP).kind == ui::Action::None);
    assert(state.selected() == 2); // Play album Up must not move retained row
    assert(screen.input(NUVIO_BTN_UP).kind == ui::Action::None); // tabs clamp Up
    screen.input(NUVIO_BTN_DOWN);
    screen.input(NUVIO_BTN_CROSS);
    play = state.take_play_intent();
    assert(play && play->song_id.empty()); // returning from tabs still focuses Play album
    {
        // Magnifier: right of last tab, outside pill; Search owns its focus drop.
        ui::MusicScreen s;
        auto &st = s.state();
        s.activate(); complete(st, {{item(Kind::Album, "a")}, ""});
        for (int i = 0; i < 3; ++i) s.input(NUVIO_BTN_RIGHT);
        assert(st.tab_index() == 3);
        complete(st, {{item(Kind::Playlist, "p")}, ""});
        s.input(NUVIO_BTN_RIGHT); // Past Playlists: magnifier focus, no request.
        assert(!st.take_request() && st.request().view == View::Playlists);
        panels.clear(); drops.clear(); s.draw(0, .05f);
        assert(std::any_of(panels.begin(), panels.end(), [](const gfx::Rect &r) { return r.x >= 100 + 720 && r.w == 76; }));
        assert(pill_selected == -1 && !pill_focused && drops.size() == 1);
        assert(drops.back().y == 64 && drops.back().w == 64);
        for (int i = 0; i < 3; ++i) assert(s.input(NUVIO_BTN_UP).kind == ui::Action::None);
        s.input(NUVIO_BTN_CROSS);
        assert(st.request().view == View::Search);
        drops.clear(); s.draw(0, .05f);
        assert(pill_selected == -1 && !pill_focused && drops.size() == 1 && drops.back().y == 380);
        s.input(NUVIO_BTN_UP); // Keyboard top hands off to Search exactly once.
        for (int i = 0; i < 3; ++i) assert(s.input(NUVIO_BTN_UP).kind == ui::Action::None);
        drops.clear(); s.draw(0, .05f);
        assert(pill_selected == -1 && drops.size() == 1 && drops.back().y == 64);
        s.input(NUVIO_BTN_DOWN);
        s.input(NUVIO_BTN_CROSS); s.input(NUVIO_BTN_RIGHT); s.input(NUVIO_BTN_CROSS); // "a", "b"
        assert(st.query() == "ab");
        for (int i = 0; i < 5; ++i) s.input(NUVIO_BTN_DOWN);
        s.input(NUVIO_BTN_RIGHT);
        s.input(NUVIO_BTN_CROSS); // Delete.
        assert(st.query() == "a");
        st.tick(1);
        complete(st, {{item(Kind::Track, "t"), item(Kind::Album, "al")}, ""});
        s.input(NUVIO_BTN_RIGHT); s.input(NUVIO_BTN_RIGHT); s.input(NUVIO_BTN_RIGHT); // Clear, Symbols, then edge to results.
        s.input(NUVIO_BTN_UP); // First result to Search header, never hidden nav.
        for (int i = 0; i < 3; ++i) assert(s.input(NUVIO_BTN_UP).kind == ui::Action::None);
        drops.clear(); s.draw(0, .05f);
        assert(st.query() == "a" && pill_selected == -1 && drops.size() == 1 && drops.back().y == 64);
        s.input(NUVIO_BTN_DOWN); // Back into keyboard, preserving query and Clear key.
        s.input(NUVIO_BTN_RIGHT); // Results again.
        s.input(NUVIO_BTN_DOWN); s.input(NUVIO_BTN_CROSS);
        auto detail = st.take_request();
        assert(detail && detail->view == View::AlbumTracks && detail->parent_id == "al");
        s.input(NUVIO_BTN_CIRCLE);
        assert(st.request().view == View::Search && st.query() == "a" && st.selected() == 1);
        s.input(NUVIO_BTN_CIRCLE); s.input(NUVIO_BTN_LEFT); s.input(NUVIO_BTN_CROSS); // Results to keyboard; Symbols left to Clear.
        assert(st.query().empty() && st.page().items.empty());
        s.input(NUVIO_BTN_CIRCLE); // Leave search.
        assert(st.request().view == View::Playlists && !st.take_request());
        s.input(NUVIO_BTN_R1);
        assert(st.request().view == View::Search); // Playlists to Search.
        s.input(NUVIO_BTN_R1);
        assert(st.tab_index() == 0 && st.request().view == View::Home);
    }
    {
        // Both shoulder directions include Search; root grid restores after Search/back.
        ui::MusicScreen s;
        auto &st = s.state();
        s.activate(); complete(st, {});
        const View forward[]{View::Artists, View::Albums, View::Playlists, View::Search, View::Home};
        for (auto view : forward) {
            s.input(NUVIO_BTN_R1); assert(st.request().view == view);
            if (view != View::Search) complete(st, {});
        }
        const View backward[]{View::Search, View::Playlists, View::Albums, View::Artists, View::Home};
        for (auto view : backward) {
            s.input(NUVIO_BTN_L1); assert(st.request().view == view);
            if (st.status() == Status::Loading) complete(st, {});
        }
    }
    {
        ui::MusicScreen s;
        auto &st = s.state();
        s.activate(); complete(st, {});
        s.input(NUVIO_BTN_R1);
        Page grid;
        for (int i = 0; i < 12; ++i) grid.items.push_back(item(Kind::Artist, "artist"));
        complete(st, grid);
        s.input(NUVIO_BTN_DOWN); s.input(NUVIO_BTN_RIGHT);
        assert(st.selected() == 1);
        s.input(NUVIO_BTN_UP); // First row still hands off to header.
        for (int i = 0; i < 4; ++i) {
            assert(s.input(NUVIO_BTN_UP).kind == ui::Action::None);
            assert(st.selected() == 1);
            s.draw(0, .05f); assert(pill_selected == 1 && pill_focused);
        }
        s.input(NUVIO_BTN_DOWN); s.input(NUVIO_BTN_CROSS);
        assert(st.request().view == View::ArtistAlbums); // Valid content focus survived.
        complete(st, {});
        s.input(NUVIO_BTN_L1); // Detail to Home root, then Search.
        complete(st, {});
        s.input(NUVIO_BTN_L1); assert(st.request().view == View::Search);
        s.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < 7; ++i) s.input(NUVIO_BTN_RIGHT); // Empty results must not steal focus.
        drops.clear(); s.draw(0, .05f);
        assert(drops.size() == 1 && drops.back().y == 380);
        s.input(NUVIO_BTN_CIRCLE);
        assert(st.request().view == View::Home);
        s.input(NUVIO_BTN_R1); // Root Artists window/selection cached through detail and Search.
        assert(st.request().view == View::Artists && st.selected() == 1 && !st.take_request());
    }
    {
        ui::MusicScreen s;
        auto &st = s.state();
        s.activate(); complete(st, {{item(Kind::Album, "a")}, ""});
        for (int i = 0; i < 4; ++i) s.input(NUVIO_BTN_RIGHT);
        s.input(NUVIO_BTN_CROSS);
        assert(st.request().view == View::Search);
        const auto contains = [](const char *text) { return std::count(drawn_text.begin(), drawn_text.end(), text); };
        for (auto language : {i18n::Choice::English, i18n::Choice::Norwegian}) {
            i18n::set_choice(language); drawn_text.clear(); s.draw(0, .05f);
            assert(contains(T("Skriv for å søke")) == 1);
            assert(contains(T("Tilbaketast")) == 1 && !contains("Delete"));
            for (const char *n : {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"}) assert(contains(n));
            for (const char *special : {"-", "'", "æ", "ø", "å", "é", "ü", "ö", "&", "."}) assert(!contains(special));
            for (const char *footer : {"Right: results", "Høyre: resultater", "Up to 5 per group · first matches", "Inntil 5 per gruppe · første treff"}) assert(!contains(footer));
        }
        // Last occupied row clamps Down; no focusable blank cells.
        for (int i = 0; i < 5; ++i) s.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < 4; ++i) s.input(NUVIO_BTN_RIGHT);
        st.query("東京æ", 0);
        const auto before_switch = st.request().token;
        s.input(NUVIO_BTN_CROSS); // Symbols, no query mutation.
        assert(st.query() == "東京æ" && st.request().token == before_switch);
        drawn_text.clear(); s.draw(0, .05f);
        for (const char *special : {"-", "'", "æ", "ø", "å", "é", "ü", "ö", "&", ".", "ABC"}) assert(contains(special));
        assert(!contains("0") && !contains("a"));
        s.input(NUVIO_BTN_SQUARE); assert(st.query() == "東京");
        s.input(NUVIO_BTN_CIRCLE); // Main keyboard, focus Symbols restored.
        assert(st.request().view == View::Search && st.query() == "東京");
        s.input(NUVIO_BTN_CROSS); // Symbols again proves focus restored.
        for (int i = 0; i < 6; ++i) s.input(NUVIO_BTN_RIGHT);
        s.input(NUVIO_BTN_DOWN); s.input(NUVIO_BTN_CROSS); // ABC.
        assert(st.query() == "東京");
        assert(!s.search_backspace(true) && st.query() == "東京"); // Fullscreen leaves Square for pause.
        s.input(NUVIO_BTN_SQUARE); assert(st.query() == "東");
        s.input(NUVIO_BTN_SQUARE); assert(st.query().empty());
        const auto empty_token = st.request().token;
        s.input(NUVIO_BTN_SQUARE); assert(st.request().token == empty_token && !st.take_request());
        st.query("query", 1);
        drawn_text.clear(); s.draw(1, .05f); assert(contains(T("Søker …")) == 1);
        st.tick(2);
        complete(st, {{item(Kind::Track, "t"), item(Kind::Artist, "ar"), item(Kind::Album, "al"), item(Kind::Playlist, "pl")}, ""});
        for (auto language : {i18n::Choice::English, i18n::Choice::Norwegian}) {
            i18n::set_choice(language); drawn_text.clear(); s.draw(2, .05f);
            if (language == i18n::Choice::English) assert(contains("Tracks") == 1 && !contains("Track"));
            std::vector<std::string> headings;
            for (const auto &text : drawn_text)
                if (text == T("Spor") || text == T("Artister") || text == T("Album") || text == T("Spillelister")) headings.push_back(text);
            assert(headings == (std::vector<std::string>{T("Spor"), T("Artister"), T("Album"), T("Spillelister")}));
        }
        s.input(NUVIO_BTN_RIGHT); // Main row edge to results.
        s.input(NUVIO_BTN_DOWN); s.input(NUVIO_BTN_CROSS);
        auto detail = st.take_request();
        assert(detail && detail->view == View::ArtistAlbums && detail->parent_id == "ar");
        Page artist_page{{item(Kind::Album, "artist-album")}, ""};
        artist_page.top_songs_ok = true;
        for (int i = 0; i < 6; ++i) {
            auto top = item(Kind::Track, "top"); top.title = "Top " + std::to_string(i);
            artist_page.top_songs.push_back(top);
        }
        assert(st.complete(detail->token, artist_page));
        drawn_text.clear(); s.draw(2, .05f);
        assert(contains("Top 0") && contains("Top 4") && !contains("Top 5"));
        assert(!s.search_backspace() && st.query().empty()); // Artist detail retains global pause.
        s.input(NUVIO_BTN_CROSS);
        auto play_artist = st.take_play_intent();
        assert(play_artist && play_artist->action == PlayAction::ArtistPlayAll);
        s.input(NUVIO_BTN_CIRCLE);
        assert(st.request().view == View::Search && st.query() == "query" && st.selected() == 1 && !st.take_request());
        s.input(NUVIO_BTN_SQUARE); assert(st.query() == "quer"); // Results focus also deletes.
        st.tick(2.1); assert(!st.take_request());
        st.tick(2.4); auto stale = st.take_request(); assert(stale);
        s.input(NUVIO_BTN_SQUARE); assert(st.query() == "que");
        assert(!st.complete(stale->token, {}));
        st.tick(3); complete(st, {{item(Kind::Artist, "only")}, ""});
        drawn_text.clear(); s.draw(3, .05f);
        assert(contains(T("Artister")) == 1 && !contains(T("Spor")) && !contains(T("Album")) && !contains(T("Spillelister")));
        st.query("none", 3); st.tick(4); complete(st, {});
        drawn_text.clear(); s.draw(4, .05f); assert(contains(T("Ingen treff")) == 1);
        st.query("error", 4); st.tick(5);
        auto failed = st.take_request(); assert(failed && st.complete(failed->token, {}, false));
        drawn_text.clear(); hint_calls.clear(); s.draw(5, .05f);
        assert(contains(T("Kunne ikke hente musikken")) == 1 && !contains(T("Cross: prøv igjen")));
        assert(std::any_of(hint_calls.begin(), hint_calls.end(), [](const auto &hint) {
            return hint.button == ui::PadButton::Cross && hint.label == T("Prøv igjen");
        }));
    }
    for (bool edited : {false, true}) {
        // Initial failure and edited-query failure both expose keyboard-reachable retry.
        ui::MusicScreen s; auto &st = s.state();
        s.activate(); complete(st, {}); s.input(NUVIO_BTN_L1); s.input(NUVIO_BTN_DOWN);
        assert(st.query("ab", 0)); st.tick(1);
        if (edited) {
            complete(st, {{item(Kind::Track, "old")}, ""});
            for (int i = 0; i < 7; ++i) s.input(NUVIO_BTN_RIGHT);
            assert(st.query("edited", 1)); st.tick(2);
            s.input(NUVIO_BTN_LEFT); // Old results focus back to keyboard.
        }
        const auto failed = st.take_request();
        assert(failed && failed->view == View::Search && st.complete(failed->token, {}, false));
        const auto query = st.query();
        for (int i = 0; i < 7; ++i) s.input(NUVIO_BTN_RIGHT);
        drops.clear(); s.draw(2, .05f);
        assert(drops.size() == 1 && drops.back().x == 800 && drops.back().h == 110);
        s.input(NUVIO_BTN_CROSS);
        const auto retry = st.take_request();
        assert(retry && retry->view == View::Search && retry->query == query && retry->cursor.empty());
        assert(retry->token != failed->token && st.query() == query && !st.take_play_intent());
        assert(!st.complete(failed->token, {{item(Kind::Track, "stale")}, ""}));
        assert(st.complete(retry->token, {{item(Kind::Track, "fresh")}, ""}));
        s.input(NUVIO_BTN_CROSS);
        const auto play = st.take_play_intent();
        assert(play && play->song_id == "fresh" && play->context == View::Search && play->context_id == query);
        s.input(NUVIO_BTN_LEFT); s.input(NUVIO_BTN_CROSS); // Keyboard still types normally.
        assert(st.query() == query + "g" && !st.take_request());
    }
    {
        // Main routing consumes Square before pause and screen input, including header focus.
        ui::MusicScreen s;
        s.activate(); complete(s.state(), {});
        s.input(NUVIO_BTN_L1); // Search header, not keyboard.
        auto &st = s.state(); st.query("東京æ", 0);
        uint32_t pressed = NUVIO_BTN_SQUARE;
        if ((pressed & NUVIO_BTN_SQUARE) && s.search_backspace(false)) pressed &= ~NUVIO_BTN_SQUARE;
        assert(!(pressed & NUVIO_BTN_SQUARE));
        s.input(pressed); assert(st.query() == "東京"); // One codepoint, never double-delete.
        drops.clear(); s.draw(0, .05f);
        assert(pill_selected == -1 && drops.size() == 1 && drops.back().y == 64);
        assert(!s.search_backspace(true) && st.query() == "東京"); // Fullscreen Square still pauses.
        for (int i = 0; i < 3; ++i) assert(s.input(NUVIO_BTN_UP).kind == ui::Action::None);
        s.input(NUVIO_BTN_R1); assert(st.request().view == View::Home);
        assert(!s.search_backspace(false)); // Other views retain global pause.
    }
    {
        // One-letter results retain four-group order; every row/drop stays reachable
        // above playback, paused playback and idle hints after scroll settles.
        ui::MusicScreen s; auto &st = s.state();
        s.activate(); complete(st, {}); s.input(NUVIO_BTN_L1); s.input(NUVIO_BTN_DOWN);
        s.input(NUVIO_BTN_CROSS); assert(st.query() == "a");
        st.tick(.29); assert(!st.take_request()); st.tick(.3);
        Page page;
        for (auto kind : {Kind::Track, Kind::Artist, Kind::Album, Kind::Playlist})
            for (int i = 0; i < 5; ++i) page.items.push_back(item(kind, "match"));
        complete(st, page);
        for (int i = 0; i < 7; ++i) s.input(NUVIO_BTN_RIGHT);
        for (int mode : {0, 1, 2}) {
            const bool live = mode != 2;
            s.session(live, mode == 1, "Title", "Artist", "");
            st.move(-int(st.selected()));
            for (size_t i = 0; i < page.items.size(); ++i) {
                if (i) s.input(NUVIO_BTN_DOWN);
                for (int frame = 0; frame < 120; ++frame) {
                    scissors.clear(); drops.clear(); s.draw(1, .05f);
                }
                assert(st.selected() == i && scissors.size() == 1 && drops.size() == 1);
                const auto &clip = scissors.back(); const auto &drop = drops.back();
                assert(clip.y == 278 && clip.h == (live ? 610 : 698));
                assert(drop.y >= clip.y - .01f && drop.y + drop.h <= clip.y + clip.h + .01f);
                assert(clip.y + clip.h < hints_y - 14);
                assert(hints_y + 14 < (live ? 950 : 1080));
            }
            s.input(NUVIO_BTN_LEFT); // Results back to keyboard; last key row remains clear.
            for (int i = 0; i < 5; ++i) s.input(NUVIO_BTN_DOWN);
            drops.clear(); s.draw(1, .05f);
            assert(drops.back().y + drops.back().h == 780 && drops.back().y + drops.back().h < hints_y - 14);
            s.input(NUVIO_BTN_RIGHT);
        }
    }
    ui::MusicScreen home;
    auto &hs = home.state();
    home.activate();
    Page shelves{{item(Kind::Album, "same"), item(Kind::Album, "other"), item(Kind::Album, "same")}, "", 0, {{0, 2, true}, {2, 1, true}}};
    complete(hs, shelves);
    home.session(true, false, "Live", "Artist", "");
    home.input(NUVIO_BTN_DOWN); // tabs to real session
    home.input(NUVIO_BTN_CROSS);
    assert(home.take_player_open() && !hs.take_play_intent() && !hs.take_request());
    home.session(true, true, "Live", "Artist", "");
    home.input(NUVIO_BTN_CROSS); // paused hero also requests existing player, never Play
    assert(home.take_player_open() && !hs.take_play_intent() && !hs.take_request());
    home.input(NUVIO_BTN_DOWN);
    home.input(NUVIO_BTN_RIGHT);
    assert(hs.selected() == 1);
    home.input(NUVIO_BTN_RIGHT); // short shelf boundary
    assert(hs.selected() == 1);
    home.input(NUVIO_BTN_DOWN); // clamp column for one-card shelf
    assert(hs.selected() == 2);
    home.draw(0, 0.05f);
    home.input(NUVIO_BTN_CROSS);
    const auto child = hs.take_request();
    assert(child && child->parent_id == "same");
    home.input(NUVIO_BTN_CIRCLE);
    assert(hs.selected() == 2 && hs.page().sections.size() == 2 && !hs.take_request());
    assert(!hs.complete(child->token, {}));
    home.input(NUVIO_BTN_LEFT);
    assert(hs.selected() == 2); // duplicate remains second shelf occurrence
    home.input(NUVIO_BTN_UP);
    assert(hs.selected() == 0);
    home.session(false, false, "Retained", "", "");
    home.input(NUVIO_BTN_UP); // no hero after session stops
    home.input(NUVIO_BTN_CROSS); // tabs to shelf
    assert(!home.take_player_open());

    {   // Six-card Home shelves keep their own sections and readable TV-safe geometry.
        ui::MusicScreen six_home;
        six_home.activate();
        Page six;
        for (int i = 0; i < 12; ++i) { six.items.push_back(item(Kind::Album, "home-card")); six.items.back().artwork_url = "cover"; }
        six.sections = {{0, 6, true}, {6, 6, true}};
        complete(six_home.state(), six);
        assert(six_home.state().status() == Status::Ready);
        six_home.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < 5; ++i) six_home.input(NUVIO_BTN_RIGHT);
        assert(six_home.state().selected() == 5);
        six_home.input(NUVIO_BTN_RIGHT); assert(six_home.state().selected() == 5);
        for (int mode : {0, 1, 2}) {
            six_home.session(mode != 0, mode == 2, "Live", "Artist", "");
            for (int i = 0; i < 100; ++i) six_home.draw(0, .05f);
            artwork_rects.clear(); drops.clear(); gfx::placed.clear();
            six_home.draw(0, .05f);
            const auto clip = scissors.back(), focus = drops.back();
            assert(clip.x == 96 && clip.y == 238 && clip.w == 1728 && clip.h == 634);
            assert(focus.x == 1530 && focus.w == 278);
            assert(focus.x + focus.w <= 1824 && focus.y >= clip.y && focus.y + focus.h <= clip.y + clip.h);
            int first_row = 0;
            for (const auto &r : artwork_rects) if (r.w == 224 && r.y == 294) {
                ++first_row;
                assert(r.x >= 96 && r.x + r.w <= 1824 && r.y + r.h < clip.y + clip.h);
            }
            assert(first_row == 6);
            for (const auto &p : gfx::placed) if (p.text == "Fixture") assert(p.size == 25);
        }
        six_home.input(NUVIO_BTN_DOWN); assert(six_home.state().selected() == 11);
        for (int i = 0; i < 100; ++i) six_home.draw(0, .05f);
        assert(drops.back().x == 1530 && drops.back().y >= scissors.back().y);
        six_home.input(NUVIO_BTN_UP); assert(six_home.state().selected() == 5);
    }
    ui::MusicScreen partial;
    partial.activate();
    Page failed{{item(Kind::Album, "kept")}, "", 0, {{0, 0, false}, {0, 1, true}}};
    complete(partial.state(), failed);
    partial.input(NUVIO_BTN_DOWN);
    partial.input(NUVIO_BTN_CROSS);
    auto retry = partial.state().take_request();
    assert(retry && retry->home_retry == 1);
    Page retried{{item(Kind::Album, "fresh")}, "", 0, {{0, 1, true}, {1, 0, false}}};
    assert(partial.state().complete(retry->token, retried));
    assert(partial.state().page().items[1].id == "kept");
    assert(partial.state().page().sections[1].ok);
    // Failed dispatch also preserves successful shelf.
    ui::MusicScreen dispatch;
    dispatch.activate(); complete(dispatch.state(), failed);
    dispatch.input(NUVIO_BTN_DOWN); dispatch.input(NUVIO_BTN_CROSS);
    auto dispatch_retry = dispatch.state().take_request();
    assert(dispatch_retry && dispatch.state().complete(dispatch_retry->token, {}, false));
    assert(dispatch.state().status() == Status::Ready && dispatch.state().page().items[0].id == "kept");
    partial.state().tab(1);
    const auto other_tab = partial.state().take_request();
    assert(other_tab && !partial.state().complete(retry->token, retried));
    ui::MusicScreen empty;
    empty.activate();
    complete(empty.state(), {{}, "", 0, {{0, 0, true}, {0, 0, false}}});
    empty.input(NUVIO_BTN_DOWN);
    empty.input(NUVIO_BTN_CROSS); // empty recent must not invent history or Play
    assert(!empty.state().take_play_intent() && !empty.state().take_request());
    empty.input(NUVIO_BTN_DOWN);
    empty.input(NUVIO_BTN_CROSS);
    auto empty_retry = empty.state().take_request();
    assert(empty_retry && empty_retry->home_retry == 2);
    assert(empty.state().complete(empty_retry->token, {{}, "", 0, {{0, 0, false}, {0, 0, true}}}));
    assert(empty.state().page().sections[0].ok && empty.state().page().items.empty());
    // Completion precedes input, not draw: shelf selection must rebase before open.
    ui::MusicScreen racing;
    racing.activate(); complete(racing.state(), failed);
    racing.input(NUVIO_BTN_DOWN); racing.input(NUVIO_BTN_CROSS);
    auto racing_retry = racing.state().take_request();
    racing.input(NUVIO_BTN_DOWN); // Newest while Recent retry is loading.
    assert(racing_retry && racing.state().complete(racing_retry->token, retried));
    racing.input(NUVIO_BTN_CROSS); // No draw between completion and activation.
    auto kept = racing.state().take_request();
    assert(kept && kept->parent_id == "kept");

    for (int tab : {1, 3}) {
        ui::MusicScreen library;
        auto &ls = library.state();
        library.activate(); complete(ls, {});
        for (int i = 0; i < tab; ++i) library.input(NUVIO_BTN_RIGHT);
        Page cards;
        for (int i = 0; i < 7; ++i) cards.items.push_back(item(tab == 1 ? Kind::Artist : Kind::Playlist, i == 6 ? "chosen" : "other"));
        complete(ls, cards);
        library.input(NUVIO_BTN_DOWN); library.input(NUVIO_BTN_RIGHT); library.input(NUVIO_BTN_DOWN);
        assert(ls.selected() == 6);
        library.input(NUVIO_BTN_CROSS);
        assert(ls.request().parent_id == "chosen");
        if (tab == 1) {
            assert(ls.request().view == View::ArtistAlbums);
            complete(ls, {{item(Kind::Album, "album")}, ""});
            library.input(NUVIO_BTN_CROSS); // Artist Play all, not album activation.
            auto artist_play = ls.take_play_intent();
            assert(artist_play && artist_play->action == PlayAction::ArtistPlayAll);
            assert(artist_play->context == View::ArtistAlbums && artist_play->context_id == "chosen" && artist_play->song_id.empty());
            library.input(NUVIO_BTN_RIGHT); library.input(NUVIO_BTN_CROSS);
            artist_play = ls.take_play_intent();
            assert(artist_play && artist_play->action == PlayAction::ArtistShuffle && artist_play->context_id == "chosen");
            library.input(NUVIO_BTN_DOWN); // Buttons to album grid.
            library.input(NUVIO_BTN_UP); // First row restores buttons.
            library.input(NUVIO_BTN_CROSS);
            artist_play = ls.take_play_intent();
            assert(artist_play && artist_play->action == PlayAction::ArtistPlayAll);
            library.input(NUVIO_BTN_DOWN); library.input(NUVIO_BTN_CROSS);
        }
        assert(ls.request().view == (tab == 1 ? View::AlbumTracks : View::PlaylistTracks));
        Page tracks{{item(Kind::Track, "first"), item(Kind::Track, "second")}, "40"};
        tracks.detail = {"Real title", "Creator", "cover", "Genre", 2026, 42, 3600};
        complete(ls, tracks);
        drawn_text.clear(); drawn_art.clear();
        library.draw(0, 0.05f);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Real title") != drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Creator · 2026 · Genre · 42 spor · 60:00") != drawn_text.end());
        assert(std::find(drawn_art.begin(), drawn_art.end(), "cover") != drawn_art.end());
        i18n::set_choice(i18n::Choice::English);
        drawn_text.clear(); library.draw(0, 0.05f);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Creator · 2026 · Genre · 42 tracks · 60:00") != drawn_text.end());
        i18n::set_choice(i18n::Choice::Norwegian);
        library.input(NUVIO_BTN_CROSS);
        auto whole = ls.take_play_intent();
        assert(whole && whole->song_id.empty() && whole->occurrence == 0);
        library.input(NUVIO_BTN_RIGHT); library.input(NUVIO_BTN_CROSS);
        auto shuffled = ls.take_play_intent();
        assert(shuffled && shuffled->action == PlayAction::DetailShuffle && shuffled->context == ls.request().view);
        drops.clear(); drawn_text.clear(); library.draw(0, .05f);
        assert(std::any_of(drops.begin(), drops.end(), [](gfx::Rect r) { return r.x == 882 && r.y == 372 && r.w == 88; }));
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Spill spilleliste")) == drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Spill album")) == drawn_text.end());
        library.input(NUVIO_BTN_LEFT);
        library.input(NUVIO_BTN_DOWN); library.input(NUVIO_BTN_DOWN); library.input(NUVIO_BTN_CROSS);
        auto row = ls.take_play_intent();
        assert(row && row->song_id == "second" && row->occurrence == 1);
        library.input(NUVIO_BTN_UP); library.input(NUVIO_BTN_UP); library.input(NUVIO_BTN_UP);
        assert(library.input(NUVIO_BTN_UP).kind == ui::Action::None);
        library.input(NUVIO_BTN_DOWN);
        library.input(NUVIO_BTN_DOWN); // Play album to first track.
        library.input(NUVIO_BTN_DOWN); // Last track.
        library.input(NUVIO_BTN_DOWN); // Past loaded tail: continuation, no pager control.
        const auto page2 = ls.take_request();
        assert(page2 && page2->cursor == "40");
        Page second{{item(Kind::Track, "last")}, "", 40}; second.detail = tracks.detail;
        assert(ls.complete(page2->token, second));
        assert(ls.page().detail.title == "Real title" && ls.page().items.size() == 3 && ls.selected() == 2);
        assert(!ls.complete(page2->token - 1, {}));
        library.input(NUVIO_BTN_CROSS);
        row = ls.take_play_intent();
        assert(row && row->song_id == "last" && row->occurrence == 2 && row->page_offset == 0);
        library.input(NUVIO_BTN_CIRCLE); // Back restores snapshot, never refetches.
        assert(!ls.take_request());
        if (tab == 1) {
            library.input(NUVIO_BTN_CROSS); // Back restores album grid, not artist Play all.
            assert(ls.request().view == View::AlbumTracks && !ls.take_play_intent());
            complete(ls, tracks);
            library.input(NUVIO_BTN_CIRCLE); library.input(NUVIO_BTN_CIRCLE);
            assert(!ls.take_request());
        }
        assert(ls.selected() == 6 && ls.request().view == (tab == 1 ? View::Artists : View::Playlists));
        library.input(NUVIO_BTN_CROSS);
        complete(ls, {}); // Empty detail/index remains navigable, never enabled Play.
        library.input(NUVIO_BTN_CROSS);
        assert(!ls.take_play_intent());
    }
    { // Artist songs: five then local More twenty; duplicate occurrence; albums below reachable.
        ui::MusicScreen artist; auto &s = artist.state();
        artist.activate(); complete(s, {});
        artist.input(NUVIO_BTN_RIGHT); complete(s, {{item(Kind::Artist, "artist")}, ""});
        artist.input(NUVIO_BTN_DOWN); artist.input(NUVIO_BTN_CROSS);
        Page page{{item(Kind::Album, "album")}, ""}; page.top_songs_ok = true;
        for (int i = 0; i < 20; ++i) page.top_songs.push_back(item(Kind::Track, "dup"));
        complete(s, page);
        artist.input(NUVIO_BTN_DOWN); // Play to first song.
        for (int i = 0; i < 4; ++i) artist.input(NUVIO_BTN_DOWN);
        artist.input(NUVIO_BTN_CROSS);
        auto play = s.take_play_intent();
        assert(play && play->occurrence == 4 && play->top_snapshot.size() == 20 && play->context == View::ArtistAlbums);
        artist.input(NUVIO_BTN_DOWN); artist.input(NUVIO_BTN_CROSS); // More, not playback/refetch.
        assert(!s.take_request() && !s.take_play_intent());
        artist.input(NUVIO_BTN_CROSS); play = s.take_play_intent();
        assert(play && play->occurrence == 5);
        for (int i = 0; i < 15; ++i) artist.input(NUVIO_BTN_DOWN);
        drawn_art.clear(); gfx::placed.clear(); artist.draw(0, 1.f);
        assert(std::none_of(gfx::placed.begin(), gfx::placed.end(), [](const auto &p) {
            return p.x == 112 && p.y == 287; // No misleading Albums subtitle above artist Top songs.
        }));
        artist.input(NUVIO_BTN_CROSS);
        assert(s.request().view == View::AlbumTracks && !s.take_play_intent());
    }
    for (int focus : {0, 4, 5, 12}) for (int phase : {0, 1, 2}) {
        // Album prefetch must preserve song, More and expanded-song focus.
        ui::MusicScreen artist; auto &st = artist.state();
        artist.activate(); complete(st, {});
        artist.input(NUVIO_BTN_R1); complete(st, {{item(Kind::Artist, "artist")}, ""});
        artist.input(NUVIO_BTN_DOWN); artist.input(NUVIO_BTN_CROSS);
        Page page{{item(Kind::Album, "album")}, "40"}; page.top_songs_ok = true;
        for (int i = 0; i < 20; ++i) {
            auto song = item(Kind::Track, "top"); song.title = "Song " + std::to_string(i);
            page.top_songs.push_back(song);
        }
        complete(st, page);
        artist.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < std::min(focus, 5); ++i) artist.input(NUVIO_BTN_DOWN);
        if (focus > 5) {
            artist.input(NUVIO_BTN_CROSS); // Expand; focus song 6.
            for (int i = 5; i < focus; ++i) artist.input(NUVIO_BTN_DOWN);
        }
        const auto context = st.context();
        artist.draw(0, .05f); // Real automatic album continuation.
        const auto next = st.take_request();
        assert(next && next->view == View::ArtistAlbums && next->parent_id == "artist" && next->cursor == "40");
        assert(st.context() == context);
        auto active = *next;
        if (phase == 2) {
            assert(st.complete(next->token, {}, false));
            artist.input(NUVIO_BTN_CROSS); // Retry tail, never start playback.
            assert(!st.take_play_intent());
            const auto retry = st.take_request();
            assert(retry && retry->cursor == "40" && retry->parent_id == "artist" && retry->token != next->token);
            assert(!st.complete(next->token, {})); active = *retry;
        }
        if (phase) assert(st.complete(active.token, {{item(Kind::Album, "appended")}, ""}));
        artist.input(NUVIO_BTN_CROSS);
        if (focus == 5) {
            assert(!st.take_play_intent() && !st.take_request()); // More expands locally.
            artist.input(NUVIO_BTN_CROSS);
        }
        const auto play = st.take_play_intent();
        assert(play && play->action == PlayAction::Detail && play->song_id == "top");
        assert(play->context == View::ArtistAlbums && play->context_id == "artist");
        assert(play->occurrence == size_t(focus) && play->top_snapshot.size() == 20);
        // Re-entering artist is new context: expansion/song focus must reset.
        artist.input(NUVIO_BTN_CIRCLE); artist.input(NUVIO_BTN_CROSS);
        complete(st, page); artist.input(NUVIO_BTN_CROSS);
        const auto all = st.take_play_intent();
        assert(all && all->action == PlayAction::ArtistPlayAll && all->song_id.empty());
        artist.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < 5; ++i) artist.input(NUVIO_BTN_DOWN);
        artist.input(NUVIO_BTN_CROSS);
        assert(!st.take_play_intent()); // New artist context starts collapsed at More.
    }
    { // Artist albums share six-column grid; songs/albums stay visible inside artist clip.
        ui::MusicScreen artist; auto &s = artist.state();
        artist.activate(); complete(s, {});
        artist.input(NUVIO_BTN_RIGHT); complete(s, {{item(Kind::Artist, "artist")}, ""});
        artist.input(NUVIO_BTN_DOWN); artist.input(NUVIO_BTN_CROSS);
        Page page; page.top_songs_ok = true;
        for (int i = 0; i < 8; ++i) page.items.push_back(item(Kind::Album, "album"));
        for (int i = 0; i < 20; ++i) page.top_songs.push_back(item(Kind::Track, "song"));
        complete(s, page);
        const auto settle = [&] {
            for (int i = 0; i < 100; ++i) artist.draw(0, .05f);
            drops.clear(); artwork_rects.clear(); artist.draw(0, .05f);
            const auto clip = scissors.back(), focus = drops.back();
            assert(clip.y == 384 && clip.h == 488);
            assert(focus.x >= 96 && focus.x + focus.w <= 1824 && focus.y >= clip.y && focus.y + focus.h <= clip.y + clip.h);
            return focus;
        };
        artist.input(NUVIO_BTN_DOWN); for (int i = 0; i < 5; ++i) artist.input(NUVIO_BTN_DOWN);
        artist.input(NUVIO_BTN_CROSS); // More: twenty songs, focus song 6.
        for (int i = 0; i < 14; ++i) artist.input(NUVIO_BTN_DOWN);
        assert(settle().h == 66); // Last of twenty songs visible.
        artist.input(NUVIO_BTN_DOWN); // Albums.
        for (int i = 0; i < 5; ++i) artist.input(NUVIO_BTN_RIGHT);
        assert(s.selected() == 5);
        artist.input(NUVIO_BTN_RIGHT); assert(s.selected() == 5); // Six-column edge.
        auto focus = settle();
        assert(focus.x == 1530 && focus.w == 278);
        int albums = 0;
        for (const auto &r : artwork_rects) if (r.w == 234) { ++albums; assert(r.x >= 96 && r.x + r.w <= 1824); }
        assert(albums >= 6);
        artist.input(NUVIO_BTN_DOWN); assert(s.selected() == 7); // Clamp to short row.
        focus = settle(); assert(focus.x == 100 + 286 && focus.w == 278);
        artist.input(NUVIO_BTN_UP); assert(s.selected() == 1);
        artist.input(NUVIO_BTN_UP); assert(!s.take_play_intent()); // Back to last song, no playback.
        assert(settle().h == 66);
    }
    {   // Filtered-empty artist batches: draw auto-continues, never a pager.
        ui::MusicScreen artists;
        auto &as = artists.state();
        artists.activate(); complete(as, {});
        artists.input(NUVIO_BTN_RIGHT);
        complete(as, {{}, "40"});
        artists.input(NUVIO_BTN_DOWN); // Tabs to empty content.
        drawn_text.clear(); artists.draw(0, 0.05f);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Henter flere …") != drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Neste side") == drawn_text.end());
        auto next = as.take_request();
        assert(next && next->view == View::Artists && next->cursor == "40");
        assert(as.complete(next->token, {{}, "80"}));
        artists.draw(0, 0.05f);
        next = as.take_request();
        assert(next && next->cursor == "80");
        assert(as.complete(next->token, {}, false)); // Retryable tail error, rows kept.
        drawn_text.clear(); artists.draw(0, 0.05f);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Kunne ikke hente flere.") != drawn_text.end());
        assert(!as.take_request()); // Draw never auto-retries a failure.
        artists.input(NUVIO_BTN_CROSS);
        next = as.take_request();
        assert(next && next->cursor == "80");
        assert(as.complete(next->token, {{item(Kind::Artist, "found")}, ""}));
        artists.input(NUVIO_BTN_CROSS);
        assert(as.request().view == View::ArtistAlbums && as.request().parent_id == "found");
    }
    ui::MusicScreen empty_list;
    empty_list.activate(); complete(empty_list.state(), {});
    empty_list.state().tab(3); complete(empty_list.state(), {{item(Kind::Playlist, "empty")}, ""});
    empty_list.input(NUVIO_BTN_DOWN); empty_list.input(NUVIO_BTN_CROSS);
    Page empty_detail; empty_detail.detail = {"Empty playlist", "Owner", "playlist-cover", "", 0, 0, 0};
    complete(empty_list.state(), empty_detail);
    drawn_text.clear(); drawn_art.clear(); empty_list.draw(0, 0.05f);
    assert(std::find(drawn_text.begin(), drawn_text.end(), "Empty playlist") != drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), "Owner · 0 spor") != drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), "Spill spilleliste") == drawn_text.end());
    assert(std::find(drawn_art.begin(), drawn_art.end(), "playlist-cover") != drawn_art.end());
    empty_list.input(NUVIO_BTN_CROSS); assert(!empty_list.state().take_play_intent());
    State cleared; cleared.activate();
    complete(cleared, {{item(Kind::Album, "album")}, ""}); cleared.open();
    Page named{{item(Kind::Track, "one")}, "40"}; named.detail.title = "Metadata"; complete(cleared, named);
    assert(cleared.page().detail.title == "Metadata" && cleared.next());
    const auto failure = cleared.take_request();
    assert(failure && cleared.complete(failure->token, named, false));
    assert(cleared.page().detail.title == "Metadata" && cleared.continuation_error()); // Partial stays truthful.
    cleared.retry(); const auto recovered = cleared.take_request();
    assert(recovered && !cleared.complete(failure->token, named));
    named.next_cursor.clear(); named.items = {item(Kind::Track, "two")};
    assert(cleared.complete(recovered->token, named) && cleared.page().items.size() == 2 && !cleared.continuation_error());
    ui::PlaybackPresentation presentation;
    presentation.title = "Playback"; presentation.position = 30; presentation.duration = 120;
    ui::Drop transport;
    fills.clear(); drawn_text.clear();
    ui::draw_music_player(presentation, 1, 1, .05f, transport);
    assert(std::find(drawn_text.begin(), drawn_text.end(), "0:30") != drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), "2:00") != drawn_text.end());
    assert(std::any_of(fills.begin(), fills.end(), [](gfx::Rect r) { return r.x == 120 && r.y == 992 && r.w == 420; }));
    for (auto language : {i18n::Choice::English, i18n::Choice::Norwegian}) {
        i18n::set_choice(language);
        for (bool paused : {false, true}) {
            presentation.paused = paused; presentation.playing = !paused;
            presentation.status = paused ? T("Pause") : T("Spiller");
            assert(std::string(ui::player_control_label(presentation, 1)) == (paused ? T("Fortsett") : T("Pause")));
            for (float a : {1.f, 0.f}) {
                drawn_text.clear();
                ui::draw_music_player(presentation, 1, a, .05f, transport);
                for (const char *label : {T("Spiller"), T("Pause"), T("Fortsett"), T("Forrige spor"), T("Neste spor")})
                    assert(std::find(drawn_text.begin(), drawn_text.end(), label) == drawn_text.end());
                assert(presentation.paused == paused && presentation.playing == !paused);
                presentation.error = true;
                drawn_text.clear();
                ui::draw_music_player(presentation, 1, a, .05f, transport);
                assert(std::find(drawn_text.begin(), drawn_text.end(), presentation.status) != drawn_text.end());
                presentation.error = false;
            }
        }
        presentation.paused = false; presentation.playing = true; presentation.status = T("Spiller");
        drawn_text.clear();
        ui::draw_music_player(presentation, 1, 1, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Spiller")) == drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Ned: søk   Circle: tilbake til musikken")) == drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Bland: av")) == drawn_text.end());
        presentation.shuffle_enabled = true;
        drawn_text.clear(); ui::draw_music_player(presentation, 4, 1, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Bland: på")) == drawn_text.end());
        presentation.shuffle_pending = true;
        drawn_text.clear(); ui::draw_music_player(presentation, 4, 1, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Bland: endrer …")) != drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Bland: på")) == drawn_text.end());
        presentation.shuffle_error = true;
        drawn_text.clear(); ui::draw_music_player(presentation, 1, 0, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Kunne ikke endre blanding. Prøv igjen.")) != drawn_text.end());
        presentation.shuffle_error = false;
        presentation.shuffle_pending = presentation.shuffle_enabled = false;
        drawn_text.clear(); ui::draw_music_player(presentation, 1, 0, .05f, transport);
        for (const char *time : {"0:30", "2:00"})
            assert(std::find(drawn_text.begin(), drawn_text.end(), time) == drawn_text.end());
        presentation.preview = true; presentation.target = 40;
        drawn_text.clear(); fills.clear(); ui::draw_music_player(presentation, 3, 0, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "0:40") != drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), "2:00") != drawn_text.end());
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Circle: avbryt søk")) == drawn_text.end());
        assert(hints.size() == 2 && hints[0].button == ui::PadButton::Cross && hints[0].label == T("Utfør søk"));
        assert(hints[1].button == ui::PadButton::Circle && hints[1].label == T("Avbryt søk") && hints_y == 1016);
        assert(std::any_of(fills.begin(), fills.end(), [](gfx::Rect r) { return r.y == 980 && r.w == 14; }));
        presentation.preview = false;
        presentation.status = T("Søker / bufrer …");
        drawn_text.clear(); ui::draw_music_player(presentation, 1, 0, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), presentation.status) != drawn_text.end());
        presentation.error = true; presentation.status = "Real error";
        drawn_text.clear(); ui::draw_music_player(presentation, 1, 0, .05f, transport);
        assert(std::find(drawn_text.begin(), drawn_text.end(), "Real error") != drawn_text.end());
        presentation.error = false;
    }
    // End scrub uses real preview clamp, then main's dual status/flag snapshot.
    // Previously identical text drew at y=930 and y=950 (23px font), overlapping.
    for (auto language : {i18n::Choice::English, i18n::Choice::Norwegian}) {
        i18n::set_choice(language);
        ui::PlayerState player;
        player.adjust(1, 119, 120);
        assert(player.preview && player.target < 120 && player.target > 119.99);
        presentation = {}; presentation.position = 119; presentation.duration = 120;
        presentation.preview = player.preview; presentation.target = player.target;
        hints.clear(); drawn_text.clear();
        ui::draw_music_player(presentation, 3, 0, .05f, transport);
        assert(hints.size() == 2 && hints[0].button == ui::PadButton::Cross && hints[1].button == ui::PadButton::Circle);
        for (const auto &hint : hints) {
            assert(hint.label.find("Cross:") == std::string::npos && hint.label.find("Circle:") == std::string::npos);
        }
        presentation.preview = false; presentation.position = player.target;
        presentation.seeking = true; presentation.status = T("Søker / bufrer …");
        presentation.shuffle_pending = presentation.shuffle_error = true;
        for (float a : {0.f, 1.f}) {
            drawn_text.clear(); gfx::placed.clear();
            ui::draw_music_player(presentation, 3, a, .05f, transport);
            assert(std::count(drawn_text.begin(), drawn_text.end(), presentation.status) == 1);
            const auto status = std::find_if(gfx::placed.begin(), gfx::placed.end(), [&](const auto &p) { return p.text == presentation.status; });
            assert(status != gfx::placed.end() && status->y == 950 && status->x == 1800 && status->max_w == 800);
            const auto shuffle = std::find_if(gfx::placed.begin(), gfx::placed.end(), [](const auto &p) { return p.text == T("Kunne ikke endre blanding. Prøv igjen."); });
            assert(shuffle != gfx::placed.end() && shuffle->y == 950 && shuffle->x + shuffle->max_w < status->x - status->max_w);
            assert(std::count(drawn_text.begin(), drawn_text.end(), T("Bland: endrer …")) == 0);
            presentation.seeking = false; presentation.seek_failed = true;
            presentation.status = T("Kunne ikke søke. Prøv igjen.");
            drawn_text.clear(); ui::draw_music_player(presentation, 3, a, .05f, transport);
            assert(std::count(drawn_text.begin(), drawn_text.end(), presentation.status) == 1);
            presentation.error = true; presentation.status = "Terminal error";
            drawn_text.clear(); ui::draw_music_player(presentation, 3, a, .05f, transport);
            assert(std::count(drawn_text.begin(), drawn_text.end(), "Terminal error") == 1);
            assert(std::count(drawn_text.begin(), drawn_text.end(), T("Kunne ikke søke. Prøv igjen.")) == 0);
            presentation.error = presentation.seek_failed = false;
            presentation.seeking = true; presentation.status = T("Søker / bufrer …");
        }
        // Natural handoff/EOF clears preview through existing identity sync, not rendering.
        player.sync(1, 2, true); assert(!player.preview);
        presentation = {}; presentation.status = T("Ferdig");
        drawn_text.clear(); ui::draw_music_player(presentation, 1, 1, .05f, transport);
        assert(std::count(drawn_text.begin(), drawn_text.end(), presentation.status) == 1);
    }
    // Idle slide follows caller's animated alpha; preview restores layout and times.
    float previous_y = 0;
    for (float a : {1.f, .75f, .5f, .25f, 0.f}) {
        presentation = {}; presentation.title = "Slide";
        gfx::placed.clear(); drops.clear(); fills.clear();
        ui::draw_music_player(presentation, 3, a, .05f, transport);
        const auto title = std::find_if(gfx::placed.begin(), gfx::placed.end(), [](const auto &p) { return p.text == "Slide"; });
        assert(title != gfx::placed.end() && title->y >= previous_y && title->y <= 801);
        previous_y = title->y;
        assert(drops.empty()); // Seek focus never creates full-width glass highlight.
        for (auto r : fills) assert(std::isfinite(r.x) && std::isfinite(r.y) && r.w >= 0 && r.h >= 0);
        assert(std::find(drawn_text.begin(), drawn_text.end(), T("Ned: søk")) == drawn_text.end());
    }
    // Shuffle uses only its original icon plus an enabled inset plate: no check/minus badges.
    for (auto language : {i18n::Choice::English, i18n::Choice::Norwegian}) {
        i18n::set_choice(language);
        for (bool enabled : {false, true}) {
            painted_fills.clear();
            ui::draw_music_icon(ui::MusicIcon::Shuffle, 500, 878, enabled ? 0xffa8bfa8 : ui::kText);
            const auto expected_icon = painted_fills;
            for (int focus : {1, 3, 4}) for (bool pending : {false, true}) for (bool error : {false, true}) {
                presentation = {}; presentation.shuffle_enabled = enabled;
                presentation.shuffle_pending = pending; presentation.shuffle_error = error;
                painted_fills.clear(); drops.clear(); drawn_text.clear();
                ui::draw_music_player(presentation, focus, 1, .05f, transport);
                std::vector<PaintedFill> shuffle;
                for (const auto &fill : painted_fills)
                    if (fill.rect.x >= 456 && fill.rect.x < 544 && fill.rect.y >= 838 && fill.rect.y < 918)
                        shuffle.push_back(fill);
                assert(shuffle.size() == expected_icon.size() + (enabled ? 1 : 0));
                if (enabled) {
                    const auto &plate = shuffle.front();
                    assert(plate.rect.x == 472 && plate.rect.y == 854 && plate.rect.w == 56 && plate.rect.h == 48);
                    assert(plate.color == 0xff354038 && plate.radius == 12); // Solid even without hue perception.
                    shuffle.erase(shuffle.begin());
                }
                for (size_t i = 0; i < expected_icon.size(); ++i) {
                    const auto &actual = shuffle[i]; const auto &expected = expected_icon[i];
                    assert(actual.rect.x == expected.rect.x && actual.rect.y == expected.rect.y &&
                           actual.rect.w == expected.rect.w && actual.rect.h == expected.rect.h);
                    assert(actual.color == expected.color && actual.radius == expected.radius);
                }
                if (focus == 3) assert(drops.empty());
                else {
                    assert(drops.size() == 1);
                    const auto &drop = drops.front();
                    assert(drop.x == (focus == 4 ? 456 : 232) && drop.y == 838 && drop.w == 88 && drop.h == 80);
                } // Focus geometry stays independent of enabled/pending/error state.
                assert(std::string(ui::player_control_label(presentation, 4)) ==
                       (pending ? T("Bland: endrer …") : enabled ? T("Bland: på") : T("Bland: av")));
                for (const char *label : {T("Bland: på"), T("Bland: av")})
                    assert(std::find(drawn_text.begin(), drawn_text.end(), label) == drawn_text.end());
                assert((std::find(drawn_text.begin(), drawn_text.end(), T("Bland: endrer …")) != drawn_text.end()) == (pending && !error));
                assert((std::find(drawn_text.begin(), drawn_text.end(), T("Kunne ikke endre blanding. Prøv igjen.")) != drawn_text.end()) == error);
            }
        }
    }
    presentation = {}; presentation.position = 30; presentation.duration = 120;
    presentation.preview = true; presentation.target = 60;
    fills.clear(); drops.clear(); gfx::placed.clear();
    ui::draw_music_player(presentation, 3, 0, .05f, transport);
    assert(drops.empty());
    assert(std::any_of(fills.begin(), fills.end(), [](gfx::Rect r) { return r.y == 980 && r.w == 14 && r.h == 30 && r.y + r.h <= 1026; }));
    i18n::set_choice(i18n::Choice::Norwegian);
    presentation.preview = false;
    presentation.duration = 0;
    fills.clear(); drawn_text.clear();
    ui::draw_music_player(presentation, 1, 0, .05f, transport);
    assert(std::find(drawn_text.begin(), drawn_text.end(), "--:--") == drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), "0:30") == drawn_text.end());
    assert(std::none_of(fills.begin(), fills.end(), [](gfx::Rect r) { return r.y == 992 && r.w != 1680; }));
    // Same snapshot field order as main's fullscreen wiring: paused transport,
    // status and occurrence-derived Up Next must reach presentation unchanged.
    int duration = 120; // Worker/main duration remains integer seconds.
    presentation = {"Track", "Artist", "cover", "portrait", T("Pause"), "Next", 110, static_cast<double>(duration), true, false, false};
    panels.clear(); drawn_text.clear();
    ui::draw_music_player(presentation, 1, 1, .05f, transport);
    assert(std::find(drawn_text.begin(), drawn_text.end(), T("Fortsett")) == drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), T("Pause")) == drawn_text.end());
    assert(panels.empty()); // Paused playback suppresses Up Next.
    presentation.paused = false; presentation.playing = true;
    panels.clear(); drawn_text.clear();
    ui::draw_music_player(presentation, 1, 1, .05f, transport);
    assert(std::find(drawn_text.begin(), drawn_text.end(), T("Pause")) == drawn_text.end());
    assert(std::find(drawn_text.begin(), drawn_text.end(), "Next") != drawn_text.end());
    assert(panels.size() == 1);
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e30}) {
        presentation.duration = invalid;
        panels.clear();
        ui::draw_music_player(presentation, 1, 1, .05f, transport);
        assert(panels.empty()); // Never narrow invalid duration into Up Next's int.
    }
    panels.clear(); gfx::placed.clear(); screen.draw(0, .05f);
    // Toolbar pill plus separate magnifier glass, right of and outside pill.
    assert(panels.size() == 2 && panels[0].y == 58 && panels[0].h == 76);
    assert(panels[1].y == 58 && panels[1].w == 76 && panels[1].x >= panels[0].x + panels[0].w + 8);
    assert(panels[1].x + panels[1].w <= 1824); // TV safe area.
    {   // Header: wordmark left of pill, same line, gap, inside 5% TV safe area.
        const auto brand = std::find_if(gfx::placed.begin(), gfx::placed.end(), [](const auto &p) { return p.text == "Navi5"; });
        assert(brand != gfx::placed.end());
        const gfx::Rect pill = panels[0];
        const float brand_right = brand->x + gfx::text_width("Navi5", {gfx::Bold, brand->size});
        assert(brand->x >= 96 && brand_right + 24 <= pill.x && pill.x - brand_right <= 48);
        assert(std::abs(brand->y - (pill.y + 48)) <= 2); // Pill label baseline: y + pad 6 + 42.
        assert(brand->y - brand->size * .75f >= 54 && pill.y >= 54 && pill.x + pill.w <= 1824);
        assert(pill.y + pill.h < 246 - 36); // Header clears unchanged heading/content.
    }
    ui::MusicScreen tabs;
    tabs.activate(); complete(tabs.state(), {});
    for (auto expected : {View::Search, View::Playlists, View::Albums, View::Artists, View::Home, View::Search}) {
        tabs.input(NUVIO_BTN_L1);
        assert(tabs.state().request().view == expected);
        if (auto r = tabs.state().take_request()) tabs.state().complete(r->token, {});
    }
    tabs.input(NUVIO_BTN_R1); assert(tabs.state().tab_index() == 0); complete(tabs.state(), {});
    tabs.input(NUVIO_BTN_R1); assert(tabs.state().tab_index() == 1);
    assert(!tabs.state().take_request() && tabs.state().page().items.empty());
    tabs.state() = State(); tabs.state().tab(1); // Fresh load for the paged fixture.
    Page paged;
    for (int i = 0; i < 7; ++i) paged.items.push_back(item(Kind::Artist, "artist"));
    paged.next_cursor = "40";
    complete(tabs.state(), paged);
    tabs.input(NUVIO_BTN_DOWN); tabs.input(NUVIO_BTN_DOWN); // Six columns: 0 to 6.
    assert(tabs.state().selected() == 6);
    for (int i = 0; i < 100; ++i) tabs.draw(0, .05f); // Settle scroll before checking focus clipping.
    assert(scissors.back().x == 96 && scissors.back().w == 1728 && scissors.back().y == 252);
    auto card = drops.back();
    assert(card.x >= 96 && card.x + card.w <= 1824 && card.y >= 252 && card.y + card.h <= 954);
    assert(std::none_of(hints.begin(), hints.end(), [](ui::PadHint h) { return h.button == ui::PadButton::L1 || h.button == ui::PadButton::R1; }));
    auto page_request = tabs.state().take_request(); // Draw prefetched near end.
    assert(page_request && page_request->cursor == "40" && page_request->view == View::Artists);
    Page tail{{}, "", 7};
    for (int i = 0; i < 8; ++i) tail.items.push_back(item(Kind::Artist, "tail"));
    assert(tabs.state().complete(page_request->token, tail));
    assert(tabs.state().selected() == 6);
    tabs.input(NUVIO_BTN_DOWN); assert(tabs.state().absolute() == 12); // Six-column row in one list.
    tabs.input(NUVIO_BTN_RIGHT); tabs.input(NUVIO_BTN_RIGHT); assert(tabs.state().absolute() == 14);
    tabs.input(NUVIO_BTN_DOWN); assert(tabs.state().absolute() == 14 && !tabs.state().take_request()); // End: bump only.
    panels.clear(); drawn_text.clear(); tabs.draw(0, .05f);
    for (const char *pager : {"Neste side", "Forrige side"})
        assert(std::find(drawn_text.begin(), drawn_text.end(), pager) == drawn_text.end());
    assert(panels.size() == 2 && panels[0].y == 58 && panels[1].y == 58); // Pill + magnifier; no pager panels.
    tabs.input(NUVIO_BTN_R1); // Shoulder changes top-level view.
    assert(tabs.state().tab_index() == 2 && tabs.state().request().view == View::Albums);
    complete(tabs.state(), {});
    tabs.input(NUVIO_BTN_L1); // Return restores Artists focus without refetch.
    assert(!tabs.state().take_request() && tabs.state().absolute() == 14);
    for (int tab : {1, 2, 3}) {
        ui::MusicScreen grid;
        grid.activate(); complete(grid.state(), {});
        grid.state().tab(tab);
        Page cards;
        for (int i = 0; i < 40; ++i) cards.items.push_back(item(tab == 1 ? Kind::Artist : tab == 2 ? Kind::Album : Kind::Playlist, "card"));
        cards.next_cursor = "40";
        complete(grid.state(), cards);
        grid.input(NUVIO_BTN_DOWN);
        for (int i = 0; i < 5; ++i) grid.input(NUVIO_BTN_RIGHT);
        assert(grid.state().selected() == 5);
        grid.input(NUVIO_BTN_RIGHT); assert(grid.state().selected() == 5); // No row wrap.
        grid.input(NUVIO_BTN_DOWN); assert(grid.state().selected() == 11);
        grid.input(NUVIO_BTN_DOWN); assert(grid.state().selected() == 17);
        std::vector<gfx::Rect> idle_artwork;
        gfx::Rect idle_focus{};
        for (int mode : {0, 1, 2, 0}) { // Empty/stopped, playing, paused, stopped with retained metadata.
            grid.session(mode != 0, mode == 2, "Retained", "Artist", "cover");
            assert(grid.active_session() == (mode != 0));
            for (int i = 0; i < 100; ++i) grid.draw(0, .05f);
            artwork_rects.clear(); gfx::placed.clear(); drops.clear(); panels.clear();
            grid.draw(0, .05f);
            const auto clip = scissors.back();
            assert(clip.x == 96 && clip.w == 1728 && clip.y == 252 && clip.h == 608);
            assert(hints_y == 922);
            assert(artwork_rects.size() == 12); // Six columns, two rows even before first playback.
            for (const auto &r : artwork_rects) {
                assert(r.x >= 96 && r.x + r.w <= 1824 && r.y >= clip.y && r.y + r.h <= clip.y + clip.h);
                assert(r.w == 234 && r.h == 234);
            }
            const auto focus = drops.back();
            assert(focus.w == 278 && focus.x == 1530);
            assert(focus.y >= clip.y && focus.y + focus.h <= clip.y + clip.h);
            const auto same_rect = [](const gfx::Rect &a, const gfx::Rect &b) {
                return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
            };
            if (idle_artwork.empty()) { idle_artwork = artwork_rects; idle_focus = focus; }
            assert(std::equal(idle_artwork.begin(), idle_artwork.end(), artwork_rects.begin(), same_rect));
            assert(same_rect(idle_focus, focus)); // Selected third row scrolls identically in every session state.
            for (const auto &p : gfx::placed) if (p.text == "Fixture") {
                assert(p.size == 25 && p.y >= clip.y + 25 && p.y + 6 <= clip.y + clip.h);
            }
            for (const auto &panel : panels) assert(panel.y == 58); // Toolbar/magnifier only; no pager panels.
        }
        grid.input(NUVIO_BTN_CROSS); // Detail, then Back restores column/row.
        complete(grid.state(), {{item(Kind::Track, "track")}, ""});
        grid.input(NUVIO_BTN_CIRCLE); // Restores window without refetch.
        assert(!grid.state().take_request() && grid.state().selected() == 17);
        for (int i = 0; i < 3; ++i) grid.input(NUVIO_BTN_DOWN);
        assert(grid.state().absolute() == 35);
        grid.input(NUVIO_BTN_DOWN); // Past loaded rows: one continuation, focus waits.
        const auto next = grid.state().take_request();
        assert(next && next->cursor == "40" && !grid.state().take_request() && grid.state().absolute() == 35);
        Page last{{item(tab == 1 ? Kind::Artist : tab == 2 ? Kind::Album : Kind::Playlist, "last")}, ""};
        assert(grid.state().complete(next->token, last));
        assert(grid.state().absolute() == 40); // Partial final row reachable, clamped.
        for (int i = 0; i < 100; ++i) grid.draw(0, .05f);
        const auto focus = drops.back(), clip = scissors.back();
        assert(focus.x == 100 + 4 * 286 && focus.y >= clip.y && focus.y + focus.h <= clip.y + clip.h);
        assert(!grid.state().take_request());
    }
    i18n::set_choice(i18n::Choice::English);
    assert(std::string(T("Fullskjermspiller")) == "Fullscreen player");
    std::cout << "PASS: MusicScreen cards, detail Play/rows/tabs/Back, empty and stale metadata; Home retry activation\n";
}
