/* Navi5 — memory-only console smoke session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "app/server_url.h"
#include "app/recent_contexts.h"
#include "app/audio_player.h"
#include "app/play_queue.h"
#include "jf/curl_diag.h"
#include "subsonic/subsonic.h"
#include "ui/music_state.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

namespace app::smoke {
// Reject URL credentials, queries, fragments, whitespace and ambiguous authorities.
bool valid_server(const std::string &server);
enum class ServerInput { Valid, NeedsScheme, Invalid };
ServerInput prepare_server(std::string &server);
const char *login_failure_key(subsonic::LoginFailure failure);
class Library {
public:
    // Optional stale check runs between multi-request scans; true abandons the page.
    bool load(const subsonic::Client &, const ui::music::Request &, ui::music::Page &,
              const std::function<bool()> &stale = {},
              const std::vector<ui::music::Item> &recent = {});
    ui::music::Item context; // Last successfully loaded explicit queue; raw cover ID only.
    bool queue(const subsonic::Client &, const ui::music::PlayIntent &, PlayQueue &,
               const std::function<bool()> &stale = {}, std::string *error = nullptr);
private:
    std::map<std::string, ui::music::Page> cache_;
    std::optional<std::vector<ui::music::Item>> playlists_;
};
// Previous restarts current occurrence (seek to 0) once decoded position exceeds this;
// at or below it goes to previous entry. First entry always restarts.
constexpr double previous_restart_seconds = 3.0;
struct Command {
    enum Kind { Login, Browse, Play, Pause, Resume, Stop, Next, Previous, Seek, ShuffleToggle, Logout } kind = Logout;
    uint64_t generation = 0, playback_revision = 0;
    uint64_t track = 0; // Seek occurrence from displayed AudioStatus, not song ID.
    double seconds = 0;
    bool shuffle_enabled = false; // Shuffle target, guarded by playback_revision.
    // RAM only. API key wins; never retain a password fallback alongside it.
    std::string server, username, password, api_key;
    ui::music::PlayIntent play;
    ui::music::Request request;
};
struct Result {
    uint64_t generation = 0;
    Command::Kind kind = Command::Logout;
    bool ok = false;
    subsonic::LoginFailure failure = subsonic::LoginFailure::None;
    jf::CurlDiag diagnostic;
    uint64_t token = 0;
    ui::music::Page page;
    std::string playback_error; // Local fixed diagnostics only; never transport/server text.
};
// Audio admission epoch. reset() bumps value and cancels the admitted track under
// this lock; worker checks value and starts under it after stopping unlocked. So
// after reset() returns no track of an old account starts, and any admitted one
// stops before its next decode/output block. Lock order: Mailbox::mutex, then this.
struct AudioEpoch {
    std::mutex mutex;
    uint64_t value = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
};
struct Mailbox {
    std::mutex mutex;
    std::condition_variable wake, browse_wake;
    std::shared_ptr<AudioEpoch> epoch = std::make_shared<AudioEpoch>();
    std::shared_ptr<const subsonic::Client> browse_client;
    std::vector<ui::music::Item> recent_contexts;
    uint64_t history_revision = 0;
    std::deque<Command> browse_commands;
    struct FirstPage {
        ui::music::Page page;
        std::chrono::steady_clock::time_point expires;
    };
    // Four root pages only; <=40 rows (Home <=10), <=256 KiB metadata each, 60s TTL.
    std::map<ui::music::View, FirstPage> first_pages;
    unsigned warm_attempted = 0;
    bool warm_ready = false; // Initial foreground Home must finish before speculation.
    uint64_t browse_revision = 0;
    bool cached(const ui::music::Request &, ui::music::Page &, uint64_t current);
    uint64_t generation = 0, playback_revision = 0;
    bool quit = false;
    std::deque<Command> commands;
    std::deque<Result> results;
    uint64_t audio_revision = 0; // Revision of published audio snapshot, not queued controls.
    AudioStatus audio;
    std::string song_id, title, artist, artwork_url, next_title;
    std::string artist_id, portrait_url;
    int duration = 0;
    bool shuffle_enabled = false, shuffle_pending = false, shuffle_error = false;
    // UI only. Invalidates pending/in-flight account results immediately.
    uint64_t reset();
    bool send(Command command);
};
// Render-owned values, never references into the worker's mailbox. Contention
// keeps the entire last publication; account reset must invalidate immediately.
struct PlaybackSnapshot {
    uint64_t generation = 0, revision = 0, history_revision = 0;
    AudioStatus audio;
    std::string song_id, title, artist, artwork_url, portrait_url, next_title;
    int duration = 0;
    bool shuffle_enabled = false, shuffle_pending = false, shuffle_error = false;

    void invalidate(uint64_t current) {
        if (generation != current) *this = {};
    }
    bool poll(Mailbox &box, uint64_t current, std::deque<Result> &results, bool try_only) {
        invalidate(current);
        std::unique_lock<std::mutex> lock(box.mutex, std::defer_lock);
        if (try_only) { if (!lock.try_lock()) return false; }
        else lock.lock();
        results.swap(box.results);
        if (box.generation != current) { *this = {}; return true; }
        generation = box.generation; revision = box.audio_revision; history_revision = box.history_revision;
        audio = box.audio; song_id = box.song_id; title = box.title;
        artist = box.artist; artwork_url = box.artwork_url; portrait_url = box.portrait_url;
        next_title = box.next_title; duration = box.duration;
        shuffle_enabled = box.shuffle_enabled; shuffle_pending = box.shuffle_pending; shuffle_error = box.shuffle_error;
        return true;
    }
};
// Detached worker retains mailbox, never UI pointers. Player dies on worker.
// Optional injected player for host worker regression tests; worker owns lifetime.
bool start_worker(const std::shared_ptr<Mailbox> &, subsonic::HttpGet,
                  std::shared_ptr<AudioPlayer> player = {});
} // namespace app::smoke
