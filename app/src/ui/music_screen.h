/* Navi5 — glass music home/library.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ui/music_state.h"
#include "ui/screen.h"

namespace ui {
// UI-thread snapshot only; caller retains audio commands, idle/wake and focus.
struct PlaybackPresentation {
    std::string title, artist, artwork, portrait, status, next_title;
    double position = 0, duration = 0;
    bool paused = false, playing = false, error = false;
    bool seekable = false, seeking = false, seek_failed = false, preview = false;
    double target = 0;
    bool shuffle_enabled = false, shuffle_pending = false, shuffle_error = false;
};
enum class MusicIcon { Previous, Play, Pause, Next, Shuffle };
// GPU geometry, centered at (x, y); caller owns focus and localized action hints.
void draw_music_icon(MusicIcon icon, float x, float y, uint32_t color);
// Localized semantic labels for icon-only fullscreen controls (focus 3 = seek).
const char *player_control_label(const PlaybackPresentation &playback, int focus);
void draw_mini_player_progress(double position, double duration);
void draw_music_player(const PlaybackPresentation &playback, int focus, float controls_alpha,
                       float dt, Drop &transport_drop);
class MusicScreen final : public Screen {
public:
    void activate() override { state_.activate(); }
    Action input(uint32_t pressed) override;
    // Consume Square throughout Search, but never while fullscreen owns input.
    bool search_backspace(bool fullscreen = false);
    void draw(double now, float dt) override;
    bool animating() const override { return animating_; }
    float nav_alpha() const override { return 1.f; }
    bool active_session() const { return live_; }
    void enter_from_top() override { tabs_focused_ = true; }
    /* UI-thread access only. Drain take_request(), run loader on worker, then
     * marshal complete() to UI thread. No callbacks retain screen lifetime. */
    music::State &state() { return state_; }
    void session(bool live, bool paused, const std::string &title, const std::string &artist, const std::string &art) {
        live_ = live; paused_ = paused; session_title_ = title; session_artist_ = artist; session_art_ = art;
        if (!live_ && home_row_ < 0) home_row_ = 0;
    }
    // Presentation/resume only: never creates a replacement Play intent.
    bool take_player_open() { bool value = open_player_; open_player_ = false; return value; }
private:
    music::State state_;
    bool live_ = false, paused_ = false, open_player_ = false;
    std::string session_title_, session_artist_, session_art_;
    int home_row_ = 0; // -1: live hero; 0/1: bounded shelves, including empty/error.
    size_t home_column_ = 0;
    float home_scroll_ = 0;
    void select_home();
    bool tabs_focused_ = true, animating_ = false, album_play_ = true, album_shuffle_ = false;
    Drop tabs_drop_, row_drop_, play_drop_;
    Anim scroll_;
    uint64_t drawn_token_ = 0; // State::context(), not request token.
    bool snap_ = false;
    bool search_button_ = false, search_results_ = false;
    int search_key_ = 0;
    bool search_symbols_ = false;
    double search_now_ = 0;
    Action search_input(uint32_t pressed);
    void draw_search(float dt);
    int artist_action_ = 0; // -1: albums, 0/1: Play/Shuffle, 2: songs, 3: More.
    size_t artist_song_ = 0;
    bool artist_expanded_ = false;
    uint64_t artist_context_ = 0; // Continuation/retry tokens do not change artist focus.
};
} // namespace ui
