/* Navi5 — bounded, account-scoped explicit play history.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/recent_contexts.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "cJSON.h"
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
namespace app {
namespace {
bool valid(const ui::music::Item &i) {
    using ui::music::Kind;
    if (i.kind != Kind::Artist && i.kind != Kind::Album && i.kind != Kind::Playlist) return false;
    for (const auto *s : {&i.id, &i.title, &i.subtitle, &i.artwork_url})
        if (s->size() > 1024 || s->find('\0') != std::string::npos) return false;
    // Cover IDs only. No server or credential-bearing artwork URLs on disk.
    return !i.id.empty() && i.artwork_url.find("://") == std::string::npos;
}
std::string field(const cJSON *j, const char *key) {
    const auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
}
std::string RecentContexts::scope(const std::string &server, const std::string &user) {
    if (server.empty() || user.empty()) return {};
    auto *sha = av_sha_alloc();
    if (!sha) return {};
    av_sha_init(sha, 256);
    // Length-delimited, no credential input. Hash is isolation key, not encryption.
    const std::string input = std::to_string(server.size()) + ":" + server + user;
    av_sha_update(sha, reinterpret_cast<const uint8_t *>(input.data()), input.size());
    uint8_t digest[32]; av_sha_final(sha, digest); av_free(sha);
    std::string out;
    for (uint8_t b : digest) { out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; }
    return out;
}
RecentContexts::RecentContexts(std::string scope, const char *path) : scope_(std::move(scope)), path_(path) {
    if (scope_.empty()) return;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return;
    struct stat st;
    char buf[32769]; size_t got = 0;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= 32768;
    while (ok && got < sizeof buf) {
        const ssize_t n = read(fd, buf + got, sizeof buf - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = n == 0; break; }
        got += size_t(n);
    }
    close(fd);
    if (!ok || got >= sizeof buf || std::memchr(buf, 0, got)) return;
    // cJSON strings truncate at decoded NUL. Conservatively reject escaped NUL too.
    if (std::string(buf, got).find("\\u0000") != std::string::npos) return;
    const char *end = nullptr;
    auto *j = cJSON_ParseWithLengthOpts(buf, got, &end, 0);
    while (j && end < buf + got && std::strchr(" \t\r\n", *end)) ++end;
    const auto *rows = cJSON_GetObjectItemCaseSensitive(j, "items");
    ok = j && end == buf + got && field(j, "scope") == scope_ && cJSON_IsArray(rows) && cJSON_GetArraySize(rows) <= int(limit);
    if (ok) for (const cJSON *r = rows->child; r; r = r->next) {
        const auto kind = field(r, "kind");
        ui::music::Item i;
        i.kind = kind == "artist" ? ui::music::Kind::Artist : kind == "playlist" ? ui::music::Kind::Playlist : ui::music::Kind::Album;
        i.id = field(r, "id"); i.title = field(r, "title"); i.subtitle = field(r, "subtitle"); i.artwork_url = field(r, "cover");
        if ((kind != "artist" && kind != "playlist" && kind != "album") || !valid(i)) { ok = false; break; }
        if (std::any_of(items_.begin(), items_.end(), [&](const auto &old) { return old.kind == i.kind && old.id == i.id; })) { ok = false; break; }
        items_.push_back(std::move(i));
    }
    cJSON_Delete(j);
    if (!ok) items_.clear();
}
void RecentContexts::promote(ui::music::Item i) {
    if (!valid(i)) return;
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const auto &old) { return old.kind == i.kind && old.id == i.id; }), items_.end());
    items_.insert(items_.begin(), std::move(i));
    if (items_.size() > limit) items_.resize(limit);
}
bool RecentContexts::save() const {
    if (scope_.empty()) return true;
    auto *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "scope", scope_.c_str());
    auto *rows = cJSON_AddArrayToObject(j, "items");
    for (const auto &i : items_) {
        auto *r = cJSON_CreateObject(); cJSON_AddItemToArray(rows, r);
        cJSON_AddStringToObject(r, "kind", i.kind == ui::music::Kind::Artist ? "artist" : i.kind == ui::music::Kind::Playlist ? "playlist" : "album");
        cJSON_AddStringToObject(r, "id", i.id.c_str()); cJSON_AddStringToObject(r, "title", i.title.c_str());
        cJSON_AddStringToObject(r, "subtitle", i.subtitle.c_str()); cJSON_AddStringToObject(r, "cover", i.artwork_url.c_str());
    }
    char *text = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!text) return false;
    if (std::strlen(text) > 32768) { std::free(text); return false; }
    const auto slash = path_.rfind('/');
    if (slash != std::string::npos) mkdir(path_.substr(0, slash).c_str(), 0700);
    const std::string tmp = path_ + ".tmp";
    unlink(tmp.c_str());
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    const size_t len = std::strlen(text); size_t put = 0;
    if (fd >= 0) while (put < len) {
        const ssize_t n = write(fd, text + put, len - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        put += size_t(n);
    }
    bool ok = fd >= 0 && put == len && fsync(fd) == 0;
    if (fd >= 0) ok = close(fd) == 0 && ok;
    ok = ok && std::rename(tmp.c_str(), path_.c_str()) == 0;
    if (!ok) unlink(tmp.c_str());
    std::free(text); return ok;
}
}
