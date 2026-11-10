/* Navi5 — memory-only console smoke session.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/smoke_session.h"
#include "app/spawn.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <set>
extern "C" {
#include <libavutil/random_seed.h>
}
#ifdef NAVI5_DEV_TOOLS
#include "socket_trace.h"
#endif

namespace app::smoke {
ServerInput prepare_server(std::string &s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    s = first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
    const auto scheme_end = s.find("://");
    if (scheme_end != std::string::npos) {
        for (size_t i = 0; i < scheme_end; ++i)
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 'a' - 'A';
    }
    if (valid_server(s)) return ServerInput::Valid;
    if (s.find("://") == std::string::npos && valid_server("https://" + s)) return ServerInput::NeedsScheme;
    return ServerInput::Invalid;
}
const char *login_failure_key(subsonic::LoginFailure failure)
{
    using F = subsonic::LoginFailure;
    switch (failure) {
    case F::Http401: return "HTTP 401: Serveren avviste innlogging. Kontroller konto og proxy.";
    case F::Http403: return "HTTP 403: Tilgang nektet. Kontroller kontotilgang og proxy.";
    case F::Authentication: return "Subsonic 40/41: Feil brukernavn eller passord. Skriv inn på nytt.";
    case F::Connection: return "Ingen forbindelse. Kontroller serveradresse, port og PS5-nettverk.";
    case F::Timeout: return "Tilkoblingen tok for lang tid. Kontroller port og PS5-nettverk.";
    case F::CaBundle: return "HTTPS CA-fil mangler eller er ugyldig. Installer gyldig CA-fil.";
    case F::Untrusted: return "HTTPS-sertifikat ikke godkjent. Kontroller navn, tillit og konsollklokke.";
    case F::Tls: return "TLS-forhandling feilet. Kontroller HTTPS-støtte og port.";
    case F::Initialization: return "HTTP kunne ikke initialiseres. Start appen på nytt.";
    case F::Configuration: return "HTTP-oppsett feilet. Kontroller adresse og appversjon.";
    case F::Redirect: return "Omdirigering blokkert. Bruk serverens endelige adresse.";
    case F::Http: return "Serveren returnerte HTTP-feil. Kontroller port, sti og proxy.";
    case F::Response: return "Uventet Subsonic-svar. Kontroller serversti og API-støtte.";
    default: return "Nettverksforespørsel feilet. Kontroller PS5-nettverk og server.";
    }
}
namespace {
using namespace ui::music;
Item item(const subsonic::Client &c, const subsonic::Album &a) { return {Kind::Album, a.id, a.name, a.artist, a.cover_art.empty() ? "" : c.cover_art_url(a.cover_art, 720), a.duration}; }
Item item(const subsonic::Client &c, const subsonic::Song &s) { return {Kind::Track, s.id, s.title, s.artist, s.cover_art.empty() ? "" : c.cover_art_url(s.cover_art, 720), s.duration, s.artist_id}; }
}
bool Library::load(const subsonic::Client &client, const Request &r, Page &page,
                   const std::function<bool()> &stale, const std::vector<Item> &recent)
{
    page = {};
    if (r.limit != State::page_limit) return false;
    unsigned offset = 0;
    for (char c : r.cursor) {
        if (c < '0' || c > '9' || offset > (INT_MAX - 40u) / 10u) return false;
        offset = offset * 10 + unsigned(c - '0');
    }
    if (offset > INT_MAX - 40u) return false;
    std::string error;
    std::vector<Item> rows;
    if (r.view == View::Search) {
        if (!r.cursor.empty() || !r.parent_id.empty() || !Request::valid_query(r.query) ||
            r.query.find_first_not_of(' ') == std::string::npos || (stale && stale())) return false;
        subsonic::SearchResult result;
        if (!client.search3(r.query, 5, 5, 5, &result, &error) ||
            result.artists.size() > 5 || result.songs.size() > 5 || result.albums.size() > 5 || (stale && stale())) return false;
        std::set<std::string> ids;
        for (const auto &s : result.songs) {
            if (s.id.empty()) return false;
            if (ids.insert(s.id).second) page.items.push_back(item(client, s));
        }
        ids.clear();
        for (const auto &a : result.artists) {
            if (a.id.empty()) return false;
            if (ids.insert(a.id).second) page.items.push_back({Kind::Artist, a.id, a.name, {},
                a.cover_art.empty() ? "" : client.cover_art_url(a.cover_art, 720), 0, {}, a.album_count});
        }
        ids.clear();
        for (const auto &a : result.albums) {
            if (a.id.empty()) return false;
            if (ids.insert(a.id).second) page.items.push_back(item(client, a));
        }
        if (!playlists_) {
            std::vector<subsonic::Playlist> lists;
            // getPlaylists has no paging. Refuse oversized indexes, never imply exhaustive search.
            if (!client.get_playlists(&lists, &error) || lists.size() > 2000 || (stale && stale())) return false;
            std::vector<Item> index;
            ids.clear();
            size_t bytes = 0;
            for (const auto &p : lists) {
                bytes += p.id.size() + p.name.size() + p.owner.size() + p.cover_art.size();
                if (p.id.empty() || bytes > (1u << 20)) return false;
                if (ids.insert(p.id).second) index.push_back({Kind::Playlist, p.id, p.name, p.owner,
                    p.cover_art.empty() ? "" : client.cover_art_url(p.cover_art, 720), p.duration, {}, p.song_count});
            }
            playlists_ = std::move(index);
        }
        // ASCII case-insensitive substring; non-ASCII UTF-8 bytes match exactly.
        const auto fold = [](std::string s) { for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; };
        auto needle = fold(r.query);
        const auto first = needle.find_first_not_of(' ');
        needle = first == std::string::npos ? "" : needle.substr(first, needle.find_last_not_of(' ') - first + 1);
        if (!needle.empty() && needle.back() == '*') needle.pop_back(); // Navidrome trims one trailing '*'.
        // Match Navidrome's <2-byte short-query guard locally so a single ASCII letter
        // cannot produce playlist-only results while search3 returns empty other groups.
        // ponytail: ASCII fold/space trim only; UTF-8 bytes stay exact, without upstream
        // Unicode lowercase/unaccent. Add shared Unicode normalization for full parity.
        if (needle.size() < 2) return !(stale && stale());
        unsigned found = 0;
        for (const auto &p : *playlists_) {
            if (fold(p.title).find(needle) != std::string::npos) {
                page.items.push_back(p);
                if (++found == 5) break;
            }
        }
        return !(stale && stale());
    }
    if (r.view == View::Home) {
        if (!r.cursor.empty()) return false;
        page.items = recent;
        for (auto &i : page.items)
            if (!i.artwork_url.empty()) i.artwork_url = client.cover_art_url(i.artwork_url, 720);
        page.sections.push_back({0, page.items.size(), true});
        unsigned shelf = 1;
        for (const char *type : {"newest"}) {
            if (stale && stale()) return false;
            std::vector<subsonic::Album> albums;
            const bool requested = !r.home_retry || (r.home_retry & (1u << shelf));
            ++shelf;
            bool ok = requested && client.get_album_list(type, RecentContexts::limit, 0, "", &albums, &error);
            ok = ok && albums.size() <= RecentContexts::limit;
            for (const auto &a : albums) ok = ok && !a.id.empty();
            Section section{page.items.size(), ok ? albums.size() : 0, ok};
            if (ok) for (const auto &a : albums) page.items.push_back(item(client, a));
            page.sections.push_back(section);
        }
        return true; // Partial failure belongs to its shelf, not the whole Home.
    }
    if (r.view == View::Albums) {
        std::vector<subsonic::Album> albums;
        if (!client.get_album_list("alphabeticalByName", r.limit,
                                   offset, "", &albums, &error)) return false;
        if (albums.size() > r.limit) return false;
        for (const auto &a : albums) rows.push_back(item(client, a));
        if (rows.size() == r.limit) page.next_cursor = std::to_string(offset + r.limit);
        page.items = std::move(rows);
        return true;
    }
    if (r.view == View::Artists) {
        // getArtists has no paging: large indexes can time out server-side before JSON.
        // OpenSubsonic empty search is library sync, bounded to this visible page.
        // search3 has no role filter: ~85% of real rows are composer/track-only/featured
        // artists with no albums. Scan raw 200-row batches (~64 KB, <=1.1s cold live)
        // until 40 cards, server end, or 5-request budget.
        constexpr unsigned batch = 200, budget = 5;
        if (offset > unsigned(INT_MAX) - batch * budget) return false;
        unsigned raw = offset;
        bool end = false;
        for (unsigned n = 0; n < budget && !end && rows.size() < r.limit; ++n) {
            if (stale && stale()) return false;
            subsonic::SearchResult result;
            if (!client.search3("", batch, 0, 0, &result, &error, int(raw)) || result.artists.size() > batch)
                return false;
            end = result.artists.size() < batch;
            size_t used = 0;
            for (const auto &a : result.artists) {
                if (rows.size() == r.limit) { end = false; break; } // Leftovers start next page.
                if (a.id.empty()) return false;
                ++used;
                if (a.album_count == 0) continue;
                rows.push_back({Kind::Artist, a.id, a.name, {}, a.cover_art.empty() ? "" : client.cover_art_url(a.cover_art, 720), 0, {}, a.album_count});
            }
            raw += unsigned(used);
        }
        if (stale && stale()) return false;
        page.start_index = offset;
        // ponytail: budget can expire with fewer than 40 cards, or none, while a cursor
        // still exists (raw offset after last consumed row). First-page warmup uses
        // this same budget, never an unbounded getArtists/full-library sync.
        if (!end) page.next_cursor = std::to_string(raw);
        page.items = std::move(rows);
        return true;
    }
    const std::string key = std::to_string(static_cast<int>(r.view)) + ":" + r.parent_id;
    auto cached = cache_.find(key);
    if (cached == cache_.end()) {
        Detail detail;
        std::vector<Item> top_songs;
        bool top_songs_ok = false;
        switch (r.view) {
        case View::Playlists: {
            std::vector<subsonic::Playlist> lists;
            if (!client.get_playlists(&lists, &error)) return false;
            for (const auto &p : lists) rows.push_back({Kind::Playlist, p.id, p.name, p.owner, p.cover_art.empty() ? "" : client.cover_art_url(p.cover_art, 720), p.duration, {}, p.song_count});
            break;
        }
        case View::ArtistAlbums: {
            subsonic::Artist artist;
            std::vector<subsonic::Album> albums;
            if (r.parent_id.empty() || !client.get_artist(r.parent_id, &artist, &albums, &error)) return false;
            if (artist.id != r.parent_id) return false;
            if (stale && stale()) return false;
            std::vector<subsonic::Song> songs;
            top_songs_ok = client.get_top_songs(artist.name, &songs, &error);
            if (stale && stale()) return false;
            // Name-based servers can collide. Reject mismatched explicit IDs; never
            // replace unavailable rankings with arbitrary album tracks.
            for (const auto &s : songs)
                if (!s.artist_id.empty() && s.artist_id != r.parent_id) top_songs_ok = false;
            if (top_songs_ok) for (const auto &s : songs) top_songs.push_back(item(client, s));
            for (const auto &a : albums) rows.push_back(item(client, a));
            break;
        }
        case View::AlbumTracks: case View::PlaylistTracks: {
            subsonic::Album album;
            subsonic::Playlist playlist;
            std::vector<subsonic::Song> songs;
            if (r.parent_id.empty()) return false;
            bool ok = r.view == View::AlbumTracks ? client.get_album(r.parent_id, &album, &songs, &error) :
                client.get_playlist(r.parent_id, &playlist, &songs, &error);
            if (!ok) return false;
            const bool is_album = r.view == View::AlbumTracks;
            const auto &cover = is_album ? album.cover_art : playlist.cover_art;
            context = {is_album ? Kind::Album : Kind::Playlist, r.parent_id,
                is_album ? album.name : playlist.name, is_album ? album.artist : playlist.owner, cover};
            detail = {is_album ? album.name : playlist.name, is_album ? album.artist : playlist.owner,
                cover.empty() ? "" : client.cover_art_url(cover, 720), is_album ? album.genre : "",
                is_album ? album.year : 0, is_album ? album.song_count : playlist.song_count,
                is_album ? album.duration : playlist.duration};
            for (auto &s : songs) {
                if (s.cover_art.empty()) s.cover_art = cover;
                if (s.artist_id.empty() && !s.artist.empty() && s.artist == album.artist)
                    s.artist_id = album.artist_id;
                rows.push_back(item(client, s));
            }
            break;
        }
        default: return false;
        }
        // Never retain malformed entries or silently skip them.
        for (const auto &row : rows) if (row.id.empty()) return false;
        // ponytail: one detail/index cache; revisit refetches, bounded account lifetime.
        if (stale && stale()) return false;
        cache_.clear();
        Page entry;
        entry.items = std::move(rows);
        entry.detail = std::move(detail);
        entry.top_songs = std::move(top_songs);
        entry.top_songs_ok = top_songs_ok;
        if (stale && stale()) return false;
        cached = cache_.emplace(key, std::move(entry)).first;
    }
    const auto &all = cached->second.items;
    if (offset > all.size()) return false;
    page.detail = cached->second.detail;
    page.top_songs = cached->second.top_songs;
    page.top_songs_ok = cached->second.top_songs_ok;
    const size_t end = std::min(all.size(), size_t(offset) + r.limit);
    page.start_index = offset;
    page.items.assign(all.begin() + offset, all.begin() + end);
    if (end < all.size()) page.next_cursor = std::to_string(end);
    return true;
}
bool Library::queue(const subsonic::Client &client, const PlayIntent &intent, PlayQueue &queue,
                    const std::function<bool()> &stale, std::string *error)
{
    const auto fail = [&](const char *message) { if (error) *error = message; return false; };
    if (stale && stale()) return false;
    if (intent.action == PlayAction::ArtistPlayAll || intent.action == PlayAction::ArtistShuffle) {
        if (intent.context != View::ArtistAlbums || intent.context_id.empty() || !intent.song_id.empty() ||
            intent.occurrence || intent.page_offset || !intent.page_ids.empty())
            return fail("Ugyldig avspillingskontekst for artist");
        // getArtist/getAlbum have no paging. Bound aggregate work, never play a prefix.
        // Transport separately caps each response at 16 MiB and each request at 15s.
        constexpr size_t max_albums = 256, max_tracks = 20000, max_metadata = 16u << 20;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        const auto cancelled = [&] { return stale && stale(); };
        const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
        subsonic::Artist artist;
        std::vector<subsonic::Album> albums;
        std::string ignored;
        if (!client.get_artist(intent.context_id, &artist, &albums, &ignored))
            return fail("Kunne ikke laste artistalbum");
        if (cancelled()) return false;
        if (expired()) return fail("Lasting av artistavspilling tok for lang tid");
        if (artist.id != intent.context_id || artist.album_count < 0 ||
            size_t(artist.album_count) != albums.size()) return fail("Ufullstendig albumliste for artist");
        if (albums.size() > max_albums) return fail("Artist overskrider avspillingsgrensen (256 album)");
        if (albums.empty()) return fail("Artisten har ingen album å spille");
        std::set<std::string> album_ids;
        std::vector<subsonic::Song> songs;
        size_t metadata = 0;
        for (const auto &entry : albums) {
            if (cancelled()) return false;
            if (expired()) return fail("Lasting av artistavspilling tok for lang tid");
            if (entry.id.empty() || !album_ids.insert(entry.id).second)
                return fail("Ugyldig albumliste for artist");
            subsonic::Album album;
            std::vector<subsonic::Song> tracks;
            if (!client.get_album(entry.id, &album, &tracks, &ignored))
                return fail("Kunne ikke laste alle artistalbum");
            if (cancelled()) return false;
            if (expired()) return fail("Lasting av artistavspilling tok for lang tid");
            if (album.id != entry.id || album.song_count < 0 || entry.song_count < 0 ||
                size_t(album.song_count) != tracks.size() || size_t(entry.song_count) != tracks.size())
                return fail("Ufullstendige spor i artistalbum");
            if (tracks.size() > max_tracks - songs.size())
                return fail("Artist overskrider avspillingsgrensen (20000 spor)");
            // Preserve server album/track order and duplicate song occurrences.
            for (auto &song : tracks) {
                if (song.id.empty()) return fail("Ugyldig spor i artistalbum");
                if (song.cover_art.empty()) song.cover_art = album.cover_art;
                if (song.artist_id.empty() && song.artist == album.artist) song.artist_id = album.artist_id;
                if (!song.cover_art.empty()) song.cover_art = client.cover_art_url(song.cover_art, 720);
                const size_t bytes = song.id.size() + song.title.size() + song.artist.size() + song.artist_id.size() +
                    song.album.size() + song.album_id.size() + song.cover_art.size() + song.genre.size() +
                    song.content_type.size() + song.suffix.size();
                if (bytes > max_metadata - metadata) return fail("Artist overskrider metadatagrensen (16 MiB)");
                metadata += bytes;
                songs.push_back(std::move(song));
            }
        }
        if (songs.empty()) return fail("Artisten har ingen spor å spille");
        if (cancelled()) return false;
        PlayQueue next(std::move(songs));
        if (intent.action == PlayAction::ArtistShuffle) {
            std::mt19937 rng(av_get_random_seed());
            // set_shuffle preserves current occurrence: choose it uniformly first.
            const auto first = std::uniform_int_distribution<size_t>(0, next.tracks().size() - 1)(rng);
            for (size_t i = 0; i < first; ++i) next.next();
            next.set_shuffle(true, rng);
        }
        queue = std::move(next);
        context = {Kind::Artist, intent.context_id, artist.name, {}, artist.cover_art};
        return true;
    }
    if (intent.context == View::ArtistAlbums) {
        if (intent.action != PlayAction::Detail) return fail("Ugyldig avspillingshandling");
        const auto &rows = intent.top_snapshot;
        if (intent.context_id.empty() || intent.song_id.empty() || rows.empty() || rows.size() > 20 ||
            intent.occurrence >= rows.size() || intent.page_offset || intent.page_ids.size() != rows.size() ||
            rows[intent.occurrence].id != intent.song_id) return false;
        std::vector<subsonic::Song> songs;
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto &row = rows[i];
            if (row.kind != Kind::Track || row.id.empty() || row.id != intent.page_ids[i] ||
                (!row.artist_id.empty() && row.artist_id != intent.context_id)) return false;
            subsonic::Song song; song.id = row.id; song.title = row.title;
            song.artist = row.subtitle; song.artist_id = row.artist_id;
            song.duration = row.duration; song.cover_art = row.artwork_url;
            songs.push_back(std::move(song));
        }
        if (stale && stale()) return false;
        PlayQueue next(std::move(songs));
        for (size_t i = 0; i < intent.occurrence; ++i) next.next();
        queue = std::move(next);
        return true;
    }
    if (intent.context == View::Search) {
        // Search songs have no artist/album/playlist context: never recorded as recent.
        if (intent.action != PlayAction::Detail || !intent.top_snapshot.empty()) return fail("Ugyldig avspillingshandling");
        context = {};
        if (!Request::valid_query(intent.context_id) || intent.context_id.find_first_not_of(' ') == std::string::npos ||
            intent.song_id.empty() || intent.page_offset || intent.page_ids.empty() || intent.page_ids.size() > 5 ||
            intent.occurrence >= intent.page_ids.size()) return false;
        subsonic::SearchResult result;
        std::string ignored;
        if (!client.search3(intent.context_id, 0, 0, 5, &result, &ignored) || result.songs.size() > 5 || (stale && stale())) return false;
        std::set<std::string> ids;
        std::vector<subsonic::Song> songs;
        for (auto &s : result.songs) {
            if (s.id.empty()) return false;
            if (!ids.insert(s.id).second) continue;
            if (!s.cover_art.empty()) s.cover_art = client.cover_art_url(s.cover_art, 720);
            songs.push_back(std::move(s));
        }
        if (songs.size() != intent.page_ids.size()) return false;
        for (size_t i = 0; i < songs.size(); ++i) if (songs[i].id != intent.page_ids[i]) return false;
        if (songs[intent.occurrence].id != intent.song_id) return false;
        PlayQueue next(std::move(songs));
        for (size_t i = 0; i < intent.occurrence; ++i) next.next();
        queue = std::move(next);
        return true;
    }
    if (!intent.top_snapshot.empty()) return false;
    if (intent.action != PlayAction::Detail && intent.action != PlayAction::DetailShuffle)
        return fail("Ugyldig avspillingshandling");
    if (intent.action == PlayAction::DetailShuffle && (!intent.song_id.empty() || intent.occurrence))
        return fail("Ugyldig avspillingshandling");
    if (intent.context_id.empty() ||
        (intent.context != View::AlbumTracks && intent.context != View::PlaylistTracks)) return false;
    Request r; r.view = intent.context; r.parent_id = intent.context_id;
    Page page;
    if (!load(client, r, page)) return false;
    // One-entry detail cache: context retained by that fetch must match this exact key.
    if (context.kind != (r.view == View::AlbumTracks ? Kind::Album : Kind::Playlist) || context.id != r.parent_id) context = {};
    const auto &rows = cache_.at(std::to_string(static_cast<int>(r.view)) + ":" + r.parent_id).items;
    if (intent.occurrence >= rows.size() || (!intent.song_id.empty() && rows[intent.occurrence].id != intent.song_id) ||
        intent.page_offset > rows.size() || intent.page_ids.size() > rows.size() - intent.page_offset) return false;
    for (size_t i = 0; i < intent.page_ids.size(); ++i)
        if (rows[intent.page_offset + i].id != intent.page_ids[i]) return false;
    std::vector<subsonic::Song> songs;
    for (const auto &row : rows) {
        subsonic::Song song; song.id = row.id; song.title = row.title;
        song.artist = row.subtitle; song.artist_id = row.artist_id;
        song.duration = row.duration; song.cover_art = row.artwork_url;
        songs.push_back(std::move(song));
    }
    queue = PlayQueue(std::move(songs));
    for (size_t i = 0; i < intent.occurrence; ++i) queue.next();
    if (intent.action == PlayAction::DetailShuffle && !queue.tracks().empty()) {
        std::mt19937 rng(av_get_random_seed());
        const auto first = std::uniform_int_distribution<size_t>(0, queue.tracks().size() - 1)(rng);
        for (size_t i = 0; i < first; ++i) queue.next();
        queue.set_shuffle(true, rng);
    }
    return true;
}
namespace {
bool first_page(const Request &r)
{
    return r.view >= View::Home && r.view <= View::Playlists &&
        r.cursor.empty() && r.parent_id.empty() && !r.home_retry && r.limit == State::page_limit;
}
bool retain_page(const Page &page)
{
    if (page.items.size() > State::page_limit || page.sections.size() > 2) return false;
    size_t bytes = page.next_cursor.size();
    for (const auto &row : page.items) {
        bytes += row.id.size() + row.title.size() + row.subtitle.size() + row.artwork_url.size() + row.artist_id.size();
        if (bytes > 256u * 1024u) return false;
    }
    for (const auto &section : page.sections) if (!section.ok) return false;
    return true;
}
}
bool Mailbox::cached(const Request &request, Page &page, uint64_t current)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (quit || current != generation || !first_page(request)) return false;
    auto found = first_pages.find(request.view);
    if (found == first_pages.end()) return false;
    if (std::chrono::steady_clock::now() >= found->second.expires) {
        first_pages.erase(found);
        return false;
    }
    ++browse_revision; // Cached navigation also supersedes in-flight speculative work.
    browse_commands.clear();
    page = found->second.page;
    return true;
}
uint64_t Mailbox::reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    ++generation; ++playback_revision;
    { std::lock_guard<std::mutex> gate(epoch->mutex); epoch->value = generation; if (epoch->cancel) *epoch->cancel = true; }
    first_pages.clear(); warm_attempted = 0; warm_ready = false; ++browse_revision;
    browse_commands.clear(); browse_client.reset(); recent_contexts.clear(); ++history_revision; browse_wake.notify_one();
    commands.clear(); results.clear(); audio = {}; song_id.clear(); title.clear(); artist.clear(); artwork_url.clear(); next_title.clear(); duration = 0;
    artist_id.clear(); portrait_url.clear();
    shuffle_enabled = shuffle_pending = shuffle_error = false;
    Command c; c.generation = generation; c.playback_revision = playback_revision; c.kind = Command::Logout;
    commands.push_back(std::move(c)); wake.notify_one();
    return generation;
}
bool Mailbox::send(Command c)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (quit || c.generation != generation) return false;
    ++browse_revision; // Any explicit action outranks speculation between HTTP calls.
    if (c.kind == Command::Play) first_pages.erase(View::Home); // Recent history changes after playback.
    if (c.kind == Command::Login) {
        first_pages.clear(); warm_attempted = 0; warm_ready = false;
        browse_commands.clear(); browse_client.reset();
    }
    if (c.kind == Command::Browse) browse_commands.clear(); // Latest screen wins.
    auto &pending = c.kind == Command::Browse ? browse_commands : commands;
    if (pending.size() >= 16) return false;
    if (c.kind == Command::Previous && c.playback_revision != playback_revision) return false;
    if (c.kind == Command::Play || c.kind == Command::Stop || c.kind == Command::Next || c.kind == Command::Previous || c.kind == Command::Logout || c.kind == Command::Login)
        ++playback_revision;
    if ((c.kind == Command::Seek || c.kind == Command::ShuffleToggle) && c.playback_revision != playback_revision) return false;
    c.playback_revision = playback_revision;
    pending.push_back(std::move(c)); wake.notify_one(); browse_wake.notify_one(); return true;
}
bool start_worker(const std::shared_ptr<Mailbox> &box, subsonic::HttpGet http,
                  std::shared_ptr<AudioPlayer> injected)
{
    if (!jelly5::spawn_media([box] {
        Library library;
        uint64_t active = 0;
        std::shared_ptr<const subsonic::Client> account;
        std::map<std::string, std::string> portraits;
        for (;;) {
            Command c;
            bool browse = false, speculative = false;
            uint64_t revision = 0, history = 0;
            std::string artist_id;
            std::vector<Item> recent;
            std::shared_ptr<const subsonic::Client> client;
            {
                std::unique_lock<std::mutex> lock(box->mutex);
                box->browse_wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return box->quit || !box->browse_commands.empty(); });
                if (box->quit) return;
                c.generation = box->generation;
                if (!box->browse_commands.empty()) {
                    browse = true;
                    c = std::move(box->browse_commands.front()); box->browse_commands.pop_front();
                }
                if (c.generation != box->generation) continue;
                client = box->browse_client;
                recent = box->recent_contexts; history = box->history_revision;
                artist_id = box->artist_id;
                revision = box->browse_revision;
                // Existing browse worker only. No speculative work during playback/admission.
                if (!browse && client && box->warm_ready && box->commands.empty() &&
                    box->audio_revision == box->playback_revision &&
                    box->audio.state == AudioState::stopped && artist_id.empty()) {
                    for (auto view : {View::Artists, View::Albums, View::Playlists}) {
                        const unsigned bit = 1u << static_cast<unsigned>(view);
                        if (box->warm_attempted & bit) continue;
                        box->warm_attempted |= bit; // One attempt, including cancellation/error.
                        if (box->first_pages.count(view)) continue;
                        c.request.view = view;
                        browse = speculative = true;
                        break;
                    }
                }
            }
            if (active != c.generation || account != client) {
                library = {}; portraits.clear(); active = c.generation; account = client;
            }
            if (!browse) {
                if (!client || artist_id.empty()) continue;
                auto found = portraits.find(artist_id);
                if (found == portraits.end()) {
                    subsonic::ArtistInfo info;
                    std::string error, url;
                    if (client->get_artist_info2(artist_id, &info, &error)) {
                        for (const auto &candidate : {info.large_image_url, info.medium_image_url, info.small_image_url})
                            if (same_origin_portrait(client->server(), candidate)) { url = candidate; break; }
                    }
                    // ponytail: 32-entry account cache including misses; refetch after eviction.
                    if (portraits.size() >= 32) portraits.clear();
                    found = portraits.emplace(artist_id, std::move(url)).first;
                }
                std::lock_guard<std::mutex> lock(box->mutex);
                if (!box->quit && active == box->generation && artist_id == box->artist_id)
                    box->portrait_url = found->second;
                continue;
            }
            Result r; r.kind = Command::Browse; r.generation = c.generation; r.token = c.request.token;
            auto stale = [&] {
                std::lock_guard<std::mutex> lock(box->mutex);
                return box->quit || c.generation != box->generation ||
                    (speculative ? revision != box->browse_revision : !box->browse_commands.empty());
            };
            // Do not retain speculative playlist indexes/details in Library's one-entry cache.
            Library warm_library;
            r.ok = client && !stale() && (speculative ? warm_library : library).load(*client, c.request, r.page, stale, recent);
            std::lock_guard<std::mutex> lock(box->mutex);
            if (box->quit || c.generation != box->generation || client != box->browse_client) continue;
            // Home embeds recent history: never retain a page built from an older snapshot.
            const bool current = revision == box->browse_revision &&
                (c.request.view != View::Home || history == box->history_revision);
            if (current && r.ok && first_page(c.request) && retain_page(r.page)) {
                box->first_pages[c.request.view] = {r.page, std::chrono::steady_clock::now() + std::chrono::seconds(60)};
            }
            if (!speculative) {
                if (c.request.view == View::Home && r.ok) box->warm_ready = true;
                // Tokens still reject already-posted results after navigation.
                box->results.push_back(std::move(r));
            }
        }
    })) return false;
    bool started = jelly5::spawn_media([box, http = std::move(http), injected = std::move(injected)] {
        auto owned = injected ? injected : std::make_shared<AudioPlayer>();
        AudioPlayer &player = *owned;
        std::shared_ptr<subsonic::Client> client;
        Library library;
        PlayQueue queue;
        RecentContexts history;
        std::optional<Item> pending_context;
        uint64_t active = 0, playback = 0;
        bool advancing = false, seek_rejected = false;
        uint64_t occurrence = 0;
        std::optional<bool> shuffle_request;
        bool shuffle_error = false;
        std::mt19937 shuffle_rng(av_get_random_seed());
        auto successor_url = [&] {
            auto index = queue.successor_index();
            return index ? client->stream_url(queue.tracks()[*index].id) : std::string{};
        };
        std::string title;
        AudioStatus start_error;
        auto playback_current = [&] {
            std::lock_guard<std::mutex> lock(box->mutex);
            return !box->quit && active == box->generation && playback == box->playback_revision;
        };
        auto start_track = [&] {
            advancing = false;
            if (!playback_current()) return;
            shuffle_request.reset(); shuffle_error = false;
            const auto *song = queue.current();
            title = song ? song->title : "";
            start_error = {}; seek_rejected = false;
            occurrence = 0;
            player.stop(); // Join unlocked; admission lock covers only spawn.
            {
                std::lock_guard<std::mutex> gate(box->epoch->mutex);
                if (box->epoch->value == active) {
                    box->epoch->cancel = player.cancel_flag();
                    advancing = song && player.start(client->stream_url(song->id), successor_url());
                }
            }
            // A control may arrive while start joins the old decoder. Never retain
            // its stale replacement, and never publish its metadata.
            if (!playback_current()) { player.stop(); advancing = false; }
        };
        for (;;) {
            Command c;
            bool have = false;
            {
                std::unique_lock<std::mutex> lock(box->mutex);
                box->wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return box->quit || !box->commands.empty(); });
                if (box->quit) break;
                if (!box->commands.empty()) { c = std::move(box->commands.front()); box->commands.pop_front(); have = true; }
            }
            if (have) {
                { std::lock_guard<std::mutex> lock(box->mutex); if (c.generation != box->generation) continue; }
                Result result; result.kind = c.kind; result.generation = c.generation; result.token = c.request.token;
#ifdef NAVI5_DEV_TOOLS
                if (c.kind == Command::Login) jf::live_progress.publish(5, 10, 0, 0, 0, 0);
#endif
                // Previous may only restart current occurrence; it cancels these itself on track change.
                if (c.kind == Command::Play || c.kind == Command::Stop || c.kind == Command::Next || c.kind == Command::Logout || c.kind == Command::Login) {
                    shuffle_request.reset(); shuffle_error = false;
                    pending_context.reset();
                }
                if (c.kind == Command::Logout || c.kind == Command::Login) {
                    player.stop(); client.reset(); library = {}; queue = PlayQueue{};
                    advancing = false; title.clear(); start_error = {}; active = c.generation;
                    playback = c.playback_revision;
                }
                if (c.kind == Command::Login && valid_server(c.server)) {
                    const auto history_scope = c.api_key.empty() ? RecentContexts::scope(c.server, c.username) : std::string{};
                    if (!c.api_key.empty()) c.password.clear();
                    auto next = std::make_shared<subsonic::Client>(std::move(c.server), std::move(c.username), std::move(c.password), http);
                    if (!c.api_key.empty()) next->set_api_key(std::move(c.api_key));
                    subsonic::ServerInfo info; std::string error;
                    jf::curl_diag = {};
                    result.ok = next->ping(&info, &error, &result.failure);
                    result.diagnostic = jf::curl_diag;
#ifdef NAVI5_DEV_TOOLS
                    // No EVO logger or filesystem work between ping and mailbox delivery.
                    jf::trace_live(5, 4, 0, int(result.failure), int(result.ok), 0);
#endif
                    if (result.ok) { client = std::move(next); history = RecentContexts(history_scope); }
                } else if (client && active == c.generation) {
                    switch (c.kind) {
                    case Command::Play:
                        player.stop(); queue = PlayQueue{}; library = {}; advancing = false; title.clear();
                        playback = c.playback_revision;
                        {
                            std::string error = "Cannot load playback context";
                            if (library.queue(*client, c.play, queue, [&] { return !playback_current(); }, &error)) {
                                start_track(); result.ok = advancing;
                                if (advancing && !library.context.id.empty()) pending_context = library.context;
                            } else if (playback_current()) {
                                result.playback_error = error;
                                start_error = {AudioState::error, 0, error, AudioError::demux};
                            }
                        }
                        break;
                    case Command::Next:
                        playback = c.playback_revision;
                        if (queue.next()) start_track();
                        break;
                    case Command::Previous: {
                        { std::lock_guard<std::mutex> lock(box->mutex); if (c.playback_revision != box->playback_revision) break; }
                        playback = c.playback_revision;
                        const auto observed = player.status();
                        while (occurrence < observed.track && queue.on_track_end()) ++occurrence;
                        // Authoritative decoded position at handling time, serialized like Seek.
                        auto restart = [&](bool any_position) {
                            std::lock_guard<std::mutex> lock(box->mutex);
                            const auto s = player.status();
                            const double position = s.position;
                            if (!advancing || occurrence != s.track || s.track != s.next_track || s.seeking ||
                                (s.state != AudioState::playing && s.state != AudioState::paused) ||
                                !std::isfinite(s.duration) || s.duration <= 0 ||
                                !(any_position || (std::isfinite(position) && position > previous_restart_seconds))) return false;
                            if (c.playback_revision != box->playback_revision) return false;
                            return player.seek(0, s.track);
                        };
                        if (restart(false)) break;
                        if (queue.previous()) { shuffle_request.reset(); shuffle_error = false; pending_context.reset(); start_track(); }
                        else if (queue.current() && !restart(true)) {
                            // First entry, not seekable: replay same occurrence; keep pause/pending shuffle.
                            const auto keep = shuffle_request; start_track(); shuffle_request = keep;
                            if (observed.state == AudioState::paused) player.pause();
                        }
                        break;
                    }
                    case Command::Seek: {
                        // Serialize admission with Stop/Next/reset; never seek replacement PCM.
                        std::lock_guard<std::mutex> lock(box->mutex);
                        const auto s = player.status();
                        if (c.playback_revision != box->playback_revision || playback != c.playback_revision ||
                            c.track != s.track || s.track != s.next_track) break;
                        seek_rejected = (s.state != AudioState::playing && s.state != AudioState::paused) ||
                            !std::isfinite(c.seconds) || c.seconds < 0 ||
                            !std::isfinite(s.duration) || s.duration <= 0 || c.seconds >= s.duration ||
                            !player.seek(c.seconds, c.track);
                        break;
                    }
                    case Command::ShuffleToggle:
                        if (c.playback_revision == playback && playback_current()) {
                            shuffle_request = c.shuffle_enabled;
                            shuffle_error = false;
                        }
                        break;
                    case Command::Pause: player.pause(); break;
                    case Command::Resume: player.resume(); break;
                    case Command::Stop:
                        player.stop(); queue = PlayQueue{}; advancing = false; title.clear(); start_error = {};
                        playback = c.playback_revision; break;
                    default: break;
                    }
                }
#ifdef NAVI5_DEV_TOOLS
                if (c.kind == Command::Login) jf::trace_live(5, 5, 0, 0, 0, 0);
#endif
                bool stale;
                {
                    std::lock_guard<std::mutex> lock(box->mutex);
                    stale = c.generation != box->generation || box->quit;
                    if (!stale) {
                        if (c.kind == Command::Login) {
                            box->browse_client = client;
                            box->recent_contexts = client ? history.items() : std::vector<Item>{};
                            ++box->history_revision;
                        }
                        if (c.kind == Command::Login || c.kind == Command::Play) box->results.push_back(std::move(result));
                    }
                }
#ifdef NAVI5_DEV_TOOLS
                if (c.kind == Command::Login) {
                    jf::trace_live(5, 6, 0, 0, int(stale), 0);
                }
                jf::trace_end();
#endif
                if (stale) { player.stop(); client.reset(); library = {}; queue = PlayQueue{}; advancing = false; title.clear(); start_error = {}; }
            }
            auto status = start_error.state == AudioState::error ? start_error : player.status();
            if (status.track != occurrence) seek_rejected = false;
            if (status.state == AudioState::stopped || status.state == AudioState::ended) seek_rejected = false;
            status.seek_failed = status.seek_failed || seek_rejected;
            bool current;
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                current = !box->quit && active == box->generation && playback == box->playback_revision;
                // Drain controls before considering EOF, including pause/resume.
                current = current && box->commands.empty();
            }
            bool history_changed = false;
            if (current && pending_context) {
                if (status.state == AudioState::playing) {
                    // Commit once only after decoder/output reports playing, not spawn admission.
                    std::lock_guard<std::mutex> lock(box->mutex);
                    if (!box->quit && active == box->generation && playback == box->playback_revision && box->commands.empty()) {
                        history.promote(std::move(*pending_context));
                        box->recent_contexts = history.items(); ++box->history_revision;
                        box->first_pages.erase(View::Home);
                        pending_context.reset(); history_changed = true;
                    }
                } else if (status.state == AudioState::error || status.state == AudioState::ended || status.state == AudioState::stopped) {
                    pending_context.reset();
                }
            }
            if (history_changed) history.save(); // Worker I/O, never render thread or mailbox lock.
            if (current && advancing && client) {
                const uint64_t observed = status.state == AudioState::error ? status.error_track : status.track;
                while (occurrence < observed && queue.on_track_end()) {
                    ++occurrence;
                    title = queue.current()->title;
                }
                if (shuffle_request && status.state != AudioState::error && status.state != AudioState::ended) {
                    // Reconcile committed PCM first. Admission and prefetch replacement
                    // share occurrence guards; failed handoff/seek retries next tick.
                    auto candidate = queue;
                    candidate.set_live_shuffle(*shuffle_request, shuffle_rng);
                    auto index = candidate.successor_index();
                    auto url = index ? client->stream_url(candidate.tracks()[*index].id) : std::string{};
                    std::lock_guard<std::mutex> lock(box->mutex);
                    if (!box->quit && active == box->generation && playback == box->playback_revision &&
                        box->commands.empty() && occurrence == status.track &&
                        player.replace_next(std::move(url), occurrence)) {
                        queue = std::move(candidate);
                        shuffle_request.reset();
                    }
                }
                if (status.state != AudioState::error && status.state != AudioState::ended) {
                    auto future = queue;
                    for (uint64_t n = occurrence; n < status.next_track; ++n) future.on_track_end();
                    auto index = future.successor_index();
                    player.queue_next(index ? client->stream_url(future.tracks()[*index].id) : std::string{}, status.next_track);
                }
                if (status.state == AudioState::ended || status.state == AudioState::error) advancing = false;
            }
            if (shuffle_request && (!advancing || !queue.current())) {
                shuffle_request.reset(); shuffle_error = true;
            }
            {
                std::lock_guard<std::mutex> lock(box->mutex);
                if (!box->quit && active == box->generation && playback == box->playback_revision) {
                    box->shuffle_enabled = queue.shuffled();
                    box->shuffle_pending = shuffle_request.has_value();
                    box->shuffle_error = shuffle_error;
                    box->audio_revision = playback;
                    box->audio = std::move(status);
                    const auto *song = queue.current();
                    if (song && box->song_id != song->id) box->first_pages.erase(View::Home);
                    box->song_id = song ? song->id : "";
                    box->title = song ? song->title : "";
                    box->artist = song ? song->artist : "";
                    const std::string artist_id = song ? song->artist_id : "";
                    if (box->artist_id != artist_id) {
                        box->artist_id = artist_id;
                        box->portrait_url.clear();
                        box->browse_wake.notify_one();
                    }
                    box->duration = song ? song->duration : 0;
                    box->artwork_url = song ? song->cover_art : "";
                    const auto *next = queue.up_next();
                    box->next_title = next ? next->title : "";
                }
            }
        }
    });
    if (!started) { std::lock_guard<std::mutex> lock(box->mutex); box->quit = true; box->browse_wake.notify_one(); }
    return started;
}
} // namespace app::smoke
