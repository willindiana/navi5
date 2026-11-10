/*
 * Jelly5 — Navidrome/(Open)Subsonic client for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from SymphonyStation5's src/subsonic/client.cpp (GPL-3.0-or-later,
 * Copyright (C) 2026 tsvetomirgt,
 * https://github.com/TsvetomirGT/SymphonyStation5), re-based on cJSON and
 * FFmpeg's MD5. Compatible with -fno-exceptions/-fno-rtti.
 */
#include "subsonic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <atomic>
#include <string>

extern "C" {
#include "cJSON.h"
#include <libavutil/md5.h>
#include <libavutil/random_seed.h>
}

namespace subsonic {
namespace {

constexpr const char *kApiVersion = "1.16.1";
constexpr const char *kClientName = "navi5";

/* --- small helpers, mirroring jf_client.cpp's tolerant accessors --------- */

const cJSON *child(const cJSON *o, const char *key)
{
    return cJSON_IsObject(o) ? cJSON_GetObjectItemCaseSensitive(o, key) : nullptr;
}

template <typename T> T integer(const cJSON *v, T fallback = 0)
{
    double n = 0;
    if (cJSON_IsNumber(v)) {
        n = v->valuedouble;
    } else if (cJSON_IsString(v) && v->valuestring) {
        char *end = nullptr;
        n = std::strtod(v->valuestring, &end);
        if (end == v->valuestring || *end != '\0')
            return fallback;
    } else {
        return fallback;
    }
    if (std::isnan(n))
        return fallback;
    if (n >= (double)std::numeric_limits<T>::max())
        return std::numeric_limits<T>::max();
    if (n <= (double)std::numeric_limits<T>::min())
        return std::numeric_limits<T>::min();
    return (T)n;
}

/* A string; a number is accepted and rendered (servers differ). */
std::string str_of(const cJSON *o, const char *key)
{
    const cJSON *v = child(o, key);
    if (cJSON_IsString(v) && v->valuestring)
        return v->valuestring;
    if (cJSON_IsNumber(v)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", integer<long long>(v));
        return buf;
    }
    return std::string();
}

/* An integer; a numeric string is accepted (servers differ). */
int int_of(const cJSON *o, const char *key, int fallback = 0)
{
    return integer<int>(child(o, key), fallback);
}

bool bool_of(const cJSON *o, const char *key)
{
    const cJSON *v = child(o, key);
    if (cJSON_IsBool(v))
        return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring)
        return std::strcmp(v->valuestring, "true") == 0;
    return false;
}

/* A list field that may be absent, a single object, or an array. The
 * returned pointers are valid while the document is. */
std::vector<const cJSON *> items(const cJSON *o, const char *key)
{
    std::vector<const cJSON *> out;
    const cJSON *v = child(o, key);
    if (cJSON_IsArray(v)) {
        const cJSON *e = nullptr;
        cJSON_ArrayForEach(e, v) {
            if (cJSON_IsObject(e))
                out.push_back(e);
        }
    } else if (cJSON_IsObject(v)) {
        out.push_back(v);
    }
    return out;
}

/* Owns the parsed document for the scope of one request. */
struct Doc {
    cJSON *root = nullptr;
    Doc() = default;
    explicit Doc(const std::string &body) : root(cJSON_Parse(body.c_str())) {}
    ~Doc() { cJSON_Delete(root); }
    Doc(const Doc &) = delete;
    Doc &operator=(const Doc &) = delete;
    void reset(const std::string &body)
    {
        cJSON_Delete(root);
        root = cJSON_Parse(body.c_str());
    }
};

/* Every response is {"subsonic-response": {"status": "ok"|"failed", ...}}.
 * On success returns the inner object; on failure sets *error. */
const cJSON *unwrap(const Doc &doc, std::string *error)
{
    const cJSON *inner = child(doc.root, "subsonic-response");
    if (!cJSON_IsObject(inner)) {
        *error = "unexpected response (not Subsonic JSON)";
        return nullptr;
    }
    if (str_of(inner, "status") != "ok" || cJSON_IsObject(child(inner, "error"))) {
        const cJSON *err = child(inner, "error");
        long long code = int_of(err, "code", -1);
        std::string msg = str_of(err, "message");
        if (msg.empty())
            msg = "API error";
        if (code >= 0)
            *error = msg + " (code " + std::to_string(code) + ")";
        else
            *error = msg;
        return nullptr;
    }
    return inner;
}

/* --- URL bits ------------------------------------------------------------- */

std::string url_escape(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string md5_hex(const std::string &data)
{
    uint8_t md[16];
    av_md5_sum(md, (const uint8_t *)data.data(), data.size());
    char out[33];
    for (int i = 0; i < 16; i++)
        std::snprintf(out + i * 2, 3, "%02x", md[i]);
    return std::string(out, 32);
}

std::string random_salt()
{
    /* FFmpeg already supplies platform entropy, including its fallback.
     * A counter ensures distinct salts even on platforms with weak entropy.
     * Salt is public, NOT an anti-replay mechanism: token+salt is replayable. */
    static std::atomic<unsigned long long> sequence{0};
    char salt[49];
    std::snprintf(salt, sizeof salt, "%08x%08x%08x%016llx",
                  av_get_random_seed(), av_get_random_seed(), av_get_random_seed(),
                  sequence.fetch_add(1));
    return salt;
}

/* --- JSON -> structs ------------------------------------------------------ */

Artist to_artist(const cJSON *a)
{
    Artist artist;
    artist.id = str_of(a, "id");
    artist.name = str_of(a, "name");
    artist.cover_art = str_of(a, "coverArt");
    artist.album_count = int_of(a, "albumCount", -1);
    artist.starred = !str_of(a, "starred").empty();
    return artist;
}

Album to_album(const cJSON *a)
{
    Album album;
    album.id = str_of(a, "id");
    album.name = str_of(a, "name");
    if (album.name.empty())
        album.name = str_of(a, "title");
    album.artist = str_of(a, "artist");
    album.artist_id = str_of(a, "artistId");
    album.cover_art = str_of(a, "coverArt");
    album.genre = str_of(a, "genre");
    album.song_count = int_of(a, "songCount", -1);
    album.year = (int)int_of(a, "year");
    album.duration = (int)int_of(a, "duration");
    album.starred = !str_of(a, "starred").empty();
    return album;
}

Song to_song(const cJSON *t)
{
    Song song;
    song.id = str_of(t, "id");
    song.title = str_of(t, "title");
    song.artist = str_of(t, "artist");
    song.artist_id = str_of(t, "artistId");
    song.album = str_of(t, "album");
    song.album_id = str_of(t, "albumId");
    song.cover_art = str_of(t, "coverArt");
    song.genre = str_of(t, "genre");
    song.content_type = str_of(t, "contentType");
    song.suffix = str_of(t, "suffix");
    song.track = (int)int_of(t, "track");
    song.disc_number = (int)int_of(t, "discNumber");
    song.duration = (int)int_of(t, "duration");
    song.bit_rate = (int)int_of(t, "bitRate");
    song.size = integer<long long>(child(t, "size"));
    song.starred = !str_of(t, "starred").empty();
    return song;
}

} // namespace

std::string make_token(const std::string &password, const std::string &salt)
{
    return md5_hex(password + salt);
}

std::string redact_url(const std::string &url)
{
    static const char *kSecrets[] = {"u=", "t=", "s=", "p=", "apiKey="};
    std::string out = url;
    size_t q = out.find('?');
    if (q == std::string::npos)
        return out;
    for (const char *key : kSecrets) {
        size_t pos = q;
        const size_t klen = std::strlen(key);
        while ((pos = out.find(key, pos)) != std::string::npos) {
            if (out[pos - 1] != '?' && out[pos - 1] != '&') {
                pos += klen;
                continue;
            }
            size_t end = out.find('&', pos + klen);
            if (end == std::string::npos)
                end = out.size();
            out.replace(pos + klen, end - (pos + klen), "***");
            pos += klen + 3;
        }
    }
    return out;
}

Client::Client(std::string server, std::string username, std::string password, HttpGet http)
    : server_(std::move(server)), username_(std::move(username)), password_(std::move(password)),
      http_(std::move(http))
{
    while (!server_.empty() && server_.back() == '/')
        server_.pop_back();
}

std::string Client::build_url(const std::string &endpoint, const std::string &extra_query) const
{
    const size_t scheme = server_.find("://");
    if ((server_.compare(0, 7, "http://") != 0 && server_.compare(0, 8, "https://") != 0) ||
        scheme == std::string::npos || scheme + 3 == server_.size() ||
        server_.find_first_of("?#@\r\n\t ") != std::string::npos ||
        endpoint.empty() || endpoint.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        return {};
    std::string url = server_ + "/rest/" + endpoint + "?";
    if (!api_key_.empty()) {
        url += "apiKey=" + url_escape(api_key_);
    } else {
        const std::string salt = random_salt();
        url += "u=" + url_escape(username_) + "&t=" + make_token(password_, salt) + "&s=" + salt;
    }
    url += "&v=";
    url += kApiVersion;
    url += "&c=";
    url += kClientName;
    url += "&f=json";
    if (!extra_query.empty()) {
        url += "&";
        url += extra_query;
    }
    return url;
}

std::string Client::stream_url(const std::string &song_id, const std::string &format,
                               int max_bit_rate, int time_offset) const
{
    std::string extra = "id=" + url_escape(song_id);
    if (!format.empty())
        extra += "&format=" + url_escape(format);
    if (max_bit_rate > 0)
        extra += "&maxBitRate=" + std::to_string(max_bit_rate);
    if (time_offset > 0)
        extra += "&timeOffset=" + std::to_string(time_offset);
    return build_url("stream", extra);
}

std::string Client::cover_art_url(const std::string &cover_art, int size) const
{
    if (cover_art.empty()) return {};
    std::string extra = "id=" + url_escape(cover_art);
    if (size > 0)
        extra += "&size=" + std::to_string(size);
    std::lock_guard<std::mutex> lock(cover_mutex_);
    for (auto it = cover_urls_.begin(); it != cover_urls_.end(); ++it) {
        if (it->first == extra) {
            cover_urls_.splice(cover_urls_.begin(), cover_urls_, it);
            return cover_urls_.front().second;
        }
    }
    std::string url = build_url("getCoverArt", extra);
    if (!url.empty()) {
        cover_urls_.emplace_front(std::move(extra), url);
        if (cover_urls_.size() > 256)
            cover_urls_.pop_back();
    }
    return url;
}

bool Client::ping(ServerInfo *info, std::string *error, LoginFailure *failure) const
{
    LoginFailure ignored;
    if (!failure) failure = &ignored;
    *failure = LoginFailure::None;
    error->clear();
    const std::string url = build_url("ping");
    if (!http_ || url.empty()) {
        *failure = LoginFailure::Configuration;
        *error = "invalid server URL or missing transport";
        return false;
    }
    HttpResponse r = http_(url);
    if (r.status == 401 || r.status == 403) {
        *failure = r.status == 401 ? LoginFailure::Http401 : LoginFailure::Http403;
        *error = "HTTP authorization refused";
        return false;
    }
    if (r.status >= 400) {
        *failure = LoginFailure::Http;
        *error = "HTTP request failed";
        return false;
    }
    if (r.failure != LoginFailure::None || !r.error.empty() || !r.status) {
        *failure = r.failure == LoginFailure::None ? LoginFailure::Transport : r.failure;
        *error = "HTTP transport failed";
        return false;
    }
    if (!r.ok()) {
        *failure = LoginFailure::Http;
        *error = "HTTP request failed";
        return false;
    }
    Doc doc(r.body);
    const cJSON *in = unwrap(doc, error);
    if (!in) {
        const cJSON *inner = child(doc.root, "subsonic-response");
        const int code = int_of(child(inner, "error"), "code", -1);
        *failure = code == 40 || code == 41 ? LoginFailure::Authentication : LoginFailure::Response;
        *error = "Subsonic login refused or invalid response";
        return false;
    }
    info->open_subsonic = bool_of(in, "openSubsonic");
    info->version = str_of(in, "version");
    info->server_version = str_of(in, "serverVersion");
    info->type = str_of(in, "type");
    return true;
}

namespace {

/* The shared request tail: GET, check status, unwrap the envelope. */
bool fetch(const HttpGet &http, const std::string &url, Doc *doc, const cJSON **inner,
           std::string *error, LoginFailure *failure = nullptr)
{
    LoginFailure ignored;
    if (!failure) failure = &ignored;
    *failure = LoginFailure::None;
    error->clear();
    if (!http || url.empty()) {
        *failure = LoginFailure::Configuration;
        *error = "invalid server URL or missing transport";
        return false;
    }
    HttpResponse r = http(url);
    if (r.status == 401 || r.status == 403) {
        *failure = r.status == 401 ? LoginFailure::Http401 : LoginFailure::Http403;
        *error = "HTTP authorization refused";
        return false;
    }
    if (r.failure != LoginFailure::None || !r.error.empty() || !r.status) {
        /* Transport diagnostics can contain credential-bearing URLs. */
        *failure = r.failure == LoginFailure::None ? LoginFailure::Transport : r.failure;
        *error = "HTTP transport failed";
        return false;
    }
    if (!r.ok()) {
        *failure = LoginFailure::Http;
        *error = "HTTP " + std::to_string(r.status);
        return false;
    }
    doc->reset(r.body);
    *inner = unwrap(*doc, error);
    if (!*inner) {
        const int code = int_of(child(child(doc->root, "subsonic-response"), "error"), "code", -1);
        *failure = code == 40 || code == 41 ? LoginFailure::Authentication : LoginFailure::Response;
    }
    return *inner != nullptr;
}

} // namespace

#define SUBSONIC_GET(endpoint, extra, doc, inner, error) \
    Doc doc; \
    const cJSON *inner = nullptr; \
    if (!fetch(http_, build_url(endpoint, extra), &doc, &inner, error)) \
        return false;

bool Client::get_extensions(std::vector<Extension> *out, std::string *error, LoginFailure *failure) const
{
    Doc doc;
    const cJSON *in = nullptr;
    if (!fetch(http_, build_url("getOpenSubsonicExtensions", ""), &doc, &in, error, failure))
        return false;
    out->clear();
    for (const cJSON *e : items(in, "openSubsonicExtensions")) {
        Extension ext;
        ext.name = str_of(e, "name");
        const cJSON *vs = child(e, "versions");
        if (cJSON_IsArray(vs)) {
            const cJSON *v = nullptr;
            cJSON_ArrayForEach(v, vs) {
                double version;
                if (cJSON_IsNumber(v)) {
                    version = v->valuedouble;
                } else if (cJSON_IsString(v) && v->valuestring) {
                    char *end = nullptr;
                    version = std::strtod(v->valuestring, &end);
                    if (end == v->valuestring || *end != '\0') continue;
                } else {
                    continue;
                }
                if (std::isfinite(version) && std::trunc(version) == version &&
                    version >= std::numeric_limits<int>::min() &&
                    version <= std::numeric_limits<int>::max())
                    ext.versions.push_back(static_cast<int>(version));
            }
        }
        out->push_back(std::move(ext));
    }
    return true;
}

bool Client::get_artists(std::vector<Artist> *out, std::string *error) const
{
    SUBSONIC_GET("getArtists", "", doc, in, error);
    out->clear();
    /* artists.index[] -> { name: "A", artist[]: {...} }; also accept a flat
     * artists.artist[] list. */
    for (const cJSON *index : items(child(in, "artists"), "index"))
        for (const cJSON *a : items(index, "artist"))
            out->push_back(to_artist(a));
    if (out->empty())
        for (const cJSON *a : items(child(in, "artists"), "artist"))
            out->push_back(to_artist(a));
    return true;
}

bool Client::get_artist(const std::string &id, Artist *artist, std::vector<Album> *albums,
                        std::string *error) const
{
    SUBSONIC_GET("getArtist", "id=" + url_escape(id), doc, in, error);
    const cJSON *a = child(in, "artist");
    *artist = to_artist(a);
    albums->clear();
    for (const cJSON *al : items(a, "album"))
        albums->push_back(to_album(al));
    return true;
}

bool Client::get_top_songs(const std::string &artist, std::vector<Song> *songs, std::string *error) const
{
    songs->clear();
    if (artist.empty()) { *error = "Artist name required"; return false; }
    SUBSONIC_GET("getTopSongs", "artist=" + url_escape(artist) + "&count=20", doc, in, error);
    const auto *top = child(in, "topSongs");
    if (!cJSON_IsObject(top)) { *error = "Missing topSongs"; return false; }
    std::vector<Song> parsed;
    for (const auto *entry : items(top, "song")) {
        auto song = to_song(entry);
        if (song.id.empty() || parsed.size() == 20) { *error = "Invalid topSongs"; return false; }
        parsed.push_back(std::move(song));
    }
    const auto *entries = child(top, "song");
    if (entries && !cJSON_IsArray(entries) && !cJSON_IsObject(entries)) {
        *error = "Invalid topSongs"; return false;
    }
    *songs = std::move(parsed);
    return true;
}

bool Client::get_album(const std::string &id, Album *album, std::vector<Song> *songs,
                       std::string *error) const
{
    SUBSONIC_GET("getAlbum", "id=" + url_escape(id), doc, in, error);
    const cJSON *a = child(in, "album");
    *album = to_album(a);
    songs->clear();
    for (const cJSON *t : items(a, "song"))
        songs->push_back(to_song(t));
    return true;
}

bool Client::get_album_list(const std::string &type, int size, int offset, const std::string &genre,
                            std::vector<Album> *out, std::string *error,
                            std::optional<int> from_year, std::optional<int> to_year) const
{
    if ((from_year && (*from_year < 0 || *from_year > 9999)) ||
        (to_year && (*to_year < 0 || *to_year > 9999)) ||
        (type == "byYear" && (!from_year || !to_year))) {
        *error = "byYear requires both year bounds in 0..9999";
        return false;
    }
    std::string extra = "type=" + url_escape(type) + "&size=" + std::to_string(size) +
                        "&offset=" + std::to_string(offset);
    if (!genre.empty())
        extra += "&genre=" + url_escape(genre);
    if (type == "byYear")
        extra += "&fromYear=" + std::to_string(*from_year) + "&toYear=" + std::to_string(*to_year);
    SUBSONIC_GET("getAlbumList2", extra, doc, in, error);
    out->clear();
    for (const cJSON *a : items(child(in, "albumList2"), "album"))
        out->push_back(to_album(a));
    return true;
}

bool Client::get_genres(std::vector<Genre> *out, std::string *error) const
{
    SUBSONIC_GET("getGenres", "", doc, in, error);
    out->clear();
    for (const cJSON *g : items(child(in, "genres"), "genre")) {
        Genre genre;
        genre.name = cJSON_IsString(g) && g->valuestring ? g->valuestring : str_of(g, "value");
        genre.song_count = (int)int_of(g, "songCount");
        genre.album_count = (int)int_of(g, "albumCount");
        if (!genre.name.empty())
            out->push_back(std::move(genre));
    }
    return true;
}

bool Client::get_playlists(std::vector<Playlist> *out, std::string *error) const
{
    SUBSONIC_GET("getPlaylists", "", doc, in, error);
    out->clear();
    for (const cJSON *p : items(child(in, "playlists"), "playlist")) {
        Playlist pl;
        pl.id = str_of(p, "id");
        pl.name = str_of(p, "name");
        pl.comment = str_of(p, "comment");
        pl.owner = str_of(p, "owner");
        pl.cover_art = str_of(p, "coverArt");
        pl.created = str_of(p, "created");
        pl.changed = str_of(p, "changed");
        pl.song_count = int_of(p, "songCount", -1);
        pl.duration = (int)int_of(p, "duration");
        pl.is_public = bool_of(p, "public");
        out->push_back(std::move(pl));
    }
    return true;
}

bool Client::get_playlist(const std::string &id, Playlist *playlist, std::vector<Song> *songs,
                          std::string *error) const
{
    SUBSONIC_GET("getPlaylist", "id=" + url_escape(id), doc, in, error);
    const cJSON *p = child(in, "playlist");
    songs->clear();
    for (const cJSON *x : items(p, "entry"))
        songs->push_back(to_song(x));
    playlist->id = str_of(p, "id");
    playlist->name = str_of(p, "name");
    playlist->comment = str_of(p, "comment");
    playlist->owner = str_of(p, "owner");
    playlist->cover_art = str_of(p, "coverArt");
    playlist->created = str_of(p, "created");
    playlist->changed = str_of(p, "changed");
    playlist->song_count = int_of(p, "songCount", -1);
    playlist->duration = (int)int_of(p, "duration");
    playlist->is_public = bool_of(p, "public");
    return true;
}

bool Client::get_random_songs(int size, const std::string &genre, std::vector<Song> *out,
                              std::string *error) const
{
    std::string extra = "size=" + std::to_string(size);
    if (!genre.empty())
        extra += "&genre=" + url_escape(genre);
    SUBSONIC_GET("getRandomSongs", extra, doc, in, error);
    out->clear();
    for (const cJSON *t : items(child(in, "randomSongs"), "song"))
        out->push_back(to_song(t));
    return true;
}

bool Client::search3(const std::string &query, int artist_count, int album_count, int song_count,
                     SearchResult *out, std::string *error, int artist_offset) const
{
    if (artist_offset < 0) { *error = "invalid artist offset"; return false; }
    std::string extra = "query=" + url_escape(query) +
                        (artist_offset ? "&artistOffset=" + std::to_string(artist_offset) : "") +
                        "&artistCount=" + std::to_string(artist_count) +
                        "&albumCount=" + std::to_string(album_count) +
                        "&songCount=" + std::to_string(song_count);
    SUBSONIC_GET("search3", extra, doc, in, error);
    const cJSON *res = child(in, "searchResult3");
    out->artists.clear();
    out->albums.clear();
    out->songs.clear();
    for (const cJSON *a : items(res, "artist"))
        out->artists.push_back(to_artist(a));
    for (const cJSON *a : items(res, "album"))
        out->albums.push_back(to_album(a));
    for (const cJSON *t : items(res, "song"))
        out->songs.push_back(to_song(t));
    return true;
}

namespace {

void info_fields(const cJSON *i, std::string *text, std::string *mbid, std::string *lfm,
                 std::string *small, std::string *medium, std::string *large)
{
    *text = str_of(i, "biography");
    if (text->empty())
        *text = str_of(i, "notes");
    *mbid = str_of(i, "musicBrainzId");
    *lfm = str_of(i, "lastFmUrl");
    *small = str_of(i, "smallImageUrl");
    *medium = str_of(i, "mediumImageUrl");
    *large = str_of(i, "largeImageUrl");
}

} // namespace

bool Client::get_artist_info2(const std::string &id, ArtistInfo *out, std::string *error) const
{
    SUBSONIC_GET("getArtistInfo2", "id=" + url_escape(id), doc, in, error);
    const cJSON *i = child(in, "artistInfo2");
    info_fields(i, &out->biography, &out->music_brainz_id, &out->last_fm_url,
                &out->small_image_url, &out->medium_image_url, &out->large_image_url);
    out->similar.clear();
    for (const cJSON *a : items(i, "similarArtist"))
        out->similar.push_back(to_artist(a));
    return true;
}

bool Client::get_album_info2(const std::string &id, AlbumInfo *out, std::string *error) const
{
    SUBSONIC_GET("getAlbumInfo2", "id=" + url_escape(id), doc, in, error);
    const cJSON *i = child(in, "albumInfo");
    info_fields(i, &out->notes, &out->music_brainz_id, &out->last_fm_url,
                &out->small_image_url, &out->medium_image_url, &out->large_image_url);
    return true;
}

bool Client::get_lyrics(const std::string &song_id, const std::string &artist,
                        const std::string &title, Lyrics *out, std::string *error) const
{
    /* songLyrics extension: structuredLyrics[] with synced line times. */
    {
        Doc doc;
        const cJSON *in = nullptr;
        const bool ok = fetch(http_, build_url("getLyricsBySongId", "id=" + url_escape(song_id)),
                              &doc, &in, error);
        if (!ok) {
            const int code = int_of(child(child(doc.root, "subsonic-response"), "error"), "code");
            if (!doc.root || (code != 0 && code != 70))
                return false; // auth and other errors must not be hidden by fallback
        }
        const cJSON *list = child(in, "lyricsList");
        if (!list)
            list = in; // tolerate legacy flat structuredLyrics
        for (const cJSON *l : items(list, "structuredLyrics")) {
            out->artist = str_of(l, "displayArtist");
            out->title = str_of(l, "displayTitle");
            out->synced = bool_of(l, "synced");
            out->offset = int_of(l, "offset");
            out->lines.clear();
            out->value.clear();
            for (const cJSON *line : items(l, "line")) {
                LyricLine ll;
                ll.start = (int)int_of(line, "start");
                ll.text = str_of(line, "value");
                out->lines.push_back(std::move(ll));
            }
            return true;
        }
        /* none: fall through to plain getLyrics */
    }
    SUBSONIC_GET("getLyrics", "artist=" + url_escape(artist) + "&title=" + url_escape(title),
                 doc, in, error);
    const cJSON *l = child(in, "lyrics");
    out->artist = str_of(l, "artist");
    out->title = str_of(l, "title");
    out->value = cJSON_IsString(l) && l->valuestring ? l->valuestring : str_of(l, "value");
    out->synced = false;
    out->offset = 0;
    out->lines.clear();
    return true;
}

namespace {

bool ack(const HttpGet &http, const std::string &url, std::string *error)
{
    Doc doc;
    const cJSON *in = nullptr;
    return fetch(http, url, &doc, &in, error);
}

} // namespace

bool Client::scrobble(const std::string &song_id, bool submission, std::string *error) const
{
    return ack(http_, build_url("scrobble", "id=" + url_escape(song_id) + "&submission=" +
                                                (submission ? "true" : "false")),
               error);
}

bool Client::star(const std::string &id, std::string *error, StarType type) const
{
    const char *key = type == StarType::artist ? "artistId=" : type == StarType::album ? "albumId=" : "id=";
    return ack(http_, build_url("star", key + url_escape(id)), error);
}

bool Client::unstar(const std::string &id, std::string *error, StarType type) const
{
    const char *key = type == StarType::artist ? "artistId=" : type == StarType::album ? "albumId=" : "id=";
    return ack(http_, build_url("unstar", key + url_escape(id)), error);
}

bool Client::get_starred2(Starred *out, std::string *error) const
{
    SUBSONIC_GET("getStarred2", "", doc, in, error);
    const cJSON *s = child(in, "starred2");
    out->artists.clear();
    out->albums.clear();
    out->songs.clear();
    for (const cJSON *a : items(s, "artist"))
        out->artists.push_back(to_artist(a));
    for (const cJSON *a : items(s, "album"))
        out->albums.push_back(to_album(a));
    for (const cJSON *t : items(s, "song"))
        out->songs.push_back(to_song(t));
    return true;
}

} // namespace subsonic
