/* Navi5 — saved login, plaintext in the title's own storage.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/saved_login.h"
#include "app/server_url.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace app::saved_login {
namespace {

constexpr size_t kMaxFile = 8192, kMaxField = 1024;

bool valid(const Login &l)
{
    return app::smoke::valid_server(l.server) && !l.user.empty() &&
           l.user.size() <= kMaxField && l.password.size() <= kMaxField &&
           l.user.find('\0') == std::string::npos && l.password.find('\0') == std::string::npos;
}

void wipe(void *p, size_t n)
{
    volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
    while (n--) *v++ = 0;
}

bool field(const cJSON *o, const char *k, std::string &out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v)) return false;
    out = v->valuestring;
    return true;
}

// cJSON truncates strings at a decoded "\u0000" without exposing length; reject it.
// Escape-aware so an escaped backslash before "u0000" is not a false match.
bool has_nul_escape(const char *s, size_t n)
{
    for (size_t i = 0; i + 1 < n; ++i)
        if (s[i] == '\\') {
            if (s[i + 1] == 'u' && i + 5 < n && std::memcmp(s + i + 2, "0000", 4) == 0) return true;
            ++i;
        }
    return false;
}

} // namespace

Load load(Login &out, const char *path)
{
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);  // FIFO never blocks open
    if (fd < 0) return errno == ENOENT ? Load::Missing : Load::Invalid;
    struct stat st;
    char buf[kMaxFile + 1];  // +1 detects growth past bound after fstat
    ssize_t n = -1;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && size_t(st.st_size) <= kMaxFile) {
        size_t got = 0;
        while (got < sizeof buf) {
            const ssize_t r = read(fd, buf + got, sizeof buf - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { n = r < 0 ? -1 : ssize_t(got); break; }
            got += size_t(r);
        }
        if (got == sizeof buf) n = -1;
    }
    close(fd);
    if (n <= 0) { wipe(buf, sizeof buf); return Load::Invalid; }
    const char *end = nullptr;
    cJSON *j = cJSON_ParseWithLengthOpts(buf, size_t(n), &end, 0);
    while (j && end < buf + n && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) ++end;
    const bool clean = j && end == buf + n && !has_nul_escape(buf, size_t(n)) &&
                       !std::memchr(buf, 0, size_t(n));  // raw NUL truncates valuestring
    wipe(buf, sizeof buf);
    Login l;
    const bool ok = clean && cJSON_IsObject(j) && cJSON_GetArraySize(j) == 3 &&
                    field(j, "server", l.server) && field(j, "user", l.user) &&
                    field(j, "password", l.password) && valid(l);
    cJSON_Delete(j);  // ponytail: cJSON's parsed copies are freed unwiped; plaintext file is the accepted ceiling.
    if (!ok) return Load::Invalid;
    out = std::move(l);
    return Load::Ok;
}

bool save(const Login &login, const char *path)
{
    if (!valid(login)) return false;
    const std::string target(path), tmp = target + ".tmp";
    const auto slash = target.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        const std::string dir = target.substr(0, slash);
        // ponytail: pre-existing dir keeps its mode (older builds used 0777); file 0600 is the boundary.
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "server", login.server.c_str());
    cJSON_AddStringToObject(j, "user", login.user.c_str());
    cJSON_AddStringToObject(j, "password", login.password.c_str());
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text) return false;
    const size_t len = std::strlen(text);
    bool ok = len <= kMaxFile;
    unlink(tmp.c_str());  // stale temp from a crash; O_EXCL below refuses any leftover/symlink
    const int fd = ok ? open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600) : -1;
    if (fd >= 0) {
        size_t put = 0;
        while (put < len) {
            const ssize_t w = write(fd, text + put, len - put);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) break;
            put += size_t(w);
        }
        ok = put == len && fsync(fd) == 0;  // O_CREAT 0600: umask can only narrow it
        ok = close(fd) == 0 && ok;
        ok = ok && std::rename(tmp.c_str(), path) == 0;
        if (!ok) unlink(tmp.c_str());
    } else {
        ok = false;
    }
    wipe(text, len);
    std::free(text);
    return ok;
}

bool forget(const char *path)
{
    const std::string tmp = std::string(path) + ".tmp";
    const bool tmp_ok = unlink(tmp.c_str()) == 0 || errno == ENOENT;  // crash leftover holds plaintext too
    return (unlink(path) == 0 || errno == ENOENT) && tmp_ok;
}

} // namespace app::saved_login
