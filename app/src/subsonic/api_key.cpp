/* Navi5 — issue a revocable Navidrome API key from a password login.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "api_key.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace subsonic {
namespace {

constexpr size_t kKeyChars = 22, kMaxBody = 64 * 1024, kMaxToken = 4096, kMaxId = 128;

void wipe(std::string &s)
{
    volatile char *p = s.empty() ? nullptr : &s[0];
    for (size_t n = s.size(); n--;) *p++ = 0;
    s.clear();
}

std::string base(std::string server)
{
    while (!server.empty() && server.back() == '/') server.pop_back();
    return server;
}

bool usable(const HttpResponse &r)
{
    return r.ok() && r.failure == LoginFailure::None && r.error.empty() && r.body.size() <= kMaxBody;
}

/* One string member of a JSON object, restricted to `allowed` characters so it
 * can go into a header or path unescaped. Empty when absent or unsafe. */
std::string member(const std::string &body, const char *key, const char *allowed, size_t max)
{
    std::string out;
    cJSON *j = cJSON_ParseWithLength(body.data(), body.size());
    const cJSON *v = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, key) : nullptr;
    if (cJSON_IsString(v) && v->valuestring) out = v->valuestring;
    cJSON_Delete(j);
    if (out.size() > max || out.find_first_not_of(allowed) != std::string::npos) out.clear();
    return out;
}

std::string object(const char *k1, const std::string &v1, const char *k2, const std::string &v2)
{
    std::string out;
    cJSON *j = cJSON_CreateObject();
    if (j && cJSON_AddStringToObject(j, k1, v1.c_str()) && cJSON_AddStringToObject(j, k2, v2.c_str())) {
        if (char *text = cJSON_PrintUnformatted(j)) {
            out = text;
            for (volatile char *p = text; *p; ++p) *p = 0;
            std::free(text);
        }
    }
    cJSON_Delete(j);
    return out;
}

constexpr const char *kAlnum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

} // namespace

bool can_issue_api_key(const ServerInfo &info, const std::vector<Extension> &extensions)
{
    if (info.type != "navidrome") return false;
    for (const auto &e : extensions)
        if (e.name == "apiKeyAuthentication")
            for (int version : e.versions) if (version == 1) return true;
    return false;
}

std::string generate_api_key()
{
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return {};
    std::string key = "nds_";
    while (key.size() < 4 + kKeyChars) {
        unsigned char b;
        const ssize_t got = read(fd, &b, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got != 1) { wipe(key); break; }
        if (b < 248) key += kAlnum[b % 62]; // 248 = 4 * 62: no modulo bias.
    }
    close(fd);
    return key;
}

IssueResult issue_api_key(const std::string &server, const std::string &username,
                          const std::string &password, const HttpGet &get, const HttpSend &send,
                          std::string *api_key)
{
    api_key->clear();
    if (!get || !send || username.empty() || password.empty()) return IssueResult::Login;
    std::string key = generate_api_key();
    if (key.empty()) return IssueResult::Entropy;

    const std::string root = base(server);
    std::string body = object("username", username, "password", password);
    if (body.empty()) return IssueResult::Login;
    HttpResponse r = send("POST", root + "/auth/login", {}, body);
    wipe(body);
    std::string token = usable(r) ? member(r.body, "token", "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_.", kMaxToken) : "";
    wipe(r.body);
    if (token.empty()) return IssueResult::Login;
    const std::vector<std::string> auth = {"X-ND-Authorization: Bearer " + token};
    wipe(token);

    body = object("name", kApiKeyPlayerName, "apiKey", key);
    r = send("POST", root + "/api/player", auth, body);
    wipe(body);
    const std::string player = usable(r) ? member(r.body, "id", "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_", kMaxId) : "";
    if (player.empty()) { wipe(key); return IssueResult::Create; }

    Client client(root, username, "", get);
    client.set_api_key(key);
    ServerInfo info;
    std::string error;
    if (!client.ping(&info, &error)) {
        send("DELETE", root + "/api/player/" + player, auth, ""); // Best effort; unused key either way.
        wipe(key);
        return IssueResult::Verify;
    }
    *api_key = std::move(key);
    return IssueResult::Ok;
}

} // namespace subsonic
