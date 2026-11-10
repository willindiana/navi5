/* Navi5 — music browsing state, independent of renderer.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ui::music {

enum class View { Home, Artists, Albums, Playlists, ArtistAlbums, AlbumTracks, PlaylistTracks, Search };
enum class Kind { Artist, Album, Playlist, Track };
enum class Status { Idle, Loading, Ready, Error };
struct Item {
    Kind kind = Kind::Album;
    std::string id, title, subtitle, artwork_url;
    int duration = 0;
    std::string artist_id = {}; // Song metadata, never inferred from display name.
    int count = -1; // Supplied album/song count; -1 means absent.
};
struct Request {
    uint64_t token = 0;
    View view = View::Home;
    std::string parent_id, cursor;
    unsigned limit = 40;
    unsigned home_retry = 0; // Bit per failed shelf; zero loads both.
    std::string query;
    static bool valid_query(const std::string &);
};
inline bool Request::valid_query(const std::string &s)
{
    if (s.size() > 128) return false;
    // Validate UTF-8 at UI/adapter trust boundary, including overlongs/surrogates.
    for (size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 0x80) { if (c < 32 || c == 127) return false; continue; }
        unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!n || n > s.size() - i) return false;
        unsigned value = c & ((1u << (6 - n)) - 1);
        for (unsigned j = 0; j < n; ++j) {
            unsigned next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
// Home shelves retain independent empty/error states; indices address flat items.
struct Section {
    size_t begin = 0, count = 0;
    bool ok = true;
};
struct Detail {
    std::string title, creator, artwork_url, genre;
    int year = 0, count = -1, duration = 0;
};
struct Page {
    std::vector<Item> items;
    std::string next_cursor;
    size_t start_index = 0; // Absolute occurrence in full detail order, supplied by adapter.
    std::vector<Section> sections = {}; // Home: explicit recent contexts, newest albums (six each).
    Detail detail = {}; // Cached with ordered detail rows, never inferred from current page.
    std::vector<Item> top_songs = {}; // Artist: server-ranked snapshot, at most 20.
    bool top_songs_ok = false;
};
// Empty song_id means ordered detail Play; parent loads full album/playlist off UI thread.
// occurrence is the absolute row index in context (duplicate IDs are legal);
// worker verifies song_id there plus visible page IDs at page_offset.
enum class PlayAction { Detail, ArtistPlayAll, ArtistShuffle, DetailShuffle };
struct PlayIntent {
    std::string song_id, context_id, title;
    View context = View::AlbumTracks;
    size_t occurrence = 0, page_offset = 0;
    std::vector<std::string> page_ids;
    PlayAction action = PlayAction::Detail;
    std::vector<Item> top_snapshot = {}; // Exact ranked occurrences shown by artist page.
};

/* Single UI-thread owner. Parent drains requests and dispatches work elsewhere;
 * completed pages return on UI thread. Tokens reject stale results. No I/O here.
 * Cursor is opaque to UI, including for APIs whose adapter uses numeric offsets. */
class State {
public:
    static constexpr unsigned page_limit = 40;
    void activate();
    void history_changed();
    void search();
    bool query(const std::string &, double now);
    void tick(double now);
    const std::string &query() const { return frame_.request.query; }
    void tab(int index);
    bool move(int delta);
    void open();
    void play_album(bool shuffle = false);
    void play_top_song(size_t occurrence);
    void play_artist(bool shuffle) {
        if (status_ != Status::Ready || frame_.request.view != View::ArtistAlbums ||
            frame_.request.parent_id.empty() || page_.items.empty()) return;
        PlayIntent value;
        value.context = View::ArtistAlbums;
        value.context_id = frame_.request.parent_id;
        value.title = frame_.title;
        value.action = shuffle ? PlayAction::ArtistShuffle : PlayAction::ArtistPlayAll;
        play_ = std::move(value);
    }
    bool back();
    bool next();
    bool previous();
    void retry();
    bool complete(uint64_t token, Page page, bool success = true);
    std::optional<Request> take_request();
    std::optional<PlayIntent> take_play_intent();

    int tab_index() const { return tab_; }
    size_t selected() const { return frame_.selected; }
    const Page &page() const { return page_; }
    const Request &request() const { return frame_.request; }
    const std::string &title() const { return frame_.title; }
    Status status() const { return status_; }
    bool has_previous() const { return frame_.first_page > 0; }
    // Root grids call near their viewport end; no network on renderer thread.
    void approach(unsigned distance = 18);
    bool busy() const { return loading_; }
    // Changes on fresh load, Back or tab restore; not on continuation.
    uint64_t context() const { return context_; }
    size_t absolute() const { return page_.start_index + frame_.selected; }
    bool continuation_error() const { return continuation_error_; }
    bool at_ceiling() const { return frame_.last_page + 1 >= checkpoint_limit && !page_.next_cursor.empty(); }
    // ponytail: <=240 resident rows plus <=4096 cursor checkpoints (~163k albums at
    // 40/page). Ceiling is shown, never silent; upgrade: persisted checkpoint index.
    static constexpr size_t window_items = 240, checkpoint_limit = 4096;
private:
    struct Frame {
        Request request;
        std::string title;
        size_t selected = 0, offset = 0;
        struct Checkpoint { std::string cursor; size_t offset = 0, count = 0; };
        std::vector<Checkpoint> checkpoints;
        size_t first_page = 0, last_page = 0;
        std::vector<size_t> counts;
        Page home_page;
        bool history_stale = false;
        bool restore = false;
    } frame_;
    std::vector<Frame> parents_;
    std::array<std::optional<Frame>, 4> tabs_;
    Page page_, retry_page_;
    Status status_ = Status::Idle;
    int tab_ = 0;
    uint64_t serial_ = 0, context_ = 0;
    double search_due_ = -1;
    std::optional<Request> pending_;
    std::optional<PlayIntent> play_;
    bool loading_ = false, continuation_error_ = false;
    int direction_ = 0;
    std::optional<size_t> desired_;
    void load();
    void extend(int direction);
    PlayIntent intent(std::string song_id, size_t occurrence) const;
};
} // namespace ui::music
