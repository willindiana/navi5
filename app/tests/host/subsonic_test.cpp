/*
 * Jelly5 — host test for the Subsonic client (src/subsonic).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Offline: canned JSON fixtures through an injected transport, assert-based,
 * no framework. Live: NAVI5_LIVE=1 adds a read-only smoke against the public
 * Navidrome demo (demo/demo). Never calls scrobble/star/unstar live.
 */
#include "subsonic.h"
#include "http_jf.h"
#include <cassert>
#include <limits>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <curl/curl.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        const bool passed = (cond);                                        \
        assert(passed);                                                    \
        if (!passed) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

/* --- canned fixtures ------------------------------------------------------- */

static const char *kPing =
    R"json({"subsonic-response":{"status":"ok","version":"1.16.1","type":"navidrome",
       "serverVersion":"0.64.2 (10114574)","openSubsonic":true}})json";

static const char *kPingLegacy = /* no openSubsonic/serverVersion fields */
    R"json({"subsonic-response":{"status":"ok","version":"1.16.1"}})json";

static const char *kExtensions =
    R"json({"subsonic-response":{"status":"ok","openSubsonic":true,
       "openSubsonicExtensions":[
         {"name":"transcodeOffset","versions":[1]},
         {"name":"songLyrics","versions":[1,2]},
         {"name":"formPost"}]}})json";  /* missing versions: tolerated */

static const char *kError =
    R"json({"subsonic-response":{"status":"failed","error":{"code":40,
       "message":"Wrong username or password"}}})json";

static const char *kArtists =
    R"json({"subsonic-response":{"status":"ok","artists":{"index":[
         {"name":"A","artist":[
           {"id":"a1","name":"ABBA","albumCount":8,"coverArt":"ar-a1","starred":"2024-01-01"}]},
         {"name":"B","artist":{"id":"b2","name":"Beatles","albumCount":"13"}}]}}})json";

static const char *kArtist =
    R"json({"subsonic-response":{"status":"ok","artist":{"id":"a1","name":"ABBA","albumCount":8,
       "album":[{"id":"al1","name":"Gold","year":1992,"songCount":19,"duration":4554,
                 "artist":"ABBA","artistId":"a1","coverArt":"al-1"}]}}})json";

static const char *kAlbum =
    R"json({"subsonic-response":{"status":"ok","album":{"id":"al1","name":"Gold","artist":"ABBA",
       "song":[
         {"id":"s1","title":"Dancing Queen","track":1,"duration":232,"bitRate":320,
          "size":"7454720","suffix":"flac","contentType":"audio/flac","discNumber":1,
          "album":"Gold","albumId":"al1","artist":"ABBA","coverArt":"al-1","genre":"Pop"},
         {"id":"s2","title":"Fernando","track":"2","duration":252}]}}})json";

static const char *kAlbumList =
    R"json({"subsonic-response":{"status":"ok","albumList2":{"album":[
        {"id":"al1","title":"Gold","artist":"ABBA","year":1992},
        {"id":"al2","name":"Abbey Road","artist":"Beatles","year":"1969","starred":"2024-05-05"}]}}})json";

static const char *kGenres =
    R"json({"subsonic-response":{"status":"ok","genres":{"genre":[
        {"value":"Pop","songCount":100,"albumCount":"10"},
        {"value":"Rock","songCount":200,"albumCount":20}]}}})json";

static const char *kPlaylists =
    R"json({"subsonic-response":{"status":"ok","playlists":{"playlist":[
        {"id":"p1","name":"Mix","owner":"demo","songCount":2,"duration":484,
         "public":true,"created":"2024-01-01T00:00:00Z","changed":"2024-01-02T00:00:00Z"}]}}})json";

static const char *kPlaylist =
    R"json({"subsonic-response":{"status":"ok","playlist":{"id":"p1","name":"Mix","songCount":1,
       "entry":{"id":"s1","title":"Dancing Queen","duration":232}}}})json";

static const char *kRandomSongs =
    R"json({"subsonic-response":{"status":"ok","randomSongs":{"song":[
        {"id":"s9","title":"Random One","duration":100}]}}})json";

static const char *kSearch3 =
    R"json({"subsonic-response":{"status":"ok","searchResult3":{
        "artist":[{"id":"a1","name":"ABBA"}],
        "album":[{"id":"al1","name":"Gold"}],
        "song":[{"id":"s1","title":"Dancing Queen","duration":232}]}}})json";

static const char *kArtistInfo2 =
    R"json({"subsonic-response":{"status":"ok","artistInfo2":{"biography":"Swedish pop group.",
       "musicBrainzId":"mbid-1","lastFmUrl":"https://last.fm/abba","smallImageUrl":"s",
       "mediumImageUrl":"m","largeImageUrl":"l",
       "similarArtist":[{"id":"b2","name":"Beatles"}]}}})json";

static const char *kAlbumInfo2 =
    R"json({"subsonic-response":{"status":"ok","albumInfo":{"notes":"Compilation.",
       "musicBrainzId":"mbid-2","lastFmUrl":"https://last.fm/gold"}}})json";

static const char *kLyricsSynced =
    R"json({"subsonic-response":{"status":"ok","structuredLyrics":[{"lang":"en","synced":true,
       "displayArtist":"ABBA","displayTitle":"Dancing Queen","line":[
         {"start":1000,"value":"You can dance"},
         {"start":2000,"value":"You can jive"}]}]}})json";

static const char *kLyricsPlain =
    R"json({"subsonic-response":{"status":"ok","lyrics":{"artist":"ABBA","title":"Fernando",
       "value":"There was something in the air"}}})json";

static const char *kLyricsNone = /* empty list: client falls back to getLyrics */
    R"json({"subsonic-response":{"status":"ok","structuredLyrics":[]}})json";

static const char *kStarred2 =
    R"json({"subsonic-response":{"status":"ok","starred2":{
       "artist":[{"id":"a1","name":"ABBA"}],
       "album":[{"id":"al2","name":"Abbey Road"}],
       "song":[{"id":"s1","title":"Dancing Queen"}]}}})json";

static const char *kOk = R"json({"subsonic-response":{"status":"ok"}})json";

/* Transport answering from the fixture table by endpoint name. */
static subsonic::HttpResponse canned(const std::string &url)
{
    struct { const char *endpoint, *body; } table[] = {
        {"/rest/ping?", kPing},
        {"/rest/getOpenSubsonicExtensions?", kExtensions},
        {"/rest/getArtists?", kArtists},
        {"/rest/getArtist?", kArtist},
        {"/rest/getAlbum?", kAlbum},
        {"/rest/getAlbumList2?", kAlbumList},
        {"/rest/getGenres?", kGenres},
        {"/rest/getPlaylists?", kPlaylists},
        {"/rest/getPlaylist?", kPlaylist},
        {"/rest/getRandomSongs?", kRandomSongs},
        {"/rest/search3?", kSearch3},
        {"/rest/getArtistInfo2?", kArtistInfo2},
        {"/rest/getAlbumInfo2?", kAlbumInfo2},
        {"/rest/getLyricsBySongId?", kLyricsSynced},
        {"/rest/getLyrics?", kLyricsPlain},
        {"/rest/getStarred2?", kStarred2},
        {"/rest/scrobble?", kOk},
        {"/rest/star?", kOk},
        {"/rest/unstar?", kOk},
    };
    subsonic::HttpResponse r;
    for (auto &t : table)
        if (url.find(t.endpoint) != std::string::npos) {
            r.status = 200;
            r.body = t.body;
            return r;
        }
    r.error = "no fixture for " + subsonic::redact_url(url);
    return r;
}

static subsonic::Client make_client()
{
    return subsonic::Client("https://demo.example", "user", "pass", canned);
}

int main()
{
    { // Top songs: NAME query, fixed bound, singleton/empty/error/malformed.
        std::string body, url, error;
        subsonic::Client top("https://example.invalid", "u", "p", [&](const std::string &u) {
            url = u; return subsonic::HttpResponse{200, body, ""};
        });
        std::vector<subsonic::Song> songs;
        body = R"({"subsonic-response":{"status":"ok","topSongs":{"song":{"id":"dup","artistId":"artist","duration":"42"}}}})";
        CHECK(top.get_top_songs("Name & Name", &songs, &error));
        CHECK(url.find("artist=Name%20%26%20Name&count=20") != std::string::npos);
        CHECK(songs.size() == 1 && songs[0].id == "dup" && songs[0].duration == 42);
        body = R"({"subsonic-response":{"status":"ok","topSongs":{}}})";
        CHECK(top.get_top_songs("Name", &songs, &error) && songs.empty());
        for (const auto *bad : {R"({"subsonic-response":{"status":"failed","error":{"code":70,"message":"Unsupported"}}})",
             R"({"subsonic-response":{"status":"ok"}})",
             R"({"subsonic-response":{"status":"ok","topSongs":{"song":42}}})",
             R"({"subsonic-response":{"status":"ok","topSongs":{"song":[{}]}}})", "not json"}) {
            body = bad; CHECK(!top.get_top_songs("Name", &songs, &error) && songs.empty());
        }
    }
    std::string err;

    /* token and redaction */
    CHECK(subsonic::make_token("pass", "salt").size() == 32);
    /* md5("passsalt") known value */
    CHECK(subsonic::make_token("pass", "salt") == "83234657c5df8232839ac8c0572e158d");
    {
        subsonic::Client c = make_client();
        std::string url = c.build_url("ping");
        CHECK(url.find("https://demo.example/rest/ping?") == 0);
        CHECK(url.find("u=user") != std::string::npos);
        CHECK(url.find("&t=") != std::string::npos);
        CHECK(url.find("&s=") != std::string::npos);
        CHECK(url.find("v=1.16.1") != std::string::npos);
        CHECK(url.find("c=navi5") != std::string::npos);
        CHECK(url.find("f=json") != std::string::npos);
        /* fresh salt per URL */
        std::string url2 = c.build_url("ping");
        CHECK(url != url2);
        std::string red = subsonic::redact_url(url);
        CHECK(red.find("pass") == std::string::npos);
        CHECK(red.find("u=***") != std::string::npos);
        CHECK(red.find("t=***") != std::string::npos);
        CHECK(red.find("s=***") != std::string::npos);
        /* redaction keeps non-secret params */
        std::string cover = c.cover_art_url("al-1", 300);
        CHECK(subsonic::redact_url(cover).find("id=al-1") != std::string::npos);
        CHECK(subsonic::redact_url(cover).find("size=300") != std::string::npos);
        /* stream url options */
        std::string s1 = c.stream_url("s1");
        CHECK(s1.find("format=") == std::string::npos);
        std::string s2 = c.stream_url("s1", "mp3", 320, 30);
        CHECK(s2.find("format=mp3") != std::string::npos);
        CHECK(s2.find("maxBitRate=320") != std::string::npos);
        CHECK(s2.find("timeOffset=30") != std::string::npos);
        /* api key auth: no u/t/s */
        c.set_api_key("secret key");
        std::string ka = c.build_url("ping");
        CHECK(ka.find("apiKey=secret%20key") != std::string::npos);
        CHECK(ka.find("&t=") == std::string::npos);
        CHECK(subsonic::redact_url(ka).find("secret") == std::string::npos);
        CHECK(subsonic::redact_url(ka).find("apiKey=***") != std::string::npos);
    }

    /* Artwork URLs stay stable without sharing auth across clients/accounts. */
    {
        auto art = make_client();
        const std::string cover = art.cover_art_url("al-1", 300);
        CHECK(cover == art.cover_art_url("al-1", 300));
        CHECK(cover != art.cover_art_url("al-2", 300));
        CHECK(cover != art.cover_art_url("al-1", 720));
        CHECK(cover != make_client().cover_art_url("al-1", 300));
        subsonic::Client other("https://demo.example", "other", "other-pass", canned);
        CHECK(cover != other.cover_art_url("al-1", 300));
        CHECK(art.stream_url("s1") != art.stream_url("s1"));

        std::vector<std::thread> workers;
        std::vector<std::string> urls(8);
        for (size_t i = 0; i < urls.size(); ++i)
            workers.emplace_back([&, i] {
                for (int n = 0; n < 100; ++n) {
                    art.cover_art_url("worker-" + std::to_string(i), 720);
                    urls[i] = art.cover_art_url("concurrent", 300);
                }
            });
        for (auto &worker : workers)
            worker.join();
        for (const auto &url : urls)
            CHECK(url == urls.front());

        /* Keep hot entry while exceeding capacity; cold entry gets evicted. */
        const std::string cold = art.cover_art_url("cold", 300);
        for (int i = 0; i < 256; ++i) {
            art.cover_art_url("fill-" + std::to_string(i), 300);
            CHECK(art.cover_art_url("al-1", 300) == cover);
        }
        CHECK(art.cover_art_url("cold", 300) != cold);
        art.set_api_key("art-key");
        const std::string keyed = art.cover_art_url("al-1", 300);
        CHECK(keyed != cover);
        CHECK(keyed.find("apiKey=art-key") != std::string::npos);
        CHECK(keyed == art.cover_art_url("al-1", 300));
        art.set_api_key("");
        CHECK(art.cover_art_url("al-1", 300) != keyed);
        CHECK(art.cover_art_url("al-1", 300) != cover);
    }

    subsonic::Client c = make_client();

    subsonic::ServerInfo si;
    CHECK(c.ping(&si, &err));
    CHECK(si.open_subsonic);
    CHECK(si.server_version == "0.64.2 (10114574)");
    CHECK(si.type == "navidrome");

    {
        subsonic::Client legacy("https://x", "u", "p",
                                [](const std::string &) {
                                    subsonic::HttpResponse r;
                                    r.status = 200;
                                    r.body = kPingLegacy;
                                    return r;
                                });
        subsonic::ServerInfo li;
        CHECK(legacy.ping(&li, &err));
        CHECK(!li.open_subsonic);
        CHECK(li.server_version.empty());
    }

    std::vector<subsonic::Extension> exts;
    CHECK(c.get_extensions(&exts, &err));
    CHECK(exts.size() == 3);
    CHECK(exts[0].name == "transcodeOffset" && exts[0].versions.size() == 1);
    CHECK(exts[1].versions.size() == 2 && exts[1].versions[1] == 2);
    CHECK(exts[2].versions.empty());   /* missing versions tolerated */
    {
        bool has_api_key = false;
        for (const auto &e : exts)
            has_api_key |= e.name == "apiKeyAuthentication";
        CHECK(!has_api_key);
    }
    for (const char *versions : {"[1.9]", "[1]"}) {
        subsonic::Client strict("https://x", "u", "p", [&](const std::string &) {
            return subsonic::HttpResponse{200, std::string(
                R"({"subsonic-response":{"status":"ok","openSubsonicExtensions":[{"name":"apiKeyAuthentication","versions":)") +
                versions + "}]}}", ""};
        });
        CHECK(strict.get_extensions(&exts, &err));
        CHECK(exts.size() == 1 && exts[0].versions == (versions[2] == '.' ? std::vector<int>{} : std::vector<int>{1}));
    }

    /* subsonic error object */
    {
        subsonic::Client bad("https://x", "u", "p",
                             [](const std::string &) {
                                 subsonic::HttpResponse r;
                                 r.status = 200;
                                 r.body = kError;
                                 return r;
                             });
        subsonic::ServerInfo tmp;
        subsonic::LoginFailure failure;
        CHECK(!bad.ping(&tmp, &err, &failure));
        CHECK(failure == subsonic::LoginFailure::Authentication);
        CHECK(err == "Subsonic login refused or invalid response");
    }
    /* transport failure */
    {
        subsonic::Client dead("https://x", "u", "p",
                              [](const std::string &) {
                                  subsonic::HttpResponse r;
                                  r.error = "connection refused";
                                  return r;
                              });
        subsonic::ServerInfo tmp;
        CHECK(!dead.ping(&tmp, &err));
        CHECK(err == "HTTP transport failed");
    }
    /* HTTP failure */
    {
        subsonic::Client dead("https://x", "u", "p",
                              [](const std::string &) {
                                  subsonic::HttpResponse r;
                                  r.status = 500;
                                  return r;
                              });
        subsonic::ServerInfo tmp;
        CHECK(!dead.ping(&tmp, &err));
        CHECK(err == "HTTP request failed");
    }

    std::vector<subsonic::Artist> artists;
    CHECK(c.get_artists(&artists, &err));
    CHECK(artists.size() == 2);             /* single object accepted as list */
    CHECK(artists[0].name == "ABBA" && artists[0].album_count == 8 && artists[0].starred);
    CHECK(artists[1].name == "Beatles" && artists[1].album_count == 13);  /* number as string */

    subsonic::Artist artist;
    std::vector<subsonic::Album> albums;
    CHECK(c.get_artist("a1", &artist, &albums, &err));
    CHECK(artist.name == "ABBA");
    CHECK(albums.size() == 1 && albums[0].name == "Gold" && albums[0].duration == 4554);

    subsonic::Album album;
    std::vector<subsonic::Song> songs;
    CHECK(c.get_album("al1", &album, &songs, &err));
    CHECK(album.name == "Gold");
    CHECK(songs.size() == 2);
    CHECK(songs[0].title == "Dancing Queen" && songs[0].size == 7454720 && songs[0].suffix == "flac");
    CHECK(songs[1].track == 2);   /* number as string */

    CHECK(c.get_album_list("newest", 20, 0, "", &albums, &err));
    CHECK(albums.size() == 2);
    CHECK(albums[0].name == "Gold");      /* "title" accepted for name */
    CHECK(albums[1].starred && albums[1].year == 1969);

    std::vector<subsonic::Genre> genres;
    CHECK(c.get_genres(&genres, &err));
    CHECK(genres.size() == 2 && genres[0].name == "Pop" && genres[0].album_count == 10);

    std::vector<subsonic::Playlist> playlists;
    CHECK(c.get_playlists(&playlists, &err));
    CHECK(playlists.size() == 1 && playlists[0].is_public && playlists[0].owner == "demo");

    subsonic::Playlist pl;
    CHECK(c.get_playlist("p1", &pl, &songs, &err));
    CHECK(pl.name == "Mix" && songs.size() == 1);   /* single entry object */

    CHECK(c.get_random_songs(10, "Pop", &songs, &err));
    CHECK(songs.size() == 1 && songs[0].title == "Random One");

    subsonic::SearchResult res;
    CHECK(c.search3("abba", 5, 5, 5, &res, &err));
    CHECK(res.artists.size() == 1 && res.albums.size() == 1 && res.songs.size() == 1);

    subsonic::ArtistInfo ai;
    CHECK(c.get_artist_info2("a1", &ai, &err));
    CHECK(ai.biography == "Swedish pop group." && ai.similar.size() == 1);

    subsonic::AlbumInfo ali;
    CHECK(c.get_album_info2("al1", &ali, &err));
    CHECK(ali.notes == "Compilation." && ali.music_brainz_id == "mbid-2");

    subsonic::Lyrics ly;
    CHECK(c.get_lyrics("s1", "ABBA", "Dancing Queen", &ly, &err));
    CHECK(ly.synced && ly.lines.size() == 2 && ly.lines[0].start == 1000);
    /* fallback to getLyrics when no structured entry */
    {
        subsonic::Client c2("https://demo.example", "user", "pass",
                            [](const std::string &url) {
                                subsonic::HttpResponse r;
                                r.status = 200;
                                r.body = url.find("getLyricsBySongId") != std::string::npos
                                             ? kLyricsNone
                                             : kLyricsPlain;
                                return r;
                            });
        subsonic::Lyrics plain;
        CHECK(c2.get_lyrics("s2", "ABBA", "Fernando", &plain, &err));
        CHECK(!plain.synced && plain.value.find("something in the air") != std::string::npos);
    }

    subsonic::Starred starred;
    CHECK(c.get_starred2(&starred, &err));
    CHECK(starred.artists.size() == 1 && starred.albums.size() == 1 && starred.songs.size() == 1);

    /* write endpoints against the canned transport only */
    CHECK(c.scrobble("s1", true, &err));
    CHECK(c.star("s1", &err));
    CHECK(c.unstar("s1", &err));

    {
        std::string requested;
        subsonic::Client routes("https://x/", "u", "p", [&requested](const std::string &url) {
            requested = url;
            return canned(url);
        });
        CHECK(routes.get_album_list("byGenre", 5, 10, "R&B", &albums, &err));
        CHECK(requested.find("type=byGenre&size=5&offset=10&genre=R%26B") != std::string::npos);
        for (const auto &years : {std::pair<int, int>{0, 9999}, {2025, 1990}, {2000, 2000}}) {
            CHECK(routes.get_album_list("byYear", 5, 0, "", &albums, &err, years.first, years.second));
            CHECK(requested.find("&fromYear=" + std::to_string(years.first) +
                                 "&toYear=" + std::to_string(years.second)) != std::string::npos);
        }
        requested.clear();
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, 2000));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, {}, 2000));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, -1, 2000));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, 2000, -1));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, 10000, 2000));
        CHECK(!routes.get_album_list("byYear", 5, 0, "", &albums, &err, 2000, 10000));
        CHECK(requested.empty()); // invalid bounds never reach transport
        CHECK(routes.search3("a & b", 1, 2, 3, &res, &err));
        CHECK(requested.find("query=a%20%26%20b&artistCount=1&albumCount=2&songCount=3") != std::string::npos);
        CHECK(routes.search3("", 40, 0, 0, &res, &err, 40));
        CHECK(requested.find("query=&artistOffset=40&artistCount=40&albumCount=0&songCount=0") != std::string::npos);
        requested.clear();
        CHECK(!routes.search3("", 40, 0, 0, &res, &err, -1) && requested.empty());
        CHECK(routes.star("album&1", &err, subsonic::StarType::album));
        CHECK(requested.find("&albumId=album%261") != std::string::npos);
        CHECK(routes.unstar("artist1", &err, subsonic::StarType::artist));
        CHECK(requested.find("&artistId=artist1") != std::string::npos);
        CHECK(routes.scrobble("s1", false, &err));
        CHECK(requested.find("submission=false") != std::string::npos);
    }
    // Adversarial numeric input must never cause undefined float-to-int casts.
    {
        subsonic::Client edge("https://x", "u", "p", [](const std::string &) {
            return subsonic::HttpResponse{200, R"json({"subsonic-response":{"status":"ok",
                "album":{"id":1e999,"song":{"track":1e999,"duration":"bad",
                "size":"9223372036854775808"}}}})json", ""};
        });
        CHECK(edge.get_album("x", &album, &songs, &err));
        CHECK(songs.size() == 1 && songs[0].track == std::numeric_limits<int>::max());
        CHECK(songs[0].duration == 0 && songs[0].size == std::numeric_limits<long long>::max());
    }
    // Offset is signed ms, positive=earlier; retain raw timestamps, even
    // when effective display time is negative. Missing/plain offset resets.
    for (int offset : {250, -250}) {
        subsonic::Client timing("https://x", "u", "p", [offset](const std::string &) {
            return subsonic::HttpResponse{200,
                "{\"subsonic-response\":{\"status\":\"ok\",\"lyricsList\":{\"structuredLyrics\":{"
                "\"synced\":true,\"offset\":" + std::to_string(offset) +
                ",\"line\":[{\"start\":100,\"value\":\"early\"},{\"start\":-50,\"value\":\"raw\"}]}}}}", ""};
        });
        CHECK(timing.get_lyrics("s", "a", "t", &ly, &err));
        CHECK(ly.offset == offset && ly.lines.size() == 2);
        CHECK(ly.lines[0].start == 100 && ly.lines[1].start == -50);
        CHECK((long long)ly.lines[0].start - ly.offset == (offset > 0 ? -150 : 350));
        CHECK(c.get_lyrics("s1", "a", "t", &ly, &err));
        CHECK(ly.offset == 0); // omitted structured offset
    }
    // Real OpenSubsonic lyricsList envelope; unsupported endpoint fallback.
    for (bool unsupported : {false, true}) {
        subsonic::Client lyrics("https://x", "u", "p", [unsupported](const std::string &url) {
            if (url.find("getLyricsBySongId?") == std::string::npos)
                return subsonic::HttpResponse{200, kLyricsPlain, ""};
            return subsonic::HttpResponse{200, unsupported
                ? R"json({"subsonic-response":{"status":"failed","error":{"code":70}}})json"
                : R"json({"subsonic-response":{"status":"ok","lyricsList":{"structuredLyrics":{
                    "synced":true,"line":{"start":"12","value":"hello"}}}}})json", ""};
        });
        ly.offset = 123;
        CHECK(lyrics.get_lyrics("s", "a", "t", &ly, &err));
        CHECK(ly.offset == 0);
        CHECK(unsupported ? !ly.value.empty() : ly.lines[0].start == 12);
        CHECK(err.empty());
    }
    for (const char *body : {"not json", "[]", "{\"subsonic-response\":null}"}) {
        subsonic::Client invalid("https://x", "u", "p", [body](const std::string &) {
            return subsonic::HttpResponse{200, body, ""};
        });
        CHECK(!invalid.ping(&si, &err));
    }
    CHECK(subsonic::Client("https://user:secret@x", "", "", {}).build_url("ping").empty());
    CHECK(c.build_url("ping?u=injection").empty());
    CHECK(subsonic::redact_url("https://x?u=x&t=y&s=z&p=w&apiKey=k&id=ok") ==
          "https://x?u=***&t=***&s=***&p=***&apiKey=***&id=ok");

    /* --- optional live read-only smoke against the public demo ------------- */
    if (std::getenv("NAVI5_LIVE") && std::strcmp(std::getenv("NAVI5_LIVE"), "1") == 0) {
        CHECK(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
        subsonic::Client live("https://demo.navidrome.org", "demo", "demo", subsonic::http_get_jf);
        subsonic::ServerInfo lsi;
        if (live.ping(&lsi, &err)) {
            std::printf("live: %s %s (openSubsonic=%d)\n", lsi.type.c_str(),
                        lsi.server_version.c_str(), (int)lsi.open_subsonic);
            CHECK(lsi.open_subsonic);
            std::vector<subsonic::Extension> le;
            CHECK(live.get_extensions(&le, &err));
            bool api_key = false;
            for (const auto &e : le) {
                std::printf("live ext: %s v", e.name.c_str());
                for (int v : e.versions)
                    std::printf("%d ", v);
                std::printf("\n");
                api_key |= e.name == "apiKeyAuthentication";
            }
            std::printf("live: apiKeyAuthentication %s\n", api_key ? "SUPPORTED" : "NOT advertised");
            CHECK(live.get_artists(&artists, &err) && !artists.empty());
            CHECK(live.get_album_list("newest", 5, 0, "", &albums, &err) && !albums.empty());
            if (!artists.empty())
                CHECK(live.get_artist(artists[0].id, &artist, &albums, &err));
            if (!albums.empty()) {
                CHECK(live.get_album(albums[0].id, &album, &songs, &err));
                CHECK(live.get_album_info2(album.id, &ali, &err));
            }
            if (!songs.empty())
                CHECK(live.get_lyrics(songs[0].id, songs[0].artist, songs[0].title, &ly, &err));
            CHECK(live.get_playlists(&playlists, &err));
            if (!playlists.empty())
                CHECK(live.get_playlist(playlists[0].id, &pl, &songs, &err));
            CHECK(live.get_random_songs(2, "", &songs, &err));
            CHECK(live.get_starred2(&starred, &err));
            CHECK(live.search3("a", 3, 3, 3, &res, &err));
            CHECK(live.get_genres(&genres, &err));
            /* read-only only: no scrobble/star/unstar here, ever */
        } else {
            std::fprintf(stderr, "live smoke failed (transport/API error; URL omitted)\n");
            ++failures;
        }
        curl_global_cleanup();
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all subsonic host checks passed\n");
    return 0;
}
