/* Navi5 — API key login against a real Navidrome; disposable server and account only.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Driven by navidrome_live.py. Uses the app's own transport and client code.
 * Mode "issue": server must issue, accept, describe and revoke a key.
 * Mode "fallback": server without API keys must be left on password auth.
 * Prints fixed text only; never credentials, keys or URLs.
 */
#include "api_key.h"
#include "http_jf.h"

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "cJSON.h"
}

using namespace subsonic;

namespace {
int failures = 0;
void check(bool ok, const char *what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

/* Native-API session for test housekeeping (listing/removing this client's players). */
struct Admin {
    std::string server;
    std::vector<std::string> auth;
    bool login(const std::string &user, const std::string &password)
    {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "username", user.c_str());
        cJSON_AddStringToObject(j, "password", password.c_str());
        char *body = cJSON_PrintUnformatted(j);
        const HttpResponse r = http_send_jf("POST", server + "/auth/login", {}, body);
        std::free(body); cJSON_Delete(j);
        cJSON *reply = cJSON_Parse(r.body.c_str());
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(reply, "token");
        const bool ok = r.ok() && cJSON_IsString(token);
        if (ok) auth = {std::string("X-ND-Authorization: Bearer ") + token->valuestring};
        cJSON_Delete(reply);
        return ok;
    }
    /* IDs of players carrying the app's issued-key name. */
    std::vector<std::string> players() const
    {
        std::vector<std::string> ids;
        const HttpResponse r = http_send_jf("GET", server + "/api/player", auth, "");
        cJSON *list = cJSON_Parse(r.body.c_str());
        const cJSON *p = nullptr;
        cJSON_ArrayForEach(p, list) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(p, "id");
            if (cJSON_IsString(name) && cJSON_IsString(id) && std::string(name->valuestring) == kApiKeyPlayerName)
                ids.push_back(id->valuestring);
        }
        cJSON_Delete(list);
        return ids;
    }
    bool remove(const std::string &id) const
    {
        return http_send_jf("DELETE", server + "/api/player/" + id, auth, "").ok();
    }
};
} // namespace

int main(int argc, char **argv)
{
    const char *url = std::getenv("NAVI5_LIVE_URL"), *user = std::getenv("NAVI5_LIVE_USER"),
               *password = std::getenv("NAVI5_LIVE_PASSWORD");
    if (argc != 2 || !url || !user || !password) { std::puts("usage: env NAVI5_LIVE_{URL,USER,PASSWORD} api_key_live issue|fallback"); return 2; }
    const std::string mode = argv[1], server = url;
    ServerInfo info; std::string error; LoginFailure failure{};
    Client by_password(server, user, password, http_get_jf);
    check(by_password.ping(&info, &error, &failure), "password ping");
    std::printf("  server: %s %s\n", info.type.c_str(), info.server_version.c_str());
    std::vector<Extension> extensions;
    check(by_password.get_extensions(&extensions, &error), "extension list");
    Admin admin{server, {}};
    check(admin.login(user, password), "native login for housekeeping");
    for (const auto &id : admin.players()) admin.remove(id); // Leftovers from an aborted run.

    if (mode == "fallback") {
        check(!can_issue_api_key(info, extensions), "server does not offer key issuance");
        // The app never issues here; if it did, nothing usable or lasting may result.
        std::string key = "stale";
        check(issue_api_key(server, user, password, http_get_jf, http_send_jf, &key) != IssueResult::Ok && key.empty(),
              "forced issuance fails closed");
        check(admin.players().empty(), "no issued player left behind");
        check(by_password.ping(&info, &error), "password still signs in");
    } else if (mode == "issue") {
        check(can_issue_api_key(info, extensions), "server offers key issuance");
        std::string key;
        check(issue_api_key(server, user, password, http_get_jf, http_send_jf, &key) == IssueResult::Ok, "key issued");
        check(key.size() == 26 && key.compare(0, 4, "nds_") == 0, "key has Navidrome's format");
        check(admin.players().size() == 1, "exactly one issued player on the server");
        Client by_key(server, "", "", http_get_jf);
        by_key.set_api_key(key);
        check(by_key.ping(&info, &error, &failure), "key alone signs in");
        std::string owner;
        check(by_key.token_info(&owner, &error) && owner == user, "tokenInfo names the account");
        std::vector<Album> albums;
        check(by_key.get_album_list("newest", 5, 0, "", &albums, &error), "library request with the key");
        Client unknown(server, "", "", http_get_jf);
        unknown.set_api_key("nds_0000000000000000000000");
        check(!unknown.ping(&info, &error, &failure) && failure == LoginFailure::ApiKeyRejected, "unknown key is rejected as 44");
        // A key beside a username must be refused by the server, never silently accepted.
        const HttpResponse mixed = http_get_jf(by_key.build_url("ping", std::string("u=") + user));
        check(mixed.body.find("\"code\":43") != std::string::npos, "key plus username is refused as 43");
        for (const auto &id : admin.players()) check(admin.remove(id), "player removed (revocation)");
        check(!by_key.ping(&info, &error, &failure) && failure == LoginFailure::ApiKeyRejected, "revoked key is rejected as 44");
        check(by_password.ping(&info, &error), "password unaffected by revocation");
    } else {
        std::puts("unknown mode"); return 2;
    }
    std::printf("%s: %s\n", mode.c_str(), failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
