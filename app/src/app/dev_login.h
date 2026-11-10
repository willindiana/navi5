/* Navi5 — explicit private development input; never packaged.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "cJSON.h"
#include "app/server_url.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace app::dev {
// Numeric only: stage identifies failed check; errno only meaningful for syscall failure.
struct ImportStatus { int stage = 0, error = 0; };
inline thread_local ImportStatus import_status;
inline bool rejected(int stage, int error = 0) { import_status = {stage, error}; return false; }
// Only explicit temporary app0 import permits FTP ownership/mode differences.
inline bool read_input(const char *path, size_t limit, std::string &out, bool temporary) {
    out.clear();
    // libkernel exports open/fstat; pathname metadata can be denied in native titles.
    // O_NOFOLLOW binds final-component symlink rejection to this open, not a prior lstat.
    // Parent directories must be trusted; O_NOFOLLOW does not constrain ancestors.
    if (!path || !*path || !limit || limit == SIZE_MAX) return rejected(5);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return rejected(6, errno);
    struct stat opened{};
    int failure = 0, error = 0;
    if (fstat(fd, &opened)) { failure = 7; error = errno; }
    else if (!S_ISREG(opened.st_mode)) failure = 8;
    // PS5 sandbox fstat reports st_nlink=0 for a fresh O_EXCL file, so 0 is accepted.
    // Hardlink detection is weaker there: only counts above 1 are visible and rejected.
    else if (opened.st_nlink > 1) failure = 10;
    // ponytail: no owner check. PS5 fstat reports st_uid=0 for our own new file while
    // getuid()=1, and a probe-file uid (0) would match every root-owned file: false
    // assurance. Owner check returns only if native st_uid proves process-scoped.
    else if (!temporary && (opened.st_mode & 077)) failure = 11;
    else if (opened.st_size <= 0 || static_cast<uintmax_t>(opened.st_size) > limit) failure = 5;
    if (failure) { close(fd); return rejected(failure, error); }
    std::string data(limit + 1, '\0');
    size_t n = 0;
    while (n < data.size()) {
        const ssize_t count = read(fd, data.data() + n, data.size() - n);
        if (count < 0) {
            if (errno == EINTR) continue;
            const int error = errno; close(fd); return rejected(13, error);
        }
        if (!count) break;
        n += static_cast<size_t>(count);
    }
    const int closed = close(fd), close_error = errno;
    if (closed) return rejected(12, close_error);
    if (!n || n > limit) return rejected(13);
    if (std::memchr(data.data(), 0, n)) return rejected(14);
    data.resize(n); out = std::move(data); return true;
}
inline bool read_private(const char *path, size_t limit, std::string &out) {
    return read_input(path, limit, out, false);
}
inline bool load(const char *path, std::string &server, std::string &user, std::string &password,
                 bool temporary = false) {
    server.clear(); user.clear(); password.clear();
    import_status = {};
    std::string data;
    if (!read_input(path, 8192, data, temporary)) return false;
    if (data.find("\\u0000") != std::string::npos) return rejected(20);
    cJSON *root = cJSON_ParseWithOpts(data.c_str(), nullptr, 1);
    if (!root) return rejected(21);
    std::string s, u, p;
    bool ok = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 3;
    unsigned fields = 0;
    for (auto *v = root->child; ok && v; v = v->next) {
        if (!v->string || !cJSON_IsString(v) || !v->valuestring || !*v->valuestring) { ok = false; break; }
        unsigned bit = 0;
        if (!std::strcmp(v->string, "server")) { bit = 1; s = v->valuestring; }
        else if (!std::strcmp(v->string, "username")) { bit = 2; u = v->valuestring; }
        else if (!std::strcmp(v->string, "password_file")) { bit = 4; p = v->valuestring; }
        else { ok = false; break; }
        if (fields & bit) ok = false;
        fields |= bit;
    }
    cJSON_Delete(root);
    ok = ok && fields == 7 && s.size() <= 2048 && u.size() <= 256 && p.size() <= 4096 &&
        p[0] == '/' && app::smoke::valid_server(s);
    for (unsigned char c : s + u + p) if (c < 32 || c == 127) ok = false;
    std::string secret;
    if (temporary && p != "/app0/.navi5-dev-password.tmp") return rejected(22);
    if (!ok) return rejected(23);
    if (!read_input(p.c_str(), 1024, secret, temporary)) { import_status.stage += 100; return false; }
    if (!secret.empty() && secret.back() == '\n') secret.pop_back();
    if (!secret.empty() && secret.back() == '\r') secret.pop_back();
    if (secret.empty()) return rejected(24);
    for (unsigned char c : secret) if (c < 32 || c == 127) return rejected(25);
    server = std::move(s); user = std::move(u); password = std::move(secret);
    import_status = {30, 0}; return true;
}
#ifdef NAVI5_DEV_TOOLS
// Startup-only call: never retried during render/login. No persistence or auto-connect.
inline bool import_temporary(std::string &server, std::string &user, std::string &password,
                             bool &cleanup_failed) {
    cleanup_failed = false;
    constexpr const char *config = "/app0/.navi5-dev-login.tmp";
    constexpr const char *secret = "/app0/.navi5-dev-password.tmp";
    if (!load(config, server, user, password, true)) return false;
    const bool removed_secret = unlink(secret) == 0;
    const int secret_error = removed_secret ? 0 : errno;
    const bool removed_config = unlink(config) == 0;
    const int config_error = removed_config ? 0 : errno;
    cleanup_failed = !removed_secret || !removed_config;
    if (cleanup_failed) import_status = {removed_secret ? 32 : 31, removed_secret ? config_error : secret_error};
    return true;
}
#endif
} // namespace app::dev
