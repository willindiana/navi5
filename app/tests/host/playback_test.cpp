/* Navi5 — offline worker orchestration checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/smoke_session.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>

// Deterministic backend: no network, decoder or console. Exercise real worker,
// library, UI intent and PlayQueue, including synchronous start failure.
namespace {
std::mutex audio_mutex;
app::AudioStatus audio;
std::vector<std::string> starts;
std::string prepared;
bool fail_start = false, destroyed = false, active = false, output_ready = false;
std::condition_variable audio_wake;
bool hold_start = false, start_entered = false, hold_stop = false, stop_entered = false;
size_t stops = 0;
void terminal(app::AudioState state) {
    std::lock_guard<std::mutex> lock(audio_mutex); audio.state = state;
    if (state == app::AudioState::ended && !prepared.empty()) {
        if (fail_start) { audio.state = app::AudioState::error; audio.error_track = audio.track + 1; }
        else { audio.state = app::AudioState::playing; ++audio.track; audio.next_track = audio.track; }
        prepared.clear();
    }
    active = audio.state == app::AudioState::playing;
}
template<class F> void until(F f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
namespace app {
AudioPlayer::AudioPlayer(AudioSink *) {}
bool AudioPlayer::seek(double, uint64_t) { assert(false); return false; }
bool AudioPlayer::replace_next(std::string, uint64_t) { assert(false); return false; }
std::shared_ptr<std::atomic<bool>> AudioPlayer::cancel_flag() { return std::make_shared<std::atomic<bool>>(false); }
AudioPlayer::~AudioPlayer() { std::lock_guard<std::mutex> lock(audio_mutex); destroyed = true; }
bool AudioPlayer::queue_next(std::string url, uint64_t track) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (track != audio.next_track || !prepared.empty()) return false;
    prepared = std::move(url); return true;
}
bool AudioPlayer::start(std::string url, std::string next) {
    stop();
    std::unique_lock<std::mutex> lock(audio_mutex);
    starts.push_back(std::move(url)); prepared = std::move(next);
    active = true; output_ready = false;
    audio = {AudioState::opening, 0, {}};
    start_entered = true; audio_wake.notify_all();
    audio_wake.wait(lock, [] { return !hold_start; });
    active = !fail_start; output_ready = active;
    audio = {fail_start ? AudioState::error : AudioState::playing, 0, {}};
    return !fail_start;
}
void AudioPlayer::stop() {
    std::unique_lock<std::mutex> lock(audio_mutex);
    active = false; output_ready = false; audio = {}; prepared.clear(); ++stops;
    stop_entered = true; audio_wake.notify_all();
    audio_wake.wait(lock, [] { return !hold_stop; });
}
void AudioPlayer::pause() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (active) audio.state = AudioState::paused;
}
void AudioPlayer::resume() {
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (audio.state == AudioState::paused)
        audio.state = output_ready ? AudioState::playing : AudioState::opening;
}
AudioStatus AudioPlayer::status() const { std::lock_guard<std::mutex> lock(audio_mutex); return audio; }
}
int main() {
    using namespace app::smoke;
    using namespace ui::music;
    // Opening pause/resume mirrors output-not-ready guard before decoder opens.
    {
        app::AudioPlayer player;
        { std::lock_guard<std::mutex> lock(audio_mutex); hold_start = true; }
        std::thread opening([&] { assert(player.start("fixture")); });
        until([] { std::lock_guard<std::mutex> lock(audio_mutex); return start_entered; });
        player.pause(); assert(player.status().state == app::AudioState::paused);
        player.resume(); assert(player.status().state == app::AudioState::opening);
        { std::lock_guard<std::mutex> lock(audio_mutex); hold_start = false; audio_wake.notify_all(); }
        opening.join(); player.stop();
    }
    { std::lock_guard<std::mutex> lock(audio_mutex); destroyed = false; }
    auto box = std::make_shared<Mailbox>();
    std::mutex gate; std::condition_variable wake; bool entered = false, release = false;
    auto http = [&](const std::string &url) {
        if (url.find("id=held") != std::string::npos) {
            std::unique_lock<std::mutex> lock(gate); entered = true; wake.notify_one();
            wake.wait(lock, [&] { return release; });
        }
        std::string detail;
        if (url.find("getAlbum?") != std::string::npos || url.find("getPlaylist?") != std::string::npos) {
            const bool album = url.find("getAlbum?") != std::string::npos;
            detail = std::string(",\"") + (album ? "album" : "playlist") + "\":{\"id\":\"context\",\"" + (album ? "song" : "entry") + "\":[";
            for (int i = 0; i < 43; ++i) {
                if (i) detail += ',';
                detail += "{\"id\":\"" + std::string(i == 40 || i == 41 ? "duplicate" : std::to_string(i)) + "\",\"title\":\"Track " + std::to_string(i) + "\"}";
            }
            detail += "]}";
        }
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"" + detail + "}}", {}};
    };
    assert(start_worker(box, http));
    auto send = [&](Command c) { c.generation = box->generation; assert(box->send(std::move(c))); };
    Command login; login.kind = Command::Login; login.server = "https://example.invalid"; send(login);
    until([&] { std::lock_guard<std::mutex> lock(box->mutex); return !box->results.empty(); });
    auto count = [&] { std::lock_guard<std::mutex> lock(audio_mutex); return starts.size(); };
    auto wait_status = [&](app::AudioState state, const std::string &title) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        until([&] {
            std::lock_guard<std::mutex> lock(box->mutex);
            if (std::chrono::steady_clock::now() > deadline - std::chrono::milliseconds(10))
                std::cerr << "expected " << int(state) << ' ' << title << "; got " << int(box->audio.state) << ' ' << box->title << '\n';
            return box->audio.state == state && box->title == title;
        });
    };
    uint64_t barrier_token = 0;
    auto barrier = [&] {
        Command c; c.kind = Command::Browse; c.request.token = ++barrier_token; send(c);
        until([&] {
            std::lock_guard<std::mutex> lock(box->mutex);
            for (const auto &result : box->results)
                if (result.kind == Command::Browse && result.token == barrier_token) return true;
            return false;
        });
    };
    auto terminal_controls = [&](app::AudioState state, const std::string &title) {
        const size_t before = count();
        Command c; c.kind = Command::Pause; send(c); barrier(); wait_status(state, title);
        c.kind = Command::Resume; send(c); barrier(); wait_status(state, title);
        assert(count() == before);
    };
    terminal_controls(app::AudioState::stopped, "");
    for (View view : {View::AlbumTracks, View::PlaylistTracks}) {
        // Real adapter supplies absolute occurrence on second page; UI carries it.
        subsonic::Client client("https://example.invalid", "fixture", "fixture", http);
        Library library; Request r; r.view = view; r.parent_id = "context"; r.cursor = "40";
        Page page; assert(library.load(client, r, page) && page.start_index == 40);
        State state; state.activate(); auto request = state.take_request();
        Page home; home.items.push_back({view == View::AlbumTracks ? Kind::Album : Kind::Playlist, "context", "Context", {}, {}, 0});
        if (view == View::PlaylistTracks) { state.tab(3); request = state.take_request(); }
        assert(state.complete(request->token, home)); state.open(); request = state.take_request();
        Page first; r.cursor.clear(); assert(library.load(client, r, first) && first.next_cursor == "40");
        assert(state.complete(request->token, first) && state.next()); request = state.take_request();
        assert(request->cursor == "40"); // Continuation appends into one scrolling list.
        assert(state.complete(request->token, page)); assert(state.move(41)); state.open();
        auto intent = state.take_play_intent(); assert(intent && intent->occurrence == 41 && intent->song_id == "duplicate");
        Command play; play.kind = Command::Play; play.play = *intent;
        size_t before = count(); send(play); wait_status(app::AudioState::playing, "Track 41"); assert(count() == before + 1);
        terminal(app::AudioState::ended); wait_status(app::AudioState::playing, "Track 42"); assert(count() == before + 1);
        terminal(app::AudioState::ended); wait_status(app::AudioState::ended, "Track 42");
        terminal_controls(app::AudioState::ended, "Track 42");
        std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before + 1);
        send(play); wait_status(app::AudioState::playing, "Track 41");
        terminal(app::AudioState::error); wait_status(app::AudioState::error, "Track 41");
        terminal_controls(app::AudioState::error, "Track 41");
        before = count(); std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before);
        { std::lock_guard<std::mutex> lock(audio_mutex); fail_start = true; }
        send(play); wait_status(app::AudioState::error, "Track 41");
        until([&] { return count() == before + 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before + 1);
        { std::lock_guard<std::mutex> lock(audio_mutex); fail_start = false; }
        // Failed automatic replacement consumes EOF, preserves attempted title,
        // and never skips ahead on later worker ticks.
        send(play); wait_status(app::AudioState::playing, "Track 41");
        { std::lock_guard<std::mutex> lock(audio_mutex); fail_start = true; }
        terminal(app::AudioState::ended);
        wait_status(app::AudioState::error, "Track 42"); before = count();
        std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before);
        { std::lock_guard<std::mutex> lock(audio_mutex); fail_start = false; }
        // First duplicate advances to second duplicate, not first ID match.
        play.play.occurrence = 40; send(play); wait_status(app::AudioState::playing, "Track 40");
        Command pause; pause.kind = Command::Pause; send(pause);
        wait_status(app::AudioState::paused, "Track 40"); before = count();
        std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before);
        pause.kind = Command::Resume; send(pause); wait_status(app::AudioState::playing, "Track 40");
        terminal(app::AudioState::ended); wait_status(app::AudioState::playing, "Track 41"); assert(count() == before);
        // Invalid occurrence replaces old audio and cannot retain old title/queue.
        play.play.occurrence = 500; send(play); wait_status(app::AudioState::error, "");
        play.play = *intent; send(play); wait_status(app::AudioState::playing, "Track 41");
        // Pending Pause at natural EOF cannot resurrect inactive backend or
        // swallow completion: advance exactly once after controls drain.
        before = count();
        { std::lock_guard<std::mutex> lock(box->mutex);
          terminal(app::AudioState::ended);
          Command pause_eof; pause_eof.kind = Command::Pause;
          pause_eof.generation = box->generation; pause_eof.playback_revision = box->playback_revision;
          box->commands.push_back(pause_eof); box->wake.notify_one(); }
        wait_status(app::AudioState::paused, "Track 42"); assert(count() == before);
        Command resume_eof; resume_eof.kind = Command::Resume; send(resume_eof);
        wait_status(app::AudioState::playing, "Track 42");
        send(play); wait_status(app::AudioState::playing, "Track 41");
        // Queue commands atomically against EOF: pending Stop must beat advance.
        Command stop; stop.kind = Command::Stop;
        before = count();
        { std::lock_guard<std::mutex> lock(box->mutex);
          terminal(app::AudioState::ended); ++box->playback_revision;
          stop.generation = box->generation; stop.playback_revision = box->playback_revision;
          box->commands.push_back(stop); box->wake.notify_one(); }
        wait_status(app::AudioState::stopped, ""); assert(count() == before);
        send(play); wait_status(app::AudioState::playing, "Track 41");
        before = count();
        { std::lock_guard<std::mutex> lock(box->mutex);
          terminal(app::AudioState::ended); ++box->playback_revision;
          play.play.occurrence = 42; play.play.song_id = "42";
          play.generation = box->generation; play.playback_revision = box->playback_revision;
          box->commands.push_back(play); box->wake.notify_one(); }
        wait_status(app::AudioState::playing, "Track 42"); assert(count() == before + 1);
    }
    // Hold start after opening; invalidate while worker cannot process commands.
    // Hold its subsequent stop too, exposing stale-publication window directly.
    for (auto kind : {Command::Stop, Command::Play, Command::Logout}) {
        Command stop; stop.kind = Command::Stop; send(stop); barrier();
        wait_status(app::AudioState::stopped, "");
        { std::lock_guard<std::mutex> lock(audio_mutex);
          hold_start = true; start_entered = false; stop_entered = false; }
        Command play; play.kind = Command::Play;
        play.play = {"duplicate", "context", {}, View::AlbumTracks, 41, 0, {}}; send(play);
        until([] { std::lock_guard<std::mutex> lock(audio_mutex); return start_entered; });
        const size_t before = count();
        Command replacement; replacement.kind = kind;
        replacement.play = {"42", "context", {}, View::AlbumTracks, 42, 0, {}}; send(replacement);
        size_t stop_count;
        { std::lock_guard<std::mutex> lock(audio_mutex);
          stop_count = stops; hold_stop = true; stop_entered = false;
          hold_start = false; audio_wake.notify_all(); }
        until([] { std::lock_guard<std::mutex> lock(audio_mutex); return stop_entered; });
        { std::lock_guard<std::mutex> lock(audio_mutex);
          assert(stops == stop_count + 1 && !active && audio.state == app::AudioState::stopped); }
        { std::lock_guard<std::mutex> lock(box->mutex);
          assert(box->audio.state == app::AudioState::stopped && box->title.empty()); }
        { std::lock_guard<std::mutex> lock(audio_mutex); hold_stop = false; audio_wake.notify_all(); }
        barrier();
        wait_status(kind == Command::Play ? app::AudioState::playing : app::AudioState::stopped,
                    kind == Command::Play ? "Track 42" : "");
        assert(count() == before + (kind == Command::Play ? 1 : 0));
        if (kind == Command::Logout) { send(login); barrier(); }
    }
    // Logout during full-context fetch prevents stale player start and metadata.
    Command held; held.kind = Command::Play;
    held.play = {"0", "held", {}, View::AlbumTracks, 0, 0, {}}; send(held);
    { std::unique_lock<std::mutex> lock(gate);
      assert(wake.wait_for(lock, std::chrono::seconds(5), [&] { return entered; })); }
    size_t before = count(); box->reset();
    { std::lock_guard<std::mutex> lock(gate); release = true; wake.notify_one(); }
    // New login is FIFO barrier proving stale fetch cleanup and Logout finished.
    { std::lock_guard<std::mutex> lock(box->mutex); assert(box->results.empty()); }
    send(login);
    until([&] { std::lock_guard<std::mutex> lock(box->mutex); return !box->results.empty(); });
    wait_status(app::AudioState::stopped, ""); assert(count() == before);
    // Generation invalidation beats old EOF even before Logout is processed.
    box->reset(); wait_status(app::AudioState::stopped, "");
    before = count(); terminal(app::AudioState::ended);
    std::this_thread::sleep_for(std::chrono::milliseconds(240)); assert(count() == before);
    { std::lock_guard<std::mutex> lock(box->mutex); box->quit = true; box->wake.notify_one(); }
    until([] { std::lock_guard<std::mutex> lock(audio_mutex); return destroyed; });
}
