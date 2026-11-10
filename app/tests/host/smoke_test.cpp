/* Navi5 — offline smoke integration checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/smoke_session.h"
#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <set>
#include <random>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
int sceAudioOutInit() { assert(false); return -1; }
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned) { assert(false); return -1; }
int sceAudioOutOutput(int, const void *) { assert(false); return -1; }
int sceAudioOutClose(int) { assert(false); return -1; }
}
using namespace app::smoke;
using namespace ui::music;
namespace {
// Gate: blocks a named point until released; observes entry from test thread.
struct Gate {
    std::mutex m; std::condition_variable cv; bool armed = false, entered = false, open = false;
    void arm() { std::lock_guard<std::mutex> l(m); armed = true; entered = open = false; }
    void pass() {
        std::unique_lock<std::mutex> l(m);
        if (!armed) return;
        armed = false; entered = true; cv.notify_all();
        cv.wait(l, [&] { return open; });
    }
    void wait_entered() { std::unique_lock<std::mutex> l(m); assert(cv.wait_for(l, std::chrono::seconds(3), [&] { return entered; })); }
    void release() { std::lock_guard<std::mutex> l(m); open = true; cv.notify_all(); }
};
// Real worker, fake output: start/stop/status are the only audio seam.
struct FakePlayer : app::AudioPlayer {
    std::mutex m;
    std::vector<std::string> urls;
    int pauses = 0, resumes = 0, seeks = 0;
    double duration = 30, position = 0, last_seek = -1;
    bool seek_ok = true;
    app::AudioState start_state = app::AudioState::playing;
    uint64_t track = 0;
    app::AudioState state = app::AudioState::stopped;
    std::shared_ptr<std::atomic<bool>> flag = std::make_shared<std::atomic<bool>>(false);
    Gate stop_gate;
    bool start(std::string url, std::string = {}) override {
        std::lock_guard<std::mutex> l(m); *flag = false; urls.push_back(std::move(url)); state = start_state; return true;
    }
    void stop() override { stop_gate.pass(); std::lock_guard<std::mutex> l(m); state = app::AudioState::stopped; }
    void pause() override { std::lock_guard<std::mutex> l(m); ++pauses; }
    void resume() override { std::lock_guard<std::mutex> l(m); ++resumes; }
    app::AudioStatus status() const override {
        auto &self = const_cast<FakePlayer &>(*this);
        std::lock_guard<std::mutex> l(self.m); app::AudioStatus s; s.state = state;
        s.duration = duration; s.position = position; s.track = s.next_track = track; return s;
    }
    bool seek(double seconds, uint64_t expected) override {
        std::lock_guard<std::mutex> l(m); assert(seconds >= 0 && seconds < duration && expected == track);
        ++seeks; last_seek = seconds; return seek_ok;
    }
    bool replace_ok = true;
    std::vector<std::string> replacements;
    bool replace_next(std::string url, uint64_t expected) override {
        std::lock_guard<std::mutex> l(m);
        if (!replace_ok || expected != track) return false;
        replacements.push_back(std::move(url)); return true;
    }
    std::shared_ptr<std::atomic<bool>> cancel_flag() override { return flag; }
    size_t started() { std::lock_guard<std::mutex> l(m); return urls.size(); }
    void set(app::AudioState s) { std::lock_guard<std::mutex> l(m); state = s; }
};
template <class F> bool eventually(F f)
{
    for (int i = 0; i < 300; ++i) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
}
void tab_preload_success()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    std::atomic<int> requests{0}, searches{0};
    assert(start_worker(box, [&](const std::string &url) {
        ++requests;
        if (url.find("ping?") != std::string::npos)
            return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", {}};
        if (url.find("search3?") != std::string::npos) {
            ++searches;
            std::string body = R"({"subsonic-response":{"status":"ok","searchResult3":{"artist":[)";
            for (int i = 0; i < 200; ++i) {
                if (i) body += ',';
                body += "{\"id\":\"artist" + std::to_string(i) + "\",\"name\":\"Composer\",\"albumCount\":0}";
            }
            return subsonic::HttpResponse{200, body + "]}}}", {}};
        }
        if (url.find("getPlaylists?") != std::string::npos)
            return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","playlists":{"playlist":[{"id":"list","name":"List"}]}}})", {}};
        assert(url.find("getAlbumList2?") != std::string::npos);
        return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","albumList2":{"album":[{"id":"album","name":"Album"}]}}})", {}};
    }, player));
    Command c; c.kind = Command::Login; c.server = "https://example.invalid"; c.username = c.password = "fixture";
    assert(box->send(c));
    assert(eventually([&] { std::lock_guard<std::mutex> lock(box->mutex); return bool(box->browse_client); }));
    c = {}; c.kind = Command::Browse; c.request.token = 1; assert(box->send(c));
    assert(eventually([&] { std::lock_guard<std::mutex> lock(box->mutex); return box->first_pages.size() == 4; }));
    assert(requests == 9 && searches == 5); // ping + Home(newest only; recent is local) + Artists(5) + Albums(1) + Playlists(1).
    for (auto view : {View::Home, View::Artists, View::Albums, View::Playlists}) {
        Request r; r.view = view; Page page;
        assert(box->cached(r, page, box->generation));
        if (view == View::Artists) assert(page.items.empty() && page.next_cursor == "1000");
        r.cursor = "40"; assert(!box->cached(r, page, box->generation));
    }
    assert(requests == 9); // All initial root selections use cache; no next-page warming.
    { std::lock_guard<std::mutex> lock(box->mutex);
      assert(box->results.size() == 2); } // Login + foreground Home only, never speculative results.
    Command play; play.kind = Command::Play;
    assert(box->send(play));
    Request home_request; Page home_page;
    assert(!box->cached(home_request, home_page, box->generation)); // Recent history not frozen after Play.
    { std::lock_guard<std::mutex> lock(box->mutex);
      box->quit = true; box->wake.notify_one(); box->browse_wake.notify_one(); }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}
void tab_preload()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    Gate artist_gate; artist_gate.arm();
    std::atomic<int> home{0}, artists{0}, albums{0}, playlists{0};
    auto http = [&](const std::string &url) {
        std::string body = R"({"subsonic-response":{"status":"ok"}})";
        if (url.find("ping?") != std::string::npos) {}
        else if (url.find("search3?") != std::string::npos) {
            ++artists; artist_gate.pass();
            body = R"({"subsonic-response":{"status":"ok","searchResult3":{"artist":[{"id":"artist","name":"Artist","albumCount":1}]}}})";
        } else if (url.find("getPlaylists?") != std::string::npos) {
            ++playlists;
            return subsonic::HttpResponse{500, {}, {}}; // Speculative failure stays invisible.
        } else {
            assert(url.find("getAlbumList2?") != std::string::npos);
            if (url.find("alphabeticalByName") != std::string::npos) ++albums;
            else ++home;
            body = R"({"subsonic-response":{"status":"ok","albumList2":{"album":[{"id":"album","name":"Album"}]}}})";
        }
        return subsonic::HttpResponse{200, body, {}};
    };
    assert(start_worker(box, http, player));
    auto send = [&](Command::Kind kind, View view = View::Home, uint64_t token = 1) {
        Command c; c.kind = kind; c.generation = box->generation; c.request.view = view; c.request.token = token;
        c.server = "https://example.invalid"; c.username = c.password = "fixture";
        assert(box->send(std::move(c)));
    };
    auto result = [&](Command::Kind kind, uint64_t token = 0) {
        return eventually([&] {
            std::lock_guard<std::mutex> lock(box->mutex);
            for (auto it = box->results.begin(); it != box->results.end(); ++it)
                if (it->kind == kind && (!token || it->token == token)) {
                    assert(it->ok); box->results.erase(it); return true;
                }
            return false;
        });
    };
    send(Command::Login); assert(result(Command::Login));
    assert(home == 0 && artists == 0 && albums == 0 && playlists == 0);
    send(Command::Browse); assert(result(Command::Browse, 1));
    artist_gate.wait_entered(); // Home delivered before expensive speculative artist call.
    assert(home == 1 && artists == 1);
    // Playback controls drain on their own worker even while speculative HTTP blocks.
    send(Command::Pause);
    assert(eventually([&] { std::lock_guard<std::mutex> lock(player->m); return player->pauses == 1; }));
    // Active Albums outranks remaining artist scan and rejects its obsolete result.
    send(Command::Browse, View::Albums, 2);
    artist_gate.release(); assert(result(Command::Browse, 2));
    assert(eventually([&] { return playlists == 1; }));
    Request request; request.view = View::Albums; Page page;
    assert(box->cached(request, page, box->generation) && page.items[0].id == "album");
    assert(albums == 1 && artists == 1); // Cached first selection costs zero HTTP calls.
    State state; state.activate(); auto initial = state.take_request(); assert(initial);
    state.tab(2); auto current = state.take_request(); assert(current);
    assert(!state.complete(initial->token, page));
    assert(state.complete(current->token, page) && state.tab_index() == 2 && state.selected() == 0);
    {   // Cached first chunk feeds infinite continuation: exact offset, no duplicate/skip, never cached.
        Page first; first.next_cursor = "40";
        for (int i = 0; i < 40; ++i) first.items.push_back({Kind::Album, "c" + std::to_string(i), "Album", "", "", 0});
        State scroll; scroll.activate(); scroll.take_request();
        scroll.tab(2); auto root = scroll.take_request(); assert(root && root->cursor.empty());
        { std::lock_guard<std::mutex> lock(box->mutex);
          box->first_pages[View::Albums] = {first, std::chrono::steady_clock::now() + std::chrono::seconds(60)}; }
        Page hit; assert(box->cached(*root, hit, box->generation));
        assert(scroll.complete(root->token, std::move(hit)) && scroll.page().start_index == 0);
        for (int i = 0; i < 39; ++i) assert(scroll.move(1));
        assert(scroll.move(1)); // Past cached chunk: continuation, not a cache hit.
        auto more = scroll.take_request(); assert(more && more->cursor == "40");
        Page miss; assert(!box->cached(*more, miss, box->generation));
        Page second; second.items.push_back({Kind::Album, "c40", "Album", "", "", 0});
        assert(scroll.complete(more->token, second));
        assert(scroll.page().items.size() == 41 && scroll.page().items[40].id == "c40" && scroll.absolute() == 40);
    }
    assert(eventually([&] { std::lock_guard<std::mutex> lock(box->mutex); return box->warm_attempted & (1u << unsigned(View::Playlists)); }));
    { std::lock_guard<std::mutex> lock(box->mutex); assert(box->results.empty()); }
    request.view = View::Playlists; assert(!box->cached(request, page, box->generation));
    send(Command::Browse, View::Playlists, 3);
    assert(eventually([&] {
        std::lock_guard<std::mutex> lock(box->mutex);
        if (box->results.empty()) return false;
        assert(box->results.front().token == 3 && !box->results.front().ok);
        box->results.clear(); return true;
    }));
    assert(playlists == 2); // Failed warmup falls back to normal foreground request.
    request.view = View::Albums;
    { std::lock_guard<std::mutex> lock(box->mutex); box->first_pages.at(View::Albums).expires = std::chrono::steady_clock::now(); }
    assert(!box->cached(request, page, box->generation));
    request.view = View::Home; assert(box->cached(request, page, box->generation));
    auto old = box->generation; box->reset();
    assert(!box->cached(request, page, old) && !box->cached(request, page, box->generation));
    { std::lock_guard<std::mutex> lock(box->mutex); assert(box->first_pages.empty() && !box->warm_ready && !box->warm_attempted); }
    send(Command::Login); assert(result(Command::Login)); // New account cannot see old Home.
    assert(!box->cached(request, page, box->generation));
    artist_gate.arm(); send(Command::Browse, View::Home, 4); assert(result(Command::Browse, 4));
    artist_gate.wait_entered(); box->reset(); artist_gate.release();
    send(Command::Login); assert(result(Command::Login));
    // Next-generation Home is a FIFO barrier behind stale artist completion.
    send(Command::Browse, View::Home, 5); assert(result(Command::Browse, 5));
    { std::lock_guard<std::mutex> lock(box->mutex);
      assert(box->results.empty());
      auto found = box->first_pages.find(View::Artists);
      // If new-generation warmup has already run it must be a fresh request.
      assert(found == box->first_pages.end() || artists >= 3);
      box->quit = true; box->wake.notify_one(); box->browse_wake.notify_one(); }
    // Worker never touches http after quit; fixture captures remain alive until drained.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
}
void recent_history()
{
    char path[] = "build/host-tests/recent-XXXXXX";
    const int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
    const auto scope = app::RecentContexts::scope("https://example.invalid", "one");
    assert(scope.size() == 64 && scope != app::RecentContexts::scope("https://other.invalid", "one"));
    app::RecentContexts h(scope, path);
    h.promote({Kind::Artist, "a", "Astrid", {}, "cover"});
    h.promote({Kind::Playlist, "a", "Mix", {}, {}});
    assert(h.save());
    struct stat st; assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    app::RecentContexts reload(scope, path); assert(reload.items().size() == 2 && reload.items()[1].kind == Kind::Artist);
    app::RecentContexts other(app::RecentContexts::scope("https://example.invalid", "two"), path);
    assert(other.items().empty());
    app::RecentContexts api({}, path); assert(api.items().empty());
    std::ifstream file(path); const std::string body((std::istreambuf_iterator<char>(file)), {});
    assert(body.find("example.invalid") == std::string::npos && body.find("one") == std::string::npos);
    for (int n = 0; n < 8; ++n) h.promote({Kind::Album, std::to_string(n), "Album", {}, {}});
    assert(h.items().size() == 6 && h.items()[0].id == "7");
    h.promote({Kind::Album, "7", "Album", {}, {}}); assert(h.items().size() == 6);
    h.promote({Kind::Album, "bad", "Album", {}, "https://example.invalid/?token=secret"});
    assert(h.items()[0].id == "7");
    // Failed atomic replacement leaves old file intact.
    assert(mkdir((std::string(path) + ".tmp").c_str(), 0700) == 0);
    assert(!h.save());
    assert(app::RecentContexts(scope, path).items().size() == 2);
    rmdir((std::string(path) + ".tmp").c_str()); unlink(path);
}
void playback_snapshot()
{
    Mailbox box;
    auto generation = box.reset();
    PlaybackSnapshot view;
    std::deque<Result> results;
    auto publish = [&] {
        std::lock_guard<std::mutex> lock(box.mutex);
        box.audio_revision = 17;
        box.audio.state = app::AudioState::playing;
        box.audio.track = box.audio.next_track = 42;
        box.audio.position = 12; box.audio.duration = 180;
        box.song_id = "song"; box.title = "Title"; box.artist = "Artist";
        box.artwork_url = "cover"; box.portrait_url = "portrait";
        box.next_title = "Next"; box.duration = 181;
        box.shuffle_enabled = box.shuffle_pending = box.shuffle_error = true;
    };
    auto playing = [&] {
        assert(view.generation == generation && view.revision == 17);
        assert(view.audio.state == app::AudioState::playing && view.audio.track == 42 && view.audio.next_track == 42);
        assert(view.audio.position == 12 && view.audio.duration == 180);
        assert(view.song_id == "song" && view.title == "Title" && view.artist == "Artist");
        assert(view.artwork_url == "cover" && view.portrait_url == "portrait");
        assert(view.next_title == "Next" && view.duration == 181);
        assert(view.shuffle_enabled && view.shuffle_pending && view.shuffle_error);
    };
    auto empty = [&] {
        assert(!view.shuffle_enabled && !view.shuffle_pending && !view.shuffle_error);
        assert(view.audio.state == app::AudioState::stopped && view.audio.track == 0);
        assert(view.audio.position == 0 && view.audio.duration == 0);
        assert(view.song_id.empty() && view.title.empty() && view.artist.empty());
        assert(view.artwork_url.empty() && view.portrait_url.empty() && view.next_title.empty() && view.duration == 0);
    };
    // Another thread holds mailbox through multiple render polls. No sleeps/races.
    auto contended = [&](auto check) {
        Gate gate; gate.arm();
        std::thread holder([&] { std::lock_guard<std::mutex> lock(box.mutex); gate.pass(); });
        gate.wait_entered();
        for (int i = 0; i < 3; ++i) {
            assert(!view.poll(box, generation, results, true));
            assert(results.empty()); check();
        }
        gate.release(); holder.join();
    };
    publish();
    box.results.emplace_back();
    assert(view.poll(box, generation, results, true) && results.size() == 1);
    results.clear(); playing();
    contended(playing);
    // Stop/context replacement is an authoritative empty publication, not busy.
    { std::lock_guard<std::mutex> lock(box.mutex);
      box.audio = {}; box.song_id.clear(); box.title.clear(); box.artist.clear();
      box.artwork_url.clear(); box.portrait_url.clear(); box.next_title.clear(); box.duration = 0;
      box.shuffle_enabled = box.shuffle_pending = box.shuffle_error = false;
      box.audio_revision = 18; }
    assert(view.poll(box, generation, results, true)); empty();
    assert(view.revision == 18);
    contended(empty);
    publish(); assert(view.poll(box, generation, results, false)); playing();
    // Local logout clears cached data in the same frame, before another poll.
    generation = box.reset(); view.invalidate(generation); empty();
    assert(view.generation == 0 && view.revision == 0);
    contended(empty);
    assert(view.poll(box, generation, results, true)); empty();
    assert(view.generation == generation);
    // poll also rejects an old account cache on generation change while busy.
    publish(); assert(view.poll(box, generation, results, true)); playing();
    generation = box.reset(); contended(empty);
    // A mismatched mailbox generation must never copy another account's data.
    publish();
    assert(view.poll(box, generation - 1, results, true)); empty();
    assert(view.generation == 0);
}
void portrait_worker()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    static Gate gate;
    std::atomic<int> requests{0};
    const auto render_thread = std::this_thread::get_id();
    auto http = [&](const std::string &url) {
        assert(std::this_thread::get_id() != render_thread);
        if (url.find("ping?") != std::string::npos)
            return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", {}};
        assert(url.find("getArtistInfo2?") != std::string::npos);
        ++requests; gate.pass();
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\",\"artistInfo2\":{\"largeImageUrl\":\"https://example.invalid/p?" + std::string(1024, 'x') + "\",\"mediumImageUrl\":\"https://example.invalid/portrait.jpg\"}}}", {}};
    };
    assert(start_worker(box, http, player));
    Command login; login.kind = Command::Login; login.generation = box->reset();
    login.server = "https://example.invalid"; login.username = login.password = "fixture";
    assert(box->send(login));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return bool(box->browse_client); }));
    { std::lock_guard<std::mutex> l(box->mutex); ++box->playback_revision; } // Hold audio publication while testing metadata seam.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(requests == 0); // No artist: no metadata fetch.
    gate.arm();
    { std::lock_guard<std::mutex> l(box->mutex); box->artist_id = "first"; }
    gate.wait_entered();
    { std::lock_guard<std::mutex> l(box->mutex); box->artist_id.clear(); }
    gate.release();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->portrait_url.empty()); box->artist_id = "second"; }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->portrait_url == "https://example.invalid/portrait.jpg"; }));
    const auto cached = requests.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(250)); assert(requests == cached);
    gate.arm();
    { std::lock_guard<std::mutex> l(box->mutex); box->artist_id = "third"; box->portrait_url.clear(); }
    gate.wait_entered();
    Command pause; pause.kind = Command::Pause; pause.generation = 1;
    assert(box->send(pause));
    assert(eventually([&] { std::lock_guard<std::mutex> l(player->m); return player->pauses == 1; }));
    box->reset(); gate.release();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->portrait_url.empty()); box->quit = true; box->wake.notify_one(); box->browse_wake.notify_one(); }
}
void artist_queue()
{
    Library library;
    int requests = 0;
    bool stale = false;
    std::string artist = R"({"id":"artist","albumCount":2,"album":[{"id":"one","songCount":2},{"id":"two","songCount":1}]})";
    std::string first = R"({"id":"one","songCount":2,"artist":"Artist","artistId":"artist","coverArt":"cover","song":[{"id":"dup","title":"First","artist":"Artist","track":9},{"id":"dup","title":"Second","track":1}]})";
    std::string second = R"({"id":"two","songCount":1,"song":[{"id":"last","title":"Last"}]})";
    bool fail_second = false, cancel_first = false;
    subsonic::Client client("https://example.invalid", "fixture", "fixture", [&](const std::string &url) {
        ++requests;
        std::string body;
        if (url.find("getArtist?") != std::string::npos) body = "\"artist\":" + artist;
        else if (url.find("id=one") != std::string::npos) {
            body = "\"album\":" + first;
            if (cancel_first) stale = true;
        } else {
            assert(url.find("id=two") != std::string::npos);
            if (fail_second) return subsonic::HttpResponse{500, {}, {}};
            body = "\"album\":" + second;
        }
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    });
    PlayIntent intent; intent.context = View::ArtistAlbums; intent.context_id = "artist";
    intent.action = PlayAction::ArtistPlayAll;
    app::PlayQueue queue;
    std::string error;
    assert(library.queue(client, intent, queue, [&] { return stale; }, &error));
    assert(requests == 3 && queue.tracks().size() == 3);
    assert(queue.current()->title == "First" && queue.up_next()->title == "Second");
    assert(queue.current()->artist_id == "artist" && queue.current()->cover_art.find("getCoverArt?") != std::string::npos);
    assert(queue.on_track_end() && queue.current()->title == "Second");
    assert(queue.on_track_end() && queue.current()->title == "Last");
    assert(!queue.on_track_end() && !queue.successor_index());
    intent.action = PlayAction::ArtistShuffle;
    for (int n = 0; n < 12; ++n) {
        assert(library.queue(client, intent, queue));
        std::vector<std::string> titles;
        do { titles.push_back(queue.current()->title); } while (queue.on_track_end());
        std::sort(titles.begin(), titles.end());
        assert((titles == std::vector<std::string>{"First", "Last", "Second"}));
    }
    // Failed or cancelled assembly never publishes a prefix or replaces caller queue.
    fail_second = true;
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(error == "Kunne ikke laste alle artistalbum" && queue.tracks().size() == 3);
    fail_second = false; cancel_first = true; requests = 0;
    assert(!library.queue(client, intent, queue, [&] { return stale; }));
    assert(requests == 2 && queue.tracks().size() == 3);
    stale = false; cancel_first = false;
    const auto saved = artist;
    artist = R"({"id":"artist","albumCount":3,"album":[{"id":"one","songCount":2},{"id":"two","songCount":1}]})";
    requests = 0;
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(requests == 1 && error == "Ufullstendig albumliste for artist");
    artist = R"({"id":"artist","albumCount":0})";
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(error == "Artisten har ingen album å spille");
    artist = saved;
    second = R"({"id":"two","songCount":2,"song":[{"id":"last"}]})";
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(error == "Ufullstendige spor i artistalbum");
    artist = "{\"id\":\"artist\",\"albumCount\":257,\"album\":[";
    for (int n = 0; n < 257; ++n) {
        if (n) artist += ',';
        artist += "{\"id\":\"" + std::to_string(n) + "\"}";
    }
    artist += "]}";
    requests = 0;
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(requests == 1 && error == "Artist overskrider avspillingsgrensen (256 album)");
    artist = R"({"id":"artist","albumCount":1,"album":[{"id":"one","songCount":20001}]})";
    first = "{\"id\":\"one\",\"songCount\":20001,\"song\":[";
    for (int n = 0; n < 20001; ++n) { if (n) first += ','; first += "{\"id\":\"dup\"}"; }
    first += "]}";
    assert(!library.queue(client, intent, queue, {}, &error));
    assert(error == "Artist overskrider avspillingsgrensen (20000 spor)");
}
void history_worker()
{
    auto box = std::make_shared<Mailbox>(); auto player = std::make_shared<FakePlayer>();
    assert(start_worker(box, [](const std::string &url) {
        std::string body = "\"x\":1";
        if (url.find("getArtist?") != std::string::npos)
            body = R"("artist":{"id":"artist","name":"Astrid","albumCount":1,"album":[{"id":"album","songCount":2}]})";
        if (url.find("getAlbum?") != std::string::npos)
            body = R"("album":{"id":"album","name":"Album","songCount":2,"song":[{"id":"dup"},{"id":"dup"}]})";
        if (url.find("getPlaylist?") != std::string::npos)
            body = R"("playlist":{"id":"list","name":"List","entry":[{"id":"dup"},{"id":"dup"}]})";
        if (url.find("search3?") != std::string::npos)
            body = R"("searchResult3":{"song":[{"id":"found"}]})";
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    }, player));
    auto login = [&] {
        Command c; c.kind = Command::Login; c.generation = box->reset(); c.server = "https://example.invalid";
        c.api_key = "fixture"; assert(box->send(c));
        assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return bool(box->browse_client); }));
        return c.generation;
    };
    auto g = login();
    auto play = [&](View context, std::string id, uint64_t token) {
        Command c; c.kind = Command::Play; c.generation = g; c.play.context = context; c.play.context_id = std::move(id); c.request.token = token;
        if (context == View::ArtistAlbums) c.play.action = PlayAction::ArtistPlayAll;
        assert(box->send(c));
        assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex);
            for (const auto &r : box->results) if (r.kind == Command::Play && r.token == token) return true;
            return false;
        }));
    };
    auto expect = [&](size_t n, Kind kind) {
        assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->recent_contexts.size() == n && box->recent_contexts.front().kind == kind; }));
    };
    play(View::ArtistAlbums, "artist", 1); expect(1, Kind::Artist);
    uint64_t revision; { std::lock_guard<std::mutex> l(box->mutex); revision = box->history_revision; }
    { std::lock_guard<std::mutex> l(player->m); player->track = 1; } // Same song ID, new natural occurrence.
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->audio.track == 1; }));
    { Command c; c.kind = Command::ShuffleToggle; c.generation = g; c.shuffle_enabled = true;
      { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; } assert(box->send(c)); }
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->history_revision == revision && box->recent_contexts[0].kind == Kind::Artist); }
    { std::lock_guard<std::mutex> l(player->m); player->track = 0; }
    play(View::PlaylistTracks, "list", 2); expect(2, Kind::Playlist);
    play(View::AlbumTracks, "album", 3); expect(3, Kind::Album);
    play(View::ArtistAlbums, "artist", 4); expect(3, Kind::Artist);
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->recent_contexts[1].kind == Kind::Album && box->recent_contexts[2].kind == Kind::Playlist); revision = box->history_revision; }
    { std::lock_guard<std::mutex> l(player->m); player->start_state = app::AudioState::error; }
    play(View::PlaylistTracks, "list", 5); // Decoder failed despite successful start admission.
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->history_revision == revision); }
    { std::lock_guard<std::mutex> l(player->m); player->start_state = app::AudioState::opening; }
    play(View::PlaylistTracks, "list", 6);
    { Command c; c.kind = Command::Stop; c.generation = g; assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->audio.state == app::AudioState::stopped; }));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->history_revision == revision); }
    { std::lock_guard<std::mutex> l(player->m); player->start_state = app::AudioState::playing; }
    { Command c; c.kind = Command::Play; c.generation = g; c.play.context = View::Search; c.play.context_id = "fo";
      c.play.song_id = "found"; c.play.page_ids = {"found"}; c.request.token = 70; assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->audio.state == app::AudioState::playing && box->song_id == "found"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->history_revision == revision && box->recent_contexts[0].kind == Kind::Artist); }
    { std::lock_guard<std::mutex> l(player->m); player->start_state = app::AudioState::opening; }
    play(View::AlbumTracks, "album", 7); // Opening: Home cached meanwhile must not survive promotion.
    { std::lock_guard<std::mutex> l(box->mutex);
      box->first_pages[View::Home] = {{}, std::chrono::steady_clock::now() + std::chrono::seconds(60)}; }
    { std::lock_guard<std::mutex> l(player->m); player->state = app::AudioState::playing; }
    expect(3, Kind::Album);
    { std::lock_guard<std::mutex> l(box->mutex); assert(!box->first_pages.count(View::Home)); }
    g = login(); // API-key login never inherits prior identity, even same server.
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->recent_contexts.empty()); box->quit = true; box->wake.notify_all(); box->browse_wake.notify_all(); }
}
void worker_races()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    static Gate album_gate, browse_gate; // outlive detached workers
    assert(start_worker(box, [&](const std::string &url) {
        std::string body = "\"x\":1";
        if (url.find("getArtist?") != std::string::npos) {
            body = "\"artist\":{\"id\":\"artist\",\"albumCount\":2,\"album\":[{\"id\":\"al\",\"songCount\":3},{\"id\":\"later\",\"songCount\":3}]}";
        } else if (url.find("getAlbum?") != std::string::npos) {
            album_gate.pass();
            body = "\"album\":{\"id\":\"al\",\"song\":[{\"id\":\"a\",\"title\":\"A\"},{\"id\":\"dup\",\"title\":\"One\"},{\"id\":\"dup\",\"title\":\"\"}]}";
        } else if (url.find("getAlbumList2?") != std::string::npos) {
            browse_gate.pass();
            body = "\"albumList2\":{\"album\":[{\"id\":\"al\",\"name\":\"Album\"}]}";
        }
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    }, player));
    auto login = [&] {
        Command c; c.kind = Command::Login; c.generation = box->reset();
        c.server = "https://example.invalid"; c.username = c.password = "fixture";
        assert(box->send(c));
        assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return !box->results.empty(); }));
        std::lock_guard<std::mutex> l(box->mutex); assert(box->results.front().ok); box->results.clear();
        return c.generation;
    };
    auto play = [&](uint64_t g, size_t occurrence, uint64_t token) {
        Command c; c.kind = Command::Play; c.generation = g; c.play.context_id = "al"; c.play.song_id = occurrence ? "dup" : "a";
        c.play.occurrence = occurrence; c.play.page_offset = 1; c.play.page_ids = {"dup", "dup"}; c.request.token = token;
        assert(box->send(c));
    };
    auto play_result = [&](uint64_t token) {
        int ok = -1;
        assert(eventually([&] {
            std::lock_guard<std::mutex> l(box->mutex);
            for (auto &r : box->results) if (r.kind == Command::Play && r.token == token) ok = r.ok;
            return ok >= 0;
        }));
        return ok == 1;
    };

    // P1: logout while get_album blocks; release must not start old account's audio.
    auto g = login();
    album_gate.arm(); play(g, 2, 1); album_gate.wait_entered();
    box->reset(); album_gate.release();
    g = login(); // FIFO barrier past stale Play
    assert(player->started() == 0);

    // New artist context cancelled during first album fetch: Stop/logout must win.
    auto artist_play = [&] {
        Command c; c.kind = Command::Play; c.generation = g; c.play.context_id = "artist";
        c.play.context = View::ArtistAlbums; c.play.action = PlayAction::ArtistShuffle;
        assert(box->send(c));
    };
    album_gate.arm(); artist_play(); album_gate.wait_entered();
    { Command c; c.kind = Command::Stop; c.generation = g; assert(box->send(c)); }
    album_gate.release();
    // Login is a FIFO barrier and another playback revision; old artist must not start.
    g = login();
    assert(player->started() == 0);
    album_gate.arm(); artist_play(); album_gate.wait_entered();
    box->reset(); album_gate.release(); g = login();
    assert(player->started() == 0);

    // Duplicate IDs: second occurrence selected exactly; untitled song still present.
    play(g, 2, 2); assert(play_result(2));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->song_id == "dup" && box->title.empty() && box->next_title.empty(); }));
    play(g, 1, 3); assert(play_result(3));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "One" && box->next_title.empty(); }));
    play(g, 0, 4); assert(play_result(4));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->song_id == "a" && box->next_title == "One"; }));
    {
        // Steady playback, real worker: audio loop republishes status and browse loop
        // rewrites portrait every ~100 ms. A render poll losing try_lock keeps track.
        PlaybackSnapshot view; std::deque<Result> results;
        assert(eventually([&] { return view.poll(*box, g, results, true) && view.song_id == "a"; }));
        const auto revision = view.revision;
        for (int i = 0; i < 300; ++i) {
            view.poll(*box, g, results, true);
            assert(view.song_id == "a" && view.title == "A" && view.next_title == "One" && view.revision == revision);
            assert(view.audio.state == app::AudioState::playing);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        Gate hold; hold.arm();
        std::thread holder([&] { std::lock_guard<std::mutex> l(box->mutex); hold.pass(); });
        hold.wait_entered();
        for (int i = 0; i < 3; ++i) {
            assert(!view.poll(*box, g, results, true));
            assert(view.song_id == "a" && view.audio.state == app::AudioState::playing && view.revision == revision);
        }
        hold.release(); holder.join();
    }
    auto seek_command = [&](double seconds) {
        Command c; c.kind = Command::Seek; c.generation = g; c.seconds = seconds;
        { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; c.track = box->audio.track; }
        return c;
    };
    auto seek_barrier = [&] {
        int before; { std::lock_guard<std::mutex> l(player->m); before = player->resumes; }
        Command c; c.kind = Command::Resume; c.generation = g; assert(box->send(c));
        assert(eventually([&] { std::lock_guard<std::mutex> l(player->m); return player->resumes > before; }));
    };
    assert(box->send(seek_command(0))); seek_barrier();
    { std::lock_guard<std::mutex> l(player->m); assert(player->seeks == 1); }
    for (double target : {-1.0, 30.0, double(INFINITY), double(NAN)}) {
        assert(box->send(seek_command(target))); seek_barrier();
        { std::lock_guard<std::mutex> l(box->mutex); assert(box->audio.seek_failed); }
    }
    { std::lock_guard<std::mutex> l(player->m); assert(player->seeks == 1); player->duration = 0; }
    assert(box->send(seek_command(10))); seek_barrier();
    { std::lock_guard<std::mutex> l(player->m); assert(player->seeks == 1); player->duration = 30; player->seek_ok = false; }
    assert(box->send(seek_command(10))); seek_barrier();
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->audio.seek_failed); }
    auto stale = seek_command(10);
    stale.track = 1; // Different occurrence, even if song ID could match.
    assert(box->send(stale)); seek_barrier();
    { std::lock_guard<std::mutex> l(player->m); assert(player->seeks == 2); player->resumes = 0; }
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(!box->send(stale)); // Old displayed revision cannot seek replacement.
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "One"; }));
    { Command c; c.kind = Command::Previous; c.generation = g;
      { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; } assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->song_id == "a" && box->next_title == "One"; }));
    { Command c; c.kind = Command::Play; c.generation = g; c.play.context_id = "al"; c.play.song_id = "dup"; c.play.occurrence = 0; c.request.token = 5;
      assert(box->send(c)); }
    assert(!play_result(5)); // ID/occurrence mismatch never falls back to first match
    { Command c; c.kind = Command::Play; c.generation = g; c.play.context_id = "al"; c.play.song_id = "dup"; c.play.occurrence = 1;
      c.play.page_offset = 0; c.play.page_ids = {"dup"}; c.request.token = 6; assert(box->send(c)); }
    assert(!play_result(6)); // stale visible order rejected

    const auto starts_before_resume = player->started();
    { Command c; c.kind = Command::Resume; c.generation = g; assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(player->m); return player->resumes == 1; }));
    assert(player->started() == starts_before_resume); // Hero Resume must reuse session.
    auto before_stop = seek_command(1);
    { Command c; c.kind = Command::Stop; c.generation = g; assert(box->send(c)); }
    assert(!box->send(before_stop)); // Stop invalidates UI snapshot immediately.

    // Admitted track is cancelled by reset itself, not by a later worker poll.
    assert(!*player->flag);
    // Replacement: logout while start waits in stop(); old account must not start.
    play(g, 1, 7); assert(play_result(7));
    const size_t before = player->started();
    player->stop_gate.arm(); play(g, 0, 8);
    player->stop_gate.wait_entered();
    box->reset();
    assert(*player->flag);
    player->stop_gate.release();
    g = login();
    assert(player->started() == before);

    // Browse blocked in HTTP: transport controls still served by audio worker.
    browse_gate.arm();
    { Command c; c.kind = Command::Browse; c.generation = g; c.request.token = 9; assert(box->send(c)); }
    browse_gate.wait_entered();
    { Command c; c.kind = Command::Pause; c.generation = g; assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(player->m); return player->pauses == 1; }));
    browse_gate.release();
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex);
        for (auto &r : box->results) if (r.kind == Command::Browse && r.token == 9) return r.ok && r.page.items.size() == 1 && r.page.sections.size() == 2;
        return false; }));
    // Delayed browse of a logged-out account is dropped.
    browse_gate.arm();
    { Command c; c.kind = Command::Browse; c.generation = g; c.request.token = 10; assert(box->send(c)); }
    browse_gate.wait_entered(); box->reset(); browse_gate.release();
    g = login();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->results.empty()); box->quit = true; box->wake.notify_all(); box->browse_wake.notify_all(); }
}
void previous_worker()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    assert(start_worker(box, [](const std::string &url) {
        std::string body = "\"x\":1";
        if (url.find("getAlbum?") != std::string::npos)
            body = "\"album\":{\"id\":\"al\",\"song\":[{\"id\":\"a\",\"title\":\"A\"},{\"id\":\"b\",\"title\":\"B\"},"
                   "{\"id\":\"c\",\"title\":\"C\"},{\"id\":\"d\",\"title\":\"D\"}]}";
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    }, player));
    Command login; login.kind = Command::Login; login.generation = box->reset();
    login.server = "https://example.invalid"; login.username = login.password = "fixture";
    const auto g = login.generation;
    assert(box->send(login));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return !box->results.empty(); }));
    { std::lock_guard<std::mutex> l(box->mutex); box->results.clear(); }
    auto title = [&](const char *t) { return eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == t; }); };
    auto set = [&](double position, double duration) { std::lock_guard<std::mutex> l(player->m); player->position = position; player->duration = duration; };
    auto counts = [&](size_t starts, int seeks) { std::lock_guard<std::mutex> l(player->m); return player->urls.size() == starts && player->seeks == seeks; };
    auto previous = [&] {
        int before; { std::lock_guard<std::mutex> l(player->m); before = player->resumes; }
        { Command c; c.kind = Command::Previous; c.generation = g;
      { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; } assert(box->send(c)); }
        Command r; r.kind = Command::Resume; r.generation = g; assert(box->send(r)); // FIFO barrier
        assert(eventually([&] { std::lock_guard<std::mutex> l(player->m); return player->resumes > before; }));
    };
    Command play; play.kind = Command::Play; play.generation = g; play.play.context_id = "al";
    play.play.song_id = "c"; play.play.occurrence = 2; assert(box->send(play));
    assert(title("C"));
    uint64_t revision; { std::lock_guard<std::mutex> l(box->mutex); revision = box->audio_revision; }
    // >3s restarts same occurrence through seek(0); queue and recent history untouched.
    set(3.5, 30); previous();
    { std::lock_guard<std::mutex> l(player->m); assert(player->last_seek == 0); }
    assert(counts(1, 1) && title("C"));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->next_title == "D"); }
    // Exactly 3s boundary and below: previous track.
    set(3.0, 30); previous(); assert(title("B") && counts(2, 1));
    set(0.5, 30); previous(); assert(title("A") && counts(3, 1));
    // First entry: restart even at <=3s.
    set(1.0, 30); previous(); assert(title("A") && counts(3, 2));
    // Paused restart: seek only, no pause/resume/start of its own (player keeps pause).
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(title("B"));
    player->set(app::AudioState::paused); set(10, 30);
    int pauses, resumes; { std::lock_guard<std::mutex> l(player->m); pauses = player->pauses; resumes = player->resumes; }
    { Command c; c.kind = Command::Previous; c.generation = g;
      { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; } assert(box->send(c)); }
    assert(eventually([&] { return counts(4, 3); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(player->m); assert(player->pauses == pauses && player->resumes == resumes && player->state == app::AudioState::paused); }
    assert(title("B"));
    player->set(app::AudioState::playing);
    // Shuffle ON: restart keeps order and successor; pending toggle survives restart.
    auto toggle = [&](bool on) {
        Command c; c.kind = Command::ShuffleToggle; c.generation = g; c.shuffle_enabled = on;
        { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; }
        return c;
    };
    assert(box->send(toggle(true)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_enabled && !box->shuffle_pending; }));
    std::string next; { std::lock_guard<std::mutex> l(box->mutex); next = box->next_title; }
    set(20, 30); previous(); assert(counts(4, 4));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->title == "B" && box->shuffle_enabled && box->next_title == next); }
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = false; }
    assert(box->send(toggle(false)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_pending; }));
    previous(); assert(counts(4, 5));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->shuffle_pending && box->shuffle_enabled && box->title == "B"); }
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = true; }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return !box->shuffle_enabled && !box->shuffle_pending && box->next_title == "C"; }));
    // Stale displayed revision: Previous changed revision, old Seek/Shuffle snapshot refused.
    auto stale = toggle(true);
    assert(box->send(stale)); // Fresh snapshot after restart.
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->playback_revision != revision); }
    stale = toggle(false); previous();
    assert(!box->send(stale));
    int seeks; { std::lock_guard<std::mutex> l(player->m); seeks = player->seeks; }
    size_t starts;
    set(0, 30); previous(); assert(title("A"));
    // Unknown duration (not seekable) / nonfinite position: previous-track fallback.
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(title("B"));
    starts = player->started(); { std::lock_guard<std::mutex> l(player->m); seeks = player->seeks; }
    set(20, 0); previous(); assert(title("A") && counts(starts + 1, seeks));
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(title("B"));
    set(double(NAN), 30); previous(); assert(title("A") && counts(starts + 3, seeks));
    // First entry not seekable: replay occurrence via start.
    set(20, 0); previous(); assert(title("A") && counts(starts + 4, seeks));
    box->reset();
    { std::lock_guard<std::mutex> l(box->mutex); box->quit = true; box->wake.notify_all(); box->browse_wake.notify_all(); }
}
void shuffle_worker()
{
    auto box = std::make_shared<Mailbox>();
    auto player = std::make_shared<FakePlayer>();
    assert(start_worker(box, [](const std::string &url) {
        std::string body = "\"x\":1";
        if (url.find("getAlbum?") != std::string::npos)
            body = "\"album\":{\"id\":\"al\",\"song\":[{\"id\":\"a\",\"title\":\"A\"},{\"id\":\"b\",\"title\":\"B\"},"
                   "{\"id\":\"dup\",\"title\":\"D1\"},{\"id\":\"dup\",\"title\":\"D2\"},{\"id\":\"c\",\"title\":\"C\"}]}";
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    }, player));
    Command login; login.kind = Command::Login; login.generation = box->reset();
    login.server = "https://example.invalid"; login.username = login.password = "fixture";
    const auto g = login.generation;
    assert(box->send(login));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return !box->results.empty(); }));
    { std::lock_guard<std::mutex> l(box->mutex); box->results.clear(); }
    Command play; play.kind = Command::Play; play.generation = g; play.play.context_id = "al";
    play.play.song_id = "b"; play.play.occurrence = 1; assert(box->send(play));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "B" && box->next_title == "D1"; }));
    { Command c; c.kind = Command::Pause; c.generation = g; assert(box->send(c)); }
    auto toggle = [&](bool on) {
        Command c; c.kind = Command::ShuffleToggle; c.generation = g; c.shuffle_enabled = on;
        { std::lock_guard<std::mutex> l(box->mutex); c.playback_revision = box->audio_revision; }
        return c;
    };
    // Prefetch not reconciled (handoff/seek window): stays pending, never fakes ON.
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = false; }
    assert(box->send(toggle(true)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_pending; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->shuffle_pending && !box->shuffle_enabled && box->title == "B" && box->next_title == "D1"); }
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = true; }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_enabled && !box->shuffle_pending; }));
    std::string next_title;
    {
        std::lock_guard<std::mutex> l(player->m);
        assert(player->replacements.size() == 1 && player->urls.size() == 1 && player->pauses == 1 && player->seeks == 0);
        std::lock_guard<std::mutex> b(box->mutex);
        assert(box->title == "B" && !box->shuffle_error); // current/pause untouched
        next_title = box->next_title;
        const auto &url = player->replacements[0];
        const std::string id = next_title == "A" ? "a" : next_title == "C" ? "c" : "dup";
        assert(url.find("id=" + id + "&") != std::string::npos || url.find("id=" + id) + id.size() + 3 == url.size());
    }
    // OFF restores canonical successor of current occurrence, not replayed A.
    assert(box->send(toggle(false)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return !box->shuffle_enabled && !box->shuffle_pending; }));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->title == "B" && box->next_title == "D1"); }
    { std::lock_guard<std::mutex> l(player->m); assert(player->replacements.size() == 2 && player->urls.size() == 1); }
    // Stale snapshot across context replacement/Next/Stop is rejected at admission.
    auto stale = toggle(true);
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(!box->send(stale));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "D1"; }));
    assert(box->send(toggle(true)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_enabled; }));
    stale = toggle(false);
    assert(box->send(play)); // Replacement context resets shuffle to its own state.
    assert(!box->send(stale));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "B" && !box->shuffle_enabled; }));
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = false; }
    assert(box->send(toggle(true)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_pending; }));
    // Revision-changing Next cancels old pending intent even if no decoder restart succeeds.
    { Command c; c.kind = Command::Next; c.generation = g; assert(box->send(c)); }
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->title == "D1" && !box->shuffle_pending && !box->shuffle_enabled; }));
    assert(box->send(toggle(true)));
    assert(eventually([&] { std::lock_guard<std::mutex> l(box->mutex); return box->shuffle_pending; }));
    { Command c; c.kind = Command::Stop; c.generation = g; assert(box->send(c)); }
    { std::lock_guard<std::mutex> l(player->m); player->replace_ok = true; }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    { std::lock_guard<std::mutex> l(box->mutex); assert(box->song_id.empty() && !box->shuffle_enabled && !box->shuffle_pending); }
    box->reset();
    { std::lock_guard<std::mutex> l(box->mutex); assert(!box->shuffle_enabled && !box->shuffle_pending); box->quit = true; box->wake.notify_all(); box->browse_wake.notify_all(); }
}
}
int main()
{
    { // Ranked snapshot cached once; optional failures retain albums; stale never cached.
        int top_calls = 0;
        bool supported = true, mismatch = false, cancelled = false;
        subsonic::Client client("https://example.invalid", "u", "p", [&](const std::string &url) {
            if (url.find("getArtist?") != std::string::npos)
                return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","artist":{"id":"a","name":"Name","album":[{"id":"album"}]}}})", ""};
            assert(url.find("getTopSongs?") != std::string::npos); ++top_calls;
            if (!supported) return subsonic::HttpResponse{404, "", ""};
            return subsonic::HttpResponse{200, mismatch ? R"({"subsonic-response":{"status":"ok","topSongs":{"song":[{"id":"dup","artistId":"wrong"}]}}})" :
                R"({"subsonic-response":{"status":"ok","topSongs":{"song":[{"id":"dup","artistId":"a"},{"id":"dup","artistId":"a"}]}}})", ""};
        });
        Request r; r.view = View::ArtistAlbums; r.parent_id = "a";
        Library library; Page p;
        assert(library.load(client, r, p) && p.top_songs_ok && p.top_songs.size() == 2 && p.items.size() == 1);
        assert(library.load(client, r, p) && top_calls == 1);
        PlayIntent intent; intent.context = r.view; intent.context_id = "a"; intent.song_id = "dup";
        intent.occurrence = 1; intent.page_ids = {"dup", "dup"}; intent.top_snapshot = p.top_songs;
        app::PlayQueue q;
        assert(library.queue(client, intent, q) && q.current_index() == 1 && q.tracks().size() == 2 && top_calls == 1);
        assert(!library.queue(client, intent, q, [] { return true; }) && q.current_index() == 1);
        intent.page_ids[1] = "changed"; assert(!library.queue(client, intent, q));
        for (bool collision : {false, true}) {
            Library fallback; supported = collision; mismatch = collision;
            assert(fallback.load(client, r, p) && p.items.size() == 1 && !p.top_songs_ok && p.top_songs.empty());
        }
        Library stale; supported = true; mismatch = false;
        int checks = 0;
        assert(!stale.load(client, r, p, [&] { cancelled = ++checks == 2; return cancelled; }));
        assert(cancelled);
        const int before = top_calls;
        assert(stale.load(client, r, p) && p.top_songs_ok && top_calls == before + 1);
    }
    assert(valid_server("https://example.invalid/music"));
    assert(valid_server("http://127.0.0.1:1"));
    for (const auto *s : {"", "ftp://example.invalid", "https://", "https://user@example.invalid", "https://example.invalid?q=1", "https://example.invalid/#x", "https://bad host", "https://:80"}) assert(!valid_server(s));
    std::string input = " \thttps://example.invalid/music\r\n";
    assert(prepare_server(input) == ServerInput::Valid && input == "https://example.invalid/music");
    input = " HTTPS://example.invalid ";
    assert(prepare_server(input) == ServerInput::Valid && input == "https://example.invalid");
    assert(valid_server("https://[::1]:443/music"));
    for (const auto *s : {"http://host:0", "http://host:65536", "http://host:abc", "http://[::1", "http://a:b:c"}) assert(!valid_server(s));
    input = " example.invalid:8090/music ";
    assert(prepare_server(input) == ServerInput::NeedsScheme && input == "example.invalid:8090/music");
    for (const auto *s : {"ftp://example.invalid", "file:///etc/passwd", "", "https://bad host", "user@example.invalid"}) {
        input = s; assert(prepare_server(input) == ServerInput::Invalid);
    }
    using F = subsonic::LoginFailure;
    auto check_failure = [](subsonic::HttpResponse response, F expected) {
        subsonic::Client c("https://example.invalid", "fixture", "fixture", [response](const std::string &) { return response; });
        subsonic::ServerInfo info; std::string error; F failure;
        assert(!c.ping(&info, &error, &failure) && failure == expected);
        assert(error.find("SECRET") == std::string::npos);
        assert(std::string(login_failure_key(failure)).find("SECRET") == std::string::npos);
    };
    check_failure({401, "SECRET", "SECRET"}, F::Http401);
    check_failure({403, "SECRET", "SECRET"}, F::Http403);
    check_failure({200, R"({"subsonic-response":{"status":"failed","error":{"code":40,"message":"SECRET"}}})", {}}, F::Authentication);
    check_failure({200, R"({"subsonic-response":{"status":"failed","error":{"code":41,"message":"SECRET"}}})", {}}, F::Authentication);
    check_failure({200, "SECRET", {}}, F::Response);
    check_failure({404, "SECRET", "SECRET"}, F::Http);
    check_failure({0, {}, "SECRET"}, F::Transport);
    for (F failure : {F::Connection, F::Timeout, F::CaBundle, F::Untrusted, F::Tls, F::Initialization, F::Configuration, F::Redirect})
        check_failure({0, {}, "SECRET", failure}, failure);
    int artists = 0, details = 0, albums = 0, playlists = 0;
    subsonic::Client client("https://example.invalid", "fixture", "fixture", [&](const std::string &url) {
        std::string body;
        if (url.find("search3?") != std::string::npos) {
            ++artists;
            assert(url.find("query=&") != std::string::npos && url.find("artistCount=200") != std::string::npos);
            assert(url.find("albumCount=0&songCount=0") != std::string::npos);
            const bool second = url.find("artistOffset=40&") != std::string::npos;
            assert(second || url.find("artistOffset=") == std::string::npos);
            body = "\"searchResult3\":{\"artist\":[";
            for (int i = second ? 40 : 0; i < 41; ++i) { // Short batch: whole library.
                if (i != (second ? 40 : 0)) body += ',';
                body += "{\"id\":\"opaque:" + std::to_string(i) + "\",\"name\":\"Bjørk 東京\",\"albumCount\":3";
                if (i % 2 == 0) body += ",\"coverArt\":\"artist-cover\"";
                body += '}';
            }
            body += "]}";
        } else if (url.find("getArtist?") != std::string::npos) {
            ++details; body = "\"artist\":{\"id\":\"opaque:0\",\"album\":[{\"id\":\"album\",\"name\":\"Album\"}]}";
        } else if (url.find("getAlbumList2?") != std::string::npos) {
            ++albums; body = "\"albumList2\":{\"album\":[{\"id\":\"album\",\"name\":\"Album\"}]}";
        } else if (url.find("getPlaylists?") != std::string::npos) {
            ++playlists; body = "\"playlists\":{\"playlist\":[{\"id\":\"list\",\"name\":\"List\"}]}";
        } else if (url.find("getPlaylist?") != std::string::npos) {
            body = "\"playlist\":{\"id\":\"list\",\"entry\":[{\"id\":\"song\",\"title\":\"First\"},{\"id\":\"song\",\"title\":\"Repeat\"}]}";
        } else if (url.find("getAlbum?") != std::string::npos) {
            body = "\"album\":{\"id\":\"album\",\"name\":\"Real album\",\"artist\":\"Album artist\",\"artistId\":\"album-artist\",\"coverArt\":\"album-cover\",\"year\":2026,\"genre\":\"Rock\",\"songCount\":3,\"duration\":300,\"song\":[{\"id\":\"song\",\"title\":\"First\",\"artist\":\"Album artist\"},{\"id\":\"guest\",\"artist\":\"Guest\",\"artistId\":\"guest-artist\"},{\"id\":\"unknown\",\"artist\":\"Other\"}]}";
        } else assert(false);
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    });
    int recent_calls = 0, newest_calls = 0;
    bool fail_recent = true;
    subsonic::Client home_client("https://example.invalid", "fixture", "fixture", [&](const std::string &url) {
        assert(url.find("size=6") != std::string::npos && url.find("offset=0") != std::string::npos);
        if (url.find("type=recent&") != std::string::npos) {
            ++recent_calls;
            if (fail_recent) return subsonic::HttpResponse{503, "", {}};
        } else { assert(url.find("type=newest&") != std::string::npos); ++newest_calls; }
        return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","albumList2":{"album":[{"id":"same","name":"Real album","artist":"Real artist","coverArt":"cover"}]}}})", {}};
    });
    Library home_library; Page home_page; Request home_request;
    assert(home_library.load(home_client, home_request, home_page));
    assert(home_page.sections[0].ok && home_page.sections[0].count == 0 && home_page.sections[1].ok && home_page.items.size() == 1);
    assert(home_page.items[0].subtitle == "Real artist" && !home_page.items[0].artwork_url.empty());
    fail_recent = false; home_request.home_retry = 1;
    assert(home_library.load(home_client, home_request, home_page));
    assert(recent_calls == 0 && newest_calls == 1 && home_page.sections[0].count == 0);
    app::RecentContexts history;
    history.promote({Kind::Artist, "same", "Astrid", {}, "artist-cover"});
    history.promote({Kind::Playlist, "same", "Playlist", {}, ""});
    history.promote({Kind::Album, "same", "Album", {}, "album-cover"});
    history.promote({Kind::Artist, "same", "Astrid", {}, "artist-cover"});
    assert(history.items().size() == 3 && history.items()[0].kind == Kind::Artist && history.items()[1].kind == Kind::Album && history.items()[2].kind == Kind::Playlist);
    home_request.home_retry = 0;
    assert(home_library.load(home_client, home_request, home_page, {}, history.items()));
    assert(home_page.sections[0].count == 3 && home_page.items[0].kind == Kind::Artist && home_page.items[2].kind == Kind::Playlist);
    assert(home_page.items[0].artwork_url == home_client.cover_art_url("artist-cover", 720));
    assert(home_page.items[2].artwork_url.empty());
    Library library; Page page; Request r; r.view = View::Artists;
    assert(library.load(client, r, page) && page.items.size() == 40 && page.next_cursor == "40");
    assert(page.items[0].count == 3 && !page.items[0].artwork_url.empty());
    assert(page.items[0].title == "Bjørk 東京" && page.items[1].artwork_url.empty());
    assert(client.cover_art_url("", 720).empty());
    r.cursor = page.next_cursor;
    assert(library.load(client, r, page) && page.items.size() == 1 && page.items[0].id == "opaque:40" && page.start_index == 40 && artists == 2);
    // Simulated raw search3 library: rows [offset, offset+count) of `total`; albums(i)
    // decides album artists (0 = composer/track-only/featured).
    int raw_calls = 0;
    std::vector<int> offsets;
    auto sim = [&](int total, std::function<int(int)> albums) {
        return subsonic::Client("https://example.invalid", "fixture", "fixture", [&, total, albums](const std::string &url) {
            ++raw_calls;
            assert(url.find("artistCount=200&albumCount=0&songCount=0") != std::string::npos);
            const auto at = url.find("artistOffset=");
            const int start = at == std::string::npos ? 0 : std::stoi(url.substr(at + 13));
            offsets.push_back(start);
            std::string body = "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"artist\":[";
            for (int i = start; i < std::min(start + 200, total); ++i) {
                if (i != start) body += ',';
                body += "{\"id\":\"artist:" + std::to_string(i) + "\",\"name\":\"Å 東京\",\"albumCount\":" + std::to_string(albums(i)) + "}";
            }
            return subsonic::HttpResponse{200, body + "]}}}", {}};
        });
    };
    auto id_at = [](const Page &p, size_t i) { return std::stoi(p.items[i].id.substr(7)); };
    Request large; large.view = View::Artists;
    Page large_page;
    // Sparse (~85% zero-album): every 7th row is an album artist. 40 cards need rows
    // 0..273, spanning two batches; cursor is raw row after card 40, mid-batch.
    auto sparse_albums = [](int i) { return i % 7 == 0 ? 2 : 0; };
    auto sparse = sim(4097, sparse_albums);
    std::vector<int> seen;
    large.cursor.clear(); raw_calls = 0; offsets.clear();
    assert(library.load(sparse, large, large_page) && large_page.items.size() == 40);
    assert(raw_calls == 2 && offsets == (std::vector<int>{0, 200}));
    assert(id_at(large_page, 39) == 273 && large_page.next_cursor == "274" && large_page.start_index == 0);
    for (size_t i = 0; i < 40; ++i) seen.push_back(id_at(large_page, i));
    // Next page resumes at mid-batch leftover row 274: no skip, no duplicate.
    large.cursor = large_page.next_cursor; offsets.clear();
    assert(library.load(sparse, large, large_page) && large_page.items.size() == 40 && offsets.front() == 274);
    assert(id_at(large_page, 0) == 280 && large_page.start_index == 274);
    for (size_t i = 0; i < 40; ++i) seen.push_back(id_at(large_page, i));
    for (size_t i = 0; i < seen.size(); ++i) assert(seen[i] == int(i) * 7 && sparse_albums(seen[i]) == 2);
    // Budget: all-zero library spends exactly 5 raw requests (1,000 rows), keeps cursor.
    auto barren = sim(100000, [](int) { return 0; });
    large.cursor.clear(); raw_calls = 0;
    assert(library.load(barren, large, large_page) && large_page.items.empty());
    assert(raw_calls == 5 && large_page.next_cursor == "1000");
    // Budget with partial cards returns what it has and still a cursor.
    auto thin = sim(100000, [](int i) { return i % 100 == 0 ? 1 : 0; });
    large.cursor.clear(); raw_calls = 0;
    assert(library.load(thin, large, large_page) && large_page.items.size() == 10);
    assert(raw_calls == 5 && large_page.next_cursor == "1000");
    // Short batch ends list, even with fewer than 40 cards; tail page from mid-batch cursor.
    auto tail = sim(250, sparse_albums);
    large.cursor = "210"; raw_calls = 0;
    assert(library.load(tail, large, large_page) && raw_calls == 1 && large_page.next_cursor.empty());
    assert(large_page.items.size() == 6 && id_at(large_page, 0) == 210 && id_at(large_page, 5) == 245);
    large.cursor = "4120"; assert(library.load(sparse, large, large_page) && large_page.items.empty() && large_page.next_cursor.empty());
    // Exactly 40 cards ending a full batch keeps a cursor; ending a short batch does not.
    auto exact = sim(400, [](int i) { return i >= 160 && i < 200 ? 1 : 0; });
    large.cursor.clear(); raw_calls = 0;
    assert(library.load(exact, large, large_page) && large_page.items.size() == 40 && raw_calls == 1);
    assert(id_at(large_page, 39) == 199 && large_page.next_cursor == "200");
    large.cursor = "200";
    assert(library.load(exact, large, large_page) && large_page.items.empty() && large_page.next_cursor.empty());
    // Stale: newer browse/logout abandons scan before next raw request.
    int stale_checks = 0; raw_calls = 0; large.cursor.clear();
    assert(!library.load(sparse, large, large_page, [&] { return ++stale_checks > 1; }));
    assert(raw_calls == 1 && large_page.items.empty());
    raw_calls = 0;
    assert(!library.load(sparse, large, large_page, [] { return true; }) && raw_calls == 0);
    for (const char *payload : {"{\"name\":\"No ID\"}", "{\"id\":\"ok\"}"}) {
        subsonic::Client malformed("https://example.invalid", "fixture", "fixture", [&](const std::string &) {
            std::string body = "{\"subsonic-response\":{\"status\":\"ok\",\"searchResult3\":{\"artist\":[";
            const int count = std::string(payload).find("No ID") != std::string::npos ? 1 : 201;
            for (int i = 0; i < count; ++i) { if (i) body += ','; body += payload; }
            return subsonic::HttpResponse{200, body + "]}}}", {}};
        });
        large.cursor.clear(); assert(!library.load(malformed, large, large_page));
        assert(large_page.items.empty());
    }
    r.cursor = "-1"; assert(!library.load(client, r, page));
    r.cursor = "999999999999999999"; assert(!library.load(client, r, page));
    r.cursor.clear(); r.view = View::ArtistAlbums; r.parent_id = "opaque:0";
    assert(library.load(client, r, page) && page.items[0].kind == Kind::Album && details == 1);
    r.view = View::AlbumTracks; r.parent_id = "album";
    assert(library.load(client, r, page) && page.items[0].kind == Kind::Track);
    assert(page.detail.title == "Real album" && page.detail.creator == "Album artist" && page.detail.year == 2026 && page.detail.genre == "Rock" && page.detail.count == 3 && page.detail.duration == 300);
    assert(page.detail.artwork_url == page.items[0].artwork_url && !page.detail.artwork_url.empty());
    assert(page.items[0].artist_id == "album-artist" && page.items[1].artist_id == "guest-artist" && page.items[2].artist_id.empty());
    PlayIntent intent; intent.context_id = "album";
    app::PlayQueue metadata_queue;
    assert(library.queue(client, intent, metadata_queue));
    assert(metadata_queue.current()->artist_id == "album-artist");
    assert(library.context.kind == Kind::Album && library.context.id == "album" && library.context.artwork_url == "album-cover");
    assert(library.queue(client, intent, metadata_queue) && library.context.id == "album"); // Cached detail keeps exact context.
    r.view = View::PlaylistTracks; r.parent_id = "list";
    assert(library.load(client, r, page) && page.items.size() == 2 && page.items[0].id == page.items[1].id && page.items[1].title == "Repeat");
    r.view = View::Home; assert(library.load(client, r, page) && albums == 1);
    assert(page.sections.size() == 2 && page.items.size() == 1 && page.sections[0].count == 0);
    r.view = View::Albums; assert(library.load(client, r, page) && albums == 2);
    r.view = View::Playlists; assert(library.load(client, r, page) && playlists == 1);
    assert(page.items[0].count == -1 && page.items[0].artwork_url.empty()); // Absent never becomes fake zero.
    subsonic::Client cards_client("https://example.invalid", "fixture", "fixture", [](const std::string &) {
        return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","playlists":{"playlist":[{"id":"covered","name":"Covered","coverArt":"cover","songCount":7}]}}})", {}};
    });
    Library cards_library;
    assert(cards_library.load(cards_client, r, page) && page.items[0].title == "Covered" && page.items[0].count == 7 && !page.items[0].artwork_url.empty());

    int playlist_fetches = 0;
    subsonic::Client ordered("https://example.invalid", "fixture", "fixture", [&](const std::string &url) {
        assert(url.find("getPlaylist?") != std::string::npos); ++playlist_fetches;
        std::string body = "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"id\":\"ordered\",\"name\":\"Real playlist\",\"owner\":\"Owner\",\"coverArt\":\"list-cover\",\"songCount\":42,\"duration\":420,\"entry\":[";
        for (int i = 0; i < 42; ++i) {
            if (i) body += ',';
            body += "{\"id\":\"dup\",\"title\":\"Occurrence " + std::to_string(i) + "\"}";
        }
        return subsonic::HttpResponse{200, body + "]}}}", {}};
    });
    Library ordered_library; Request ordered_request; ordered_request.view = View::PlaylistTracks; ordered_request.parent_id = "ordered";
    Page ordered_page;
    assert(ordered_library.load(ordered, ordered_request, ordered_page));
    assert(ordered_page.detail.title == "Real playlist" && ordered_page.detail.creator == "Owner" && ordered_page.detail.count == 42 && ordered_page.detail.duration == 420);
    assert(ordered_page.items[0].artwork_url == ordered_page.detail.artwork_url && !ordered_page.detail.artwork_url.empty());
    ordered_request.cursor = ordered_page.next_cursor;
    assert(ordered_library.load(ordered, ordered_request, ordered_page) && ordered_page.items.size() == 2 && playlist_fetches == 1);
    assert(ordered_page.detail.title == "Real playlist" && ordered_page.detail.count == 42);
    PlayIntent all; all.context = View::PlaylistTracks; all.context_id = "ordered"; all.page_offset = 40; all.page_ids = {"dup", "dup"};
    app::PlayQueue full;
    assert(ordered_library.queue(ordered, all, full) && full.current()->title == "Occurrence 0");
    for (int i = 1; i < 42; ++i) { assert(full.next()); assert(full.current()->title == "Occurrence " + std::to_string(i)); }
    assert(!full.next() && playlist_fetches == 1);
    all.action = PlayAction::DetailShuffle;
    std::set<std::string> starts;
    for (int trial = 0; trial < 24; ++trial) {
        assert(ordered_library.queue(ordered, all, full));
        starts.insert(full.current()->title);
        std::set<std::string> occurrences{full.current()->title};
        while (full.next()) assert(occurrences.insert(full.current()->title).second);
        assert(occurrences.size() == 42); // Duplicate IDs remain distinct occurrences.
    }
    assert(starts.size() > 1); // Initial occurrence is randomized, not pinned at zero.
    assert(ordered_library.queue(ordered, all, full));
    std::mt19937 rng(1);
    full.set_shuffle(false, rng);
    assert(full.tracks()[0].title == "Occurrence 0" && full.tracks()[41].title == "Occurrence 41");
    assert(all.context == View::PlaylistTracks && all.context_id == "ordered");
    all.action = PlayAction::Detail;
    all.song_id = "dup"; all.occurrence = 41;
    assert(ordered_library.queue(ordered, all, full) && full.current()->title == "Occurrence 41");
    all.page_ids[1] = "changed"; assert(!ordered_library.queue(ordered, all, full));
    all.page_ids[1] = "dup"; all.song_id = "wrong"; assert(!ordered_library.queue(ordered, all, full));
    ordered_request.cursor = "99";
    assert(!ordered_library.load(ordered, ordered_request, ordered_page) && ordered_page.detail.title.empty());

    // Search: one bounded search3 (songs 5, albums 5, artists 5), cached local playlist filter.
    int search_calls = 0, list_calls = 0;
    bool search_fail = false, search_empty = false;
    std::string last_search;
    subsonic::Client finder("https://example.invalid", "fixture", "fixture", [&](const std::string &url) {
        std::string body;
        if (url.find("search3?") != std::string::npos) {
            ++search_calls; last_search = url;
            if (search_fail) return subsonic::HttpResponse{503, "", {}};
            body = R"("searchResult3":{"artist":[{"id":"ar","name":"Artist","albumCount":1,"coverArt":"portrait"},{"id":"ar","name":"dup"}],"song":[{"id":"s1","title":"Bjørk one","coverArt":"c"},{"id":"s1","title":"dup"},{"id":"s2","title":"two"}],"album":[{"id":"a1","name":"Album"},{"id":"a1","name":"dup"}]})";
            if (search_empty) body = R"("searchResult3":{})";
        } else if (url.find("getPlaylists?") != std::string::npos) {
            ++list_calls;
            body = R"("playlists":{"playlist":[{"id":"p1","name":"My BJØRK mix"},{"id":"p2","name":"Other"},{"id":"p3","name":"bjørk 2"},{"id":"p1","name":"dup"}]})";
        } else assert(false);
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"," + body + "}}", {}};
    });
    Library find_library; Request find; find.view = View::Search; find.query = "Bjørk"; Page found;
    assert(find_library.load(finder, find, found) && search_calls == 1 && list_calls == 1);
    assert(last_search.find("artistCount=5&albumCount=5&songCount=5") != std::string::npos && last_search.find("query=Bj%C3%B8rk") != std::string::npos);
    assert(found.items.size() == 5 && found.items[0].id == "s1" && found.items[1].id == "s2" && found.items[2].id == "ar" && found.items[3].id == "a1" && found.items[4].id == "p3");
    assert(found.items[0].kind == Kind::Track && found.items[2].kind == Kind::Artist && found.items[3].kind == Kind::Album && found.items[4].kind == Kind::Playlist && found.next_cursor.empty());
    assert(found.items[2].count == 1 && found.items[2].artwork_url.find("portrait") != std::string::npos);
    find.query = "b";
    assert(find_library.load(finder, find, found) && search_calls == 2 && list_calls == 1);
    assert(last_search.find("query=b&artistCount=5&albumCount=5&songCount=5") != std::string::npos);
    assert(found.items.size() == 4 && found.items[0].kind == Kind::Track && found.items[2].kind == Kind::Artist &&
           found.items[3].kind == Kind::Album); // Guard affects playlists only, never server results.
    search_empty = true; // Navidrome short-query response; one search3 per query, no index refetch.
    for (const char *query : {"b", " B* ", "*"}) {
        find.query = query;
        const int calls = search_calls;
        assert(find_library.load(finder, find, found) && found.items.empty());
        assert(search_calls == calls + 1 && list_calls == 1);
    }
    for (const char *query : {"BJ", " bj* "}) {
        find.query = query;
        const int calls = search_calls;
        assert(find_library.load(finder, find, found) && found.items.size() == 2);
        assert(found.items[0].id == "p1" && found.items[1].id == "p3");
        assert(search_calls == calls + 1 && list_calls == 1);
    }
    find.query = "ø"; // One code point, two UTF-8 bytes: eligible; no Unicode fold/unaccent.
    assert(find_library.load(finder, find, found) && found.items.size() == 1 && found.items[0].id == "p3" && list_calls == 1);
    search_empty = false;
    find.query = "bjø"; // ASCII fold only: "BJØ" != "bjø"; non-ASCII bytes exact.
    assert(find_library.load(finder, find, found) && list_calls == 1 && found.items.back().id == "p3");
    find.query = "mix";
    assert(find_library.load(finder, find, found) && found.items.back().id == "p1" && list_calls == 1);
    for (const char *bad : {"", "   ", "a\nb", "\xc0\xaf"}) {
        find.query = bad;
        const int calls = search_calls;
        assert(!find_library.load(finder, find, found) && found.items.empty());
        assert(search_calls == calls && list_calls == 1);
    }
    find.query = "mix"; find.cursor = "5"; assert(!find_library.load(finder, find, found)); find.cursor.clear();
    const int before = search_calls;
    assert(!find_library.load(finder, find, found, [] { return true; }) && search_calls == before); // Stale before I/O.
    search_fail = true; assert(!find_library.load(finder, find, found)); search_fail = false;
    Library other_account; find.query = "mix";
    assert(other_account.load(finder, find, found) && list_calls == 2); // Fresh account never reuses index.
    PlayIntent pick; pick.context = View::Search; pick.context_id = "Bjørk"; pick.song_id = "s2"; pick.occurrence = 1; pick.page_ids = {"s1", "s2"};
    app::PlayQueue search_queue;
    find_library.context = {Kind::Album, "stale", "Stale", {}, {}};
    assert(find_library.queue(finder, pick, search_queue) && search_queue.current()->id == "s2" && search_queue.tracks().size() == 2);
    assert(find_library.context.id.empty()); // Search song has no recent context to promote.
    assert(!search_queue.next()); // Context is visible song group only.
    pick.page_ids = {"s2", "s1"}; assert(!find_library.queue(finder, pick, search_queue)); // Changed results refuse.
    pick.page_ids = {"s1", "s2"}; pick.song_id = "s1"; assert(!find_library.queue(finder, pick, search_queue));

    auto box = std::make_shared<Mailbox>();
    auto generation = box->reset();
    Command c; c.kind = Command::Browse; c.generation = generation;
    assert(box->send(c));
    box->reset(); assert(!box->send(c) && box->commands.size() == 1);
    c.generation = box->generation;
    for (int i = 0; i < 16; ++i) assert(box->send(c)); // Latest browse wins, separate from Logout.
    assert(box->browse_commands.size() == 1); box->reset();
    // Hold ping in flight, invalidate account, then release. Old login must not publish.
    std::mutex gate; std::condition_variable wake; bool entered = false, release = false;
    assert(start_worker(box, [&](const std::string &url) {
        assert(url.find("apiKey=fixture-key") != std::string::npos);
        assert(url.find("&p=") == std::string::npos && url.find("&t=") == std::string::npos);
        assert(url.find("discard-password") == std::string::npos);
#ifdef __APPLE__
        assert(pthread_get_stacksize_np(pthread_self()) >= (1u << 20));
#endif
        std::unique_lock<std::mutex> lock(gate); entered = true; wake.notify_one();
        wake.wait(lock, [&] { return release; });
        jf::curl_diag.code = 7;
        jf::curl_diag.os = 22;
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"}}", {}};
    }));
    c = {}; c.kind = Command::Login; c.generation = box->generation;
    c.server = "https://example.invalid"; c.username = "fixture";
    c.password = "discard-password"; c.api_key = "fixture-key";
    assert(box->send(c));
    { std::unique_lock<std::mutex> lock(gate); assert(wake.wait_for(lock, std::chrono::seconds(3), [&] { return entered; })); }
    box->reset();
    { std::lock_guard<std::mutex> lock(gate); release = true; wake.notify_one(); }
    // FIFO barrier: next-generation login completes only after stale login cleanup.
    c.generation = box->generation; assert(box->send(c));
    bool completed = false;
    for (int i = 0; i < 300 && !completed; ++i) {
        { std::lock_guard<std::mutex> lock(box->mutex);
          if (!box->results.empty()) { assert(box->results.size() == 1 && box->results.front().generation == c.generation && box->results.front().ok); assert(box->results.front().diagnostic.code == 7 && box->results.front().diagnostic.os == 22); completed = true; }
        }
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(completed);
    { std::lock_guard<std::mutex> lock(box->mutex); box->quit = true; box->wake.notify_one(); }
    // Failed key auth must issue exactly one request, never retry with password.
    auto failed_box = std::make_shared<Mailbox>();
    std::atomic<int> key_requests{0};
    assert(start_worker(failed_box, [&](const std::string &url) {
        ++key_requests;
        assert(url.find("apiKey=fixture-key") != std::string::npos);
        assert(url.find("&p=") == std::string::npos && url.find("&t=") == std::string::npos);
        return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"failed","error":{"code":40,"message":"fixture"}}})", {}};
    }));
    c.generation = failed_box->generation;
    assert(failed_box->send(c));
    completed = false;
    for (int i = 0; i < 300 && !completed; ++i) {
        { std::lock_guard<std::mutex> lock(failed_box->mutex);
          if (!failed_box->results.empty()) {
              assert(!failed_box->results.front().ok && failed_box->results.front().failure == F::Authentication);
              completed = true;
          }
        }
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(completed && key_requests == 1);
    { std::lock_guard<std::mutex> lock(failed_box->mutex); failed_box->quit = true; failed_box->wake.notify_one(); }
    // Captured gate stays alive until worker leaves its last HTTP call; barrier above proves that.
    assert(same_origin_portrait("https://example.invalid/music", "https://EXAMPLE.invalid:443/portrait.jpg?size=100"));
    assert(same_origin_portrait("http://example.invalid:8080", "http://example.invalid:8080/portrait"));
    for (const auto *url : {"https://other.invalid/p", "http://example.invalid/p", "https://user@example.invalid/p", "file:///p", "//example.invalid/p", "https://example.invalid:444/p", "https://example.invalid/p\\x", "https://example.invalid/p#x"})
        assert(!same_origin_portrait("https://example.invalid", url));
    tab_preload_success();
    tab_preload();
    recent_history();
    playback_snapshot();
    portrait_worker();
    artist_queue();
    history_worker();
    worker_races();
    previous_worker();
    shuffle_worker();
}
