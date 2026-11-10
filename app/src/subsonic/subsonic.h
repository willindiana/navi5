/*
 * Jelly5 — Navidrome/(Open)Subsonic client for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * URL building, token+salt auth and the tolerant JSON field access are
 * adapted from SymphonyStation5's src/subsonic/client.{h,cpp}
 * (GPL-3.0-or-later, Copyright (C) 2026 tsvetomirgt,
 * https://github.com/TsvetomirGT/SymphonyStation5).
 *
 * One blocking request per call; run off the render thread. All calls go
 * through the injected HttpGet, so host tests can use libcurl and the console
 * the app's FFmpeg avio path. Parsing tolerates missing fields, numbers sent
 * as strings and single objects where an array is expected.
 */
#pragma once

#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <vector>
#include <utility>
#include <optional>

namespace subsonic {

enum class LoginFailure { None, Http401, Http403, Authentication, Connection, Timeout, CaBundle, Untrusted, Tls, Initialization, Configuration, Redirect, Http, Response, Transport, ApiKeyUnsupported };

struct HttpResponse {
    int status = 0;          /* HTTP status; 0 when no response arrived */
    std::string body;
    std::string error;       /* transport error text, if any */
    LoginFailure failure = LoginFailure::None;
    bool ok() const { return status >= 200 && status < 300; }
};

/* One HTTP GET; returns the status and body. Injected by the caller. */
using HttpGet = std::function<HttpResponse(const std::string &url)>;

struct Artist {
    std::string id, name, cover_art;
    int album_count = 0;
    bool starred = false;
};

struct Album {
    std::string id, name, artist, artist_id, cover_art, genre;
    int song_count = 0;
    int year = 0;
    int duration = 0;        /* seconds */
    bool starred = false;
};

struct Song {
    std::string id, title, artist, artist_id, album, album_id, cover_art, genre;
    std::string content_type, suffix;
    int track = 0;
    int disc_number = 0;
    int duration = 0;        /* seconds */
    int bit_rate = 0;        /* kbps */
    long long size = 0;      /* bytes */
    bool starred = false;
};

struct Genre {
    std::string name;
    int song_count = 0;
    int album_count = 0;
};

struct Playlist {
    std::string id, name, comment, owner, cover_art;
    std::string created, changed;
    int song_count = 0;
    int duration = 0;        /* seconds */
    bool is_public = false;
};

struct ArtistInfo {
    std::string biography, music_brainz_id, last_fm_url;
    std::string small_image_url, medium_image_url, large_image_url;
    std::vector<Artist> similar;
};

struct AlbumInfo {
    std::string notes, music_brainz_id, last_fm_url;
    std::string small_image_url, medium_image_url, large_image_url;
};

struct LyricLine {
    int start = 0;           /* ms; 0 when unsynced */
    std::string text;
};

struct Lyrics {
    std::string artist, title, value;   /* value: plain text when unsynced */
    bool synced = false;
    // Signed ms; positive means earlier. Raw line.start is unchanged.
    // Effective time: (long long)line.start - offset; may be negative.
    int offset = 0;
    std::vector<LyricLine> lines;       /* synced lines */
};

struct Extension {
    std::string name;
    std::vector<int> versions;
};

struct ServerInfo {
    bool open_subsonic = false;
    std::string version;         /* Subsonic API version, e.g. "1.16.1" */
    std::string server_version;  /* server build, e.g. "0.64.2 (...)" */
    std::string type;            /* server type, e.g. "navidrome" */
};

struct SearchResult {
    std::vector<Artist> artists;
    std::vector<Album> albums;
    std::vector<Song> songs;
};

struct Starred {
    std::vector<Artist> artists;
    std::vector<Album> albums;
    std::vector<Song> songs;
};

enum class StarType { song, album, artist };

class Client {
public:
    Client(std::string server, std::string username, std::string password, HttpGet http);

    const std::string &server() const { return server_; }

    /* Use OpenSubsonic apiKey auth (apiKey=...) instead of token+salt. Only
     * servers advertising the apiKeyAuthentication extension support this. */
    void set_api_key(std::string key)
    {
        std::lock_guard<std::mutex> lock(cover_mutex_);
        if (api_key_ != key) {
            api_key_ = std::move(key);
            cover_urls_.clear();
        }
    }

    /* <server>/rest/<endpoint>?auth&v=1.16.1&c=navi5&f=json[&extra]. A fresh
     * random salt per URL, as the API recommends. The result carries
     * credentials: never log it (see redact_url). */
    std::string build_url(const std::string &endpoint, const std::string &extra_query = "") const;

    /* URL builders only; they perform no request. format/max_bit_rate/
     * time_offset are optional (empty/0 leave them out). */
    std::string stream_url(const std::string &song_id, const std::string &format = "",
                           int max_bit_rate = 0, int time_offset = 0) const;
    /* Reuse authenticated URLs in a per-client 256-entry LRU. Eviction or
     * changing the API key ends reuse; concurrent readers are supported. */
    std::string cover_art_url(const std::string &cover_art, int size) const;

    /* Output and error pointers must be non-null. Do not mutate auth while
     * requests run. Transport must be thread-safe for concurrent calls.
     * A persisted token+salt pair is password-equivalent: replayable, not a
     * safer stored secret. Never log credential URLs; use HTTPS.
     * Each returns false and sets *error on transport, HTTP or API failure
     * (e.g. "Wrong username or password", code 40). */
    bool ping(ServerInfo *info, std::string *error, LoginFailure *failure = nullptr) const;
    bool get_extensions(std::vector<Extension> *out, std::string *error, LoginFailure *failure = nullptr) const;
    bool get_artists(std::vector<Artist> *out, std::string *error) const;
    bool get_artist(const std::string &id, Artist *artist, std::vector<Album> *albums,
                    std::string *error) const;
    // Legacy endpoint takes artist NAME, not ID; no similarity/random fallback.
    bool get_top_songs(const std::string &artist, std::vector<Song> *songs, std::string *error) const;
    bool get_album(const std::string &id, Album *album, std::vector<Song> *songs,
                   std::string *error) const;
    /* type: "random", "newest", "recent", "frequent", "starred",
     * "alphabeticalByName", "alphabeticalByArtist", "byYear", "byGenre".
     * genre is only used by "byGenre". byYear requires both year bounds
     * (0..9999); from_year > to_year requests reverse chronological order. */
    bool get_album_list(const std::string &type, int size, int offset, const std::string &genre,
                        std::vector<Album> *out, std::string *error,
                        std::optional<int> from_year = {}, std::optional<int> to_year = {}) const;
    bool get_genres(std::vector<Genre> *out, std::string *error) const;
    bool get_playlists(std::vector<Playlist> *out, std::string *error) const;
    bool get_playlist(const std::string &id, Playlist *playlist, std::vector<Song> *songs,
                      std::string *error) const;
    bool get_random_songs(int size, const std::string &genre, std::vector<Song> *out,
                          std::string *error) const;
    bool search3(const std::string &query, int artist_count, int album_count, int song_count,
                 SearchResult *out, std::string *error, int artist_offset = 0) const;
    bool get_artist_info2(const std::string &id, ArtistInfo *out, std::string *error) const;
    bool get_album_info2(const std::string &id, AlbumInfo *out, std::string *error) const;
    /* songLyrics extension (getLyricsBySongId) first; falls back to plain
     * getLyrics(artist, title) when the server has no synced entry. */
    bool get_lyrics(const std::string &song_id, const std::string &artist, const std::string &title,
                    Lyrics *out, std::string *error) const;
    bool scrobble(const std::string &song_id, bool submission, std::string *error) const;
    bool star(const std::string &id, std::string *error, StarType type = StarType::song) const;
    bool unstar(const std::string &id, std::string *error, StarType type = StarType::song) const;
    bool get_starred2(Starred *out, std::string *error) const;

private:
    std::string server_, username_, password_, api_key_;
    HttpGet http_;
    mutable std::mutex cover_mutex_;
    mutable std::list<std::pair<std::string, std::string>> cover_urls_;
};

/* The URL credential parameters (u, t, s, p, apiKey) replaced by "***". For
 * logs and error messages. */
std::string redact_url(const std::string &url);

/* Exposed for tests: token = md5(password + salt), lowercase hex. */
std::string make_token(const std::string &password, const std::string &salt);

} // namespace subsonic
