/* Navi5 — offline music navigation checks; fixtures are test-only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/music_state.h"
#include "ui/player_state.h"
#include <cassert>
#include <iostream>

using namespace ui::music;
static Item fixture(Kind kind, const std::string &id)
{
    return {kind, id, "Offline test fixture", "Not connected", "", 123};
}
static Request request(State &s)
{
    auto r = s.take_request();
    assert(r && r->limit == 40);
    assert(!s.take_request());
    return *r;
}
int main()
{
    ui::PlayerState shuffle;
    shuffle.open(0);
    shuffle.direction(1, 1); assert(shuffle.focus == 2);
    shuffle.direction(1, 2); assert(shuffle.focus == 4);
    assert(shuffle.toggle_shuffle(3, false));
    assert(!shuffle.toggle_shuffle(4, true));
    assert(!shuffle.toggle_shuffle(10, false)); // Idle Cross wakes only.
    assert(shuffle.toggle_shuffle(11, false));
    shuffle.navigate(0, 1, 12, true, 2, 30); assert(shuffle.focus == 3);
    shuffle.navigate(1, 0, 13, true, 2, 30); assert(shuffle.preview && shuffle.target == 12);
    shuffle.navigate(0, -1, 14, true, 2, 30); assert(shuffle.focus == 1 && !shuffle.preview);
    shuffle.direction(1, 15); shuffle.direction(1, 16);
    shuffle.direction(-1, 17); assert(shuffle.focus == 2);
    shuffle.dismiss(); assert(!shuffle.toggle_shuffle(18, false));
    ui::PlayerState wake;
    wake.open(0); wake.navigate(0, 1, 6, true, 2, 30); assert(wake.focus == 1);
    wake.navigate(0, 1, 7, true, 2, 30); assert(wake.focus == 3);
    wake.navigate(1, 0, 13, true, 2, 30); assert(!wake.preview); // Wake only.
    wake.navigate(1, 0, 14, false, 2, 30); assert(!wake.preview); // Nonseekable.
    wake.navigate(1, 0, 15, true, 2, 30); assert(wake.preview && wake.target == 12);
    wake.navigate(0, -1, 16, true, 2, 30); assert(!wake.preview && wake.focus == 1);
    ui::PlayerState seek;
    seek.sync(4, 0, true); seek.focus = 3;
    seek.adjust(-1, 2, 30); assert(seek.preview && seek.target == 0);
    seek.adjust(1, 2, 30); assert(seek.target == 10);
    seek.adjust(100, 2, 30); assert(seek.target < 30 && seek.target > 29.99);
    seek.sync(4, 1, true); assert(!seek.preview && seek.focus == 1);
    seek.focus = 3; seek.adjust(1, 2, 30); seek.sync(5, 1, true); assert(!seek.preview);
    seek.adjust(1, 2, 30); seek.sync(5, 1, false); assert(!seek.preview);
    assert(ui::PlayerState::seekable(30, 2, true, false)); // Playing or paused.
    assert(!ui::PlayerState::seekable(0, 2, true, false));
    assert(!ui::PlayerState::seekable(INFINITY, 2, true, false));
    assert(!ui::PlayerState::seekable(30, NAN, true, false));
    assert(!ui::PlayerState::seekable(30, 30, true, false));
    assert(!ui::PlayerState::seekable(30, 2, true, true));
    assert(!ui::PlayerState::seekable(30, 2, false, false));
    {
        State mixed; mixed.activate(); auto r = request(mixed);
        Page home; home.items = {fixture(Kind::Artist, "artist"), fixture(Kind::Playlist, "list"), fixture(Kind::Album, "album")};
        home.sections = {{0, 3, true}, {3, 0, true}};
        assert(mixed.complete(r.token, home));
        for (const auto view : {View::ArtistAlbums, View::PlaylistTracks, View::AlbumTracks}) {
            mixed.open(); auto detail = request(mixed); assert(detail.view == view);
            assert(mixed.complete(detail.token, {})); assert(mixed.back());
            if (view != View::AlbumTracks) assert(mixed.move(1));
        }
        mixed.history_changed(); r = request(mixed); assert(r.view == View::Home);
        assert(mixed.complete(r.token, home)); mixed.open(); r = request(mixed);
        assert(mixed.complete(r.token, {})); mixed.history_changed(); assert(mixed.back());
        assert(request(mixed).view == View::Home); // Changed history invalidates saved parent page.
    }
    State s;
    s.activate();
    auto home = request(s);
    assert(home.view == View::Home && s.status() == Status::Loading);
    assert(!s.move(1));
    s.open();
    assert(!s.take_play_intent());
    s.tab(1);
    auto artists = request(s);
    assert(artists.view == View::Artists);
    assert(!s.complete(home.token, {}));
    assert(s.complete(artists.token, {{fixture(Kind::Artist, "artist:opaque/α?x=1")}, "page:2"}));
    assert(s.next());
    auto second = request(s);
    assert(second.cursor == "page:2" && !s.has_previous());
    assert(s.complete(second.token, {{}, ""}));
    assert(!s.next() && !s.previous() && s.page().items.size() == 1); // Empty tail ends list.
    s.open();
    auto albums = request(s);
    assert(albums.view == View::ArtistAlbums && albums.parent_id == "artist:opaque/α?x=1");
    assert(s.complete(albums.token, {{fixture(Kind::Album, "album:opaque")}, ""}));
    s.open();
    auto tracks = request(s);
    assert(tracks.view == View::AlbumTracks && tracks.parent_id == "album:opaque");
    assert(s.complete(tracks.token, {{fixture(Kind::Track, "song:1"), fixture(Kind::Track, "song:2")}, ""}));
    s.play_album();
    const auto album_play = s.take_play_intent();
    assert(album_play && album_play->song_id.empty() && album_play->context_id == "album:opaque");
    assert(!s.take_play_intent());
    assert(!s.move(-1) && s.move(1) && !s.move(1));
    s.open();
    auto play = s.take_play_intent();
    assert(play && play->song_id == "song:2" && play->context_id == "album:opaque");
    assert(!s.take_play_intent());
    assert(s.back());
    assert(!s.take_request() && s.request().view == View::ArtistAlbums && s.status() == Status::Ready);
    s.open();
    albums = request(s); // Album tracks: initial-load failure is whole-view Error.
    assert(s.complete(albums.token, {}, false));
    assert(s.status() == Status::Error);
    s.retry();
    auto retry = request(s);
    assert(retry.token != albums.token && retry.parent_id == albums.parent_id);
    assert(!s.complete(albums.token, {}));
    assert(s.complete(retry.token, {}));
    assert(s.status() == Status::Ready && s.page().items.empty());
    s.open();
    assert(!s.take_request());
    s.tab(3);
    auto playlists = request(s);
    assert(s.complete(playlists.token, {{fixture(Kind::Playlist, "list/opaque")}, ""}));
    s.open();
    auto list = request(s);
    assert(list.view == View::PlaylistTracks && list.parent_id == "list/opaque");
    assert(s.complete(list.token, {{fixture(Kind::Track, "song:3")}, ""}));
    s.open();
    assert(s.take_play_intent()->context == View::PlaylistTracks);
    s.tab(2);
    auto root = request(s);
    assert(!s.back());
    assert(s.complete(root.token, {{fixture(Kind::Artist, "wrong-kind")}, ""}));
    assert(s.status() == Status::Error);
    s.retry();
    root = request(s);
    Page oversized;
    oversized.items.assign(41, fixture(Kind::Album, "id"));
    assert(s.complete(root.token, oversized) && s.status() == Status::Error);
    s.retry();
    root = request(s);
    assert(s.complete(root.token, {{fixture(Kind::Album, "")}, ""}) && s.status() == Status::Error);
    s.retry();
    root = request(s);
    assert(s.complete(root.token, {{fixture(Kind::Album, "ok")}, "next"}));
    assert(s.next());
    root = request(s);
    assert(s.complete(root.token, {{fixture(Kind::Album, "ok")}, "next"}));
    assert(s.status() == Status::Error); // repeated cursor cannot loop forever

    // Duplicate IDs on page two: intent carries absolute occurrence + visible order.
    State dup;
    dup.activate(); auto dup_home = request(dup);
    assert(dup.complete(dup_home.token, {{fixture(Kind::Album, "a")}, ""}));
    dup.open(); auto dup_tracks = request(dup);
    Page first_page; first_page.next_cursor = "40";
    for (int i = 0; i < 40; ++i) first_page.items.push_back(fixture(Kind::Track, "s" + std::to_string(i)));
    assert(dup.complete(dup_tracks.token, first_page));
    assert(dup.next()); dup_tracks = request(dup);
    assert(dup_tracks.cursor == "40");
    assert(dup.complete(dup_tracks.token, {{fixture(Kind::Track, "dup"), fixture(Kind::Track, "dup")}, "", 40}));
    assert(dup.page().items.size() == 42 && dup.move(41)); dup.open();
    auto dup_play = dup.take_play_intent();
    assert(dup_play && dup_play->song_id == "dup" && dup_play->occurrence == 41 && dup_play->page_offset == 0);
    assert(dup_play->page_ids.size() == 42 && dup_play->page_ids[40] == "dup" && dup_play->page_ids[41] == "dup");
    assert(dup.move(-41)); dup.open(); assert(dup.take_play_intent()->occurrence == 0);

    {   // Infinite scroll: crossing, failure/retry, stale, bounded window, checkpoint replay.
        auto page = [](size_t at, size_t n, std::string next, Kind kind = Kind::Album) {
            Page p; p.next_cursor = std::move(next);
            for (size_t i = 0; i < n; ++i) p.items.push_back(fixture(kind, "id" + std::to_string(at + i)));
            return p;
        };
        State w; w.tab(2); auto r = request(w);
        assert(w.complete(r.token, page(0, 40, "c40")));
        assert(w.move(39) && !w.take_request());
        assert(w.move(6) && w.busy()); // Crossing end requests, focus waits.
        r = request(w); assert(r.cursor == "c40" && w.selected() == 39);
        assert(w.move(6) && !w.take_request()); // Never a second in-flight request.
        assert(w.complete(r.token, {}, false)); // Transport failure keeps rows.
        assert(w.continuation_error() && w.status() == Status::Ready && w.page().items.size() == 40);
        assert(!w.move(6) && !w.take_request()); // No silent re-dispatch.
        w.retry(); r = request(w); assert(r.cursor == "c40");
        const auto stale = r.token;
        assert(w.complete(r.token, page(40, 40, "c80")) && w.selected() == 45);
        assert(!w.complete(stale, page(40, 40, "c80"))); // Late duplicate ignored.
        for (int n = 2; n < 10; ++n) {
            assert(w.move(int(w.page().items.size()) - 1 - int(w.selected())));
            w.approach(); r = request(w); // Near end: prefetch before focus hits it.
            assert(r.cursor == "c" + std::to_string(n * 40));
            assert(w.complete(r.token, page(n * 40, 40, "c" + std::to_string(n * 40 + 40))));
        }
        assert(w.move(int(w.page().items.size()) - 1 - int(w.selected())));
        assert(w.page().items.size() <= State::window_items && w.page().start_index == 160 && w.has_previous());
        assert(w.absolute() == 399 && w.page().items.back().id == "id399");
        assert(w.move(-int(w.selected())) && w.absolute() == 160);
        assert(w.move(-6)); r = request(w); assert(r.cursor == "c120");
        assert(w.complete(r.token, page(120, 40, "c160")));
        assert(w.absolute() == 154 && w.page().items[w.selected()].id == "id154" && w.page().items.size() <= State::window_items);
        assert(w.page().next_cursor == "c360"); // Evicted tail resumes at checkpoint.
        assert(w.move(-int(w.selected()) + 0) && w.move(-1)); r = request(w);
        assert(w.complete(r.token, page(80, 39, "c120"))); // Server changed underneath.
        assert(w.status() == Status::Error && w.page().items.empty());
        w.retry(); r = request(w); assert(r.cursor.empty());
        assert(w.complete(r.token, page(0, 3, "")));
        assert(!w.move(3) && !w.take_request() && !w.next()); // End of list.

        // Sparse artist cursor: raw offsets stay opaque; displayed occurrences contiguous.
        State a; a.tab(1); r = request(a);
        assert(a.complete(r.token, {{fixture(Kind::Artist, "x"), fixture(Kind::Artist, "y")}, "1000", 0}));
        assert(a.move(1) && a.move(6)); r = request(a); assert(r.cursor == "1000");
        assert(a.complete(r.token, {{}, "2000", 1000})); // Budget expired, zero cards: auto-walk.
        r = request(a); assert(r.cursor == "2000");
        assert(a.complete(r.token, {{fixture(Kind::Artist, "y"), fixture(Kind::Artist, "z")}, "", 2000}));
        assert(a.page().start_index == 0 && a.page().items.size() == 4 && a.absolute() == 3);
        assert(a.page().items[2].id == "y"); // Same ID twice is two occurrences, not deduped.

        // Root tab return and Back restore window/focus without refetch.
        a.open(); r = request(a); assert(r.view == View::ArtistAlbums);
        a.tab(2); r = request(a);
        a.tab(1); assert(!a.take_request() && a.request().view == View::Artists && a.absolute() == 3);
        assert(!a.complete(r.token, {})); // Stale Albums load rejected after tab return.
    }

    {   // Search ignores window/approach; Back from search restores scrolled root window.
        State w; w.tab(2); auto r = request(w);
        Page p; p.next_cursor = "40";
        for (int i = 0; i < 40; ++i) p.items.push_back(fixture(Kind::Album, "a" + std::to_string(i)));
        assert(w.complete(r.token, p));
        for (int i = 0; i < 30; ++i) assert(w.move(1));
        w.search(); assert(w.request().view == View::Search);
        w.approach(); assert(!w.take_request()); // Never continuation from search.
        assert(w.query("ab", 0)); w.tick(1); r = request(w);
        Page found{{fixture(Kind::Track, "s"), fixture(Kind::Album, "x")}, "1"}; // Stray cursor ignored.
        assert(w.complete(r.token, found)); w.approach(); assert(!w.take_request());
        assert(!w.move(2) && w.move(1)); // Results clamp; no extension.
        assert(!w.take_request());
        assert(w.back() && !w.take_request() && w.request().view == View::Albums && w.absolute() == 30);
        w.approach(); r = request(w); assert(r.cursor == "40"); // Root continuation resumes.
        Page tail; tail.start_index = 40; tail.items.push_back(fixture(Kind::Album, "a40"));
        assert(w.complete(r.token, tail) && w.page().items.size() == 41);
        w.open(); r = request(w); assert(r.view == View::AlbumTracks && r.parent_id == "a30");
        Page tracks; for (int i = 0; i < 3; ++i) tracks.items.push_back(fixture(Kind::Track, "t"));
        assert(w.complete(r.token, tracks)); assert(w.move(2)); w.open();
        auto play = w.take_play_intent();
        assert(play && play->context == View::AlbumTracks && play->context_id == "a30" && play->occurrence == 2 &&
               play->page_offset == 0 && play->page_ids.size() == 3);
    }
    State restore;
    restore.activate();
    auto parent = request(restore);
    assert(restore.complete(parent.token, {{fixture(Kind::Album, "a"), fixture(Kind::Album, "b")}, ""}));
    assert(restore.move(1));
    restore.open();
    auto child = request(restore);
    assert(restore.back());
    assert(!restore.take_request()); // Home Back restores snapshot, never refetches.
    assert(!restore.complete(child.token, {}));
    assert(restore.status() == Status::Ready && restore.selected() == 1);
    assert(restore.page().items[1].id == "b");
    restore.open();
    request(restore);
    assert(restore.back());
    assert(!restore.take_request() && restore.selected() == 1);

    // Fullscreen owns presentation only: exact page/token/selection survive dismissal.
    ui::PlayerState player;
    const auto saved_token = restore.request().token;
    const auto saved_id = restore.page().items[restore.selected()].id;
    player.open(10);
    assert(player.controls(14.999) && !player.controls(15));
    player.direction(1, 15);
    assert(player.focus == 1 && player.controls(15));
    player.direction(1, 15.1);
    assert(player.focus == 2);
    player.dismiss();
    assert(!player.visible && restore.request().token == saved_token && restore.page().items[restore.selected()].id == saved_id);
    assert(!ui::show_up_next(89.999, 100, true, false, true));
    assert(ui::show_up_next(90, 100, true, false, true));
    assert(ui::show_up_next(99.999, 100, true, false, true));
    assert(!ui::show_up_next(100, 100, true, false, true));
    assert(!ui::show_up_next(90, 100, false, false, true));
    assert(!ui::show_up_next(90, 100, true, true, true));
    assert(!ui::show_up_next(90, 0, true, false, true));
    assert(!ui::show_up_next(90, 100, true, false, false));
    assert(!ui::show_up_next(NAN, 100, true, false, true));

    // Search: trust boundary, debounce, stale cancellation, grouped play context, Back restore.
    assert(Request::valid_query("Bjørk 東京") && !Request::valid_query("a\nb") && !Request::valid_query("\xc0\xaf") &&
           !Request::valid_query("\xed\xa0\x80") && !Request::valid_query(std::string(129, 'a')) && !Request::valid_query("\xe6"));
    State find;
    find.activate(); request(find);
    assert(!find.query("x", 0)); // Only inside Search.
    find.search();
    assert(find.request().view == View::Search && find.status() == Status::Ready && !find.take_request());
    assert(find.query("b", 1) && find.query("bj", 1.1) && find.status() == Status::Loading);
    find.tick(1.35); assert(!find.take_request()); // Debounced, never per frame.
    find.tick(1.45);
    auto search = find.take_request();
    assert(search && search->query == "bj" && search->view == View::Search);
    assert(find.query("bjø", 2));
    assert(!find.complete(search->token, {{fixture(Kind::Track, "old")}, ""})); // Stale.
    assert(!find.query("bad\x01", 2) && find.query() == "bjø");
    find.tick(2.3);
    search = find.take_request();
    assert(find.complete(search->token, {{fixture(Kind::Track, "s1"), fixture(Kind::Track, "s2"),
        fixture(Kind::Artist, "ar"), fixture(Kind::Album, "al"), fixture(Kind::Playlist, "pl")}, ""}));
    assert(find.move(1)); find.open();
    auto song = find.take_play_intent();
    assert(song && song->context == View::Search && song->context_id == "bjø" && song->song_id == "s2" &&
           song->occurrence == 1 && song->page_ids == (std::vector<std::string>{"s1", "s2"}));
    assert(find.move(1)); find.open();
    auto artist_search = request(find);
    assert(artist_search.view == View::ArtistAlbums && artist_search.parent_id == "ar");
    assert(find.complete(artist_search.token, {{fixture(Kind::Album, "artist-album")}, ""}));
    find.play_artist(false);
    auto artist_play = find.take_play_intent();
    assert(artist_play && artist_play->action == PlayAction::ArtistPlayAll && artist_play->context_id == "ar");
    assert(find.back() && find.query() == "bjø" && find.selected() == 2 && !find.take_request());
    assert(find.move(2)); find.open();
    auto playlist = request(find);
    assert(playlist.view == View::PlaylistTracks && playlist.parent_id == "pl");
    assert(find.back() && !find.take_request() && find.query() == "bjø" && find.selected() == 4 && find.page().items.size() == 5);
    assert(find.query("", 3) && find.status() == Status::Ready && find.page().items.empty());
    find.tick(10); assert(!find.take_request()); // Empty never queries.
    assert(find.back() && find.request().view == View::Home);
    State bad; bad.search(); bad.query("q", 0); bad.tick(1);
    auto bad_request = bad.take_request();
    assert(!bad.complete(bad_request->token, {{fixture(Kind::Artist, "")}, ""}) || bad.status() == Status::Error);
    bad.retry(); assert(bad.take_request()->query == "q");

    std::cout << "PASS: player restoration, wake consumption, Up Next boundaries; music paging, opaque IDs, stale results, drill-down, retry, play intents\n";
}
