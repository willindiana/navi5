/* Navi5 — API key issuance checks; mock transport, fake credentials, no server.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "api_key.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace subsonic;

namespace {
struct Sent { std::string method, url, body; std::vector<std::string> headers; };
const char *kOk = R"({"subsonic-response":{"status":"ok","type":"navidrome","openSubsonic":true}})";
const char *kRefused = R"({"subsonic-response":{"status":"failed","error":{"code":44,"message":"x"}}})";

bool key_shaped(const std::string &k)
{
    if (k.size() != 26 || k.compare(0, 4, "nds_") != 0) return false;
    for (size_t i = 4; i < k.size(); ++i)
        if (!((k[i] >= '0' && k[i] <= '9') || (k[i] >= 'A' && k[i] <= 'Z') || (k[i] >= 'a' && k[i] <= 'z'))) return false;
    return true;
}

struct Server {
    std::vector<Sent> sent;
    std::vector<std::string> gets;
    HttpResponse login{200, R"({"id":"user1","name":"alice","token":"aaa.bbb-ccc_ddd","subsonicSalt":"s"})", {}};
    HttpResponse create{200, R"({"id":"player-1"})", {}};
    const char *ping = kOk;
    HttpSend send() {
        return [this](const std::string &method, const std::string &url, const std::vector<std::string> &headers,
                      const std::string &body) {
            sent.push_back({method, url, body, headers});
            if (url == "https://music.example.test/nd/auth/login") return login;
            if (method == "POST") return create;
            return HttpResponse{200, "{}", {}};
        };
    }
    HttpGet get() {
        return [this](const std::string &url) { gets.push_back(url); return HttpResponse{200, ping, {}}; };
    }
};
} // namespace

int main()
{
    // Format and independence of generated keys.
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string k = generate_api_key();
        assert(key_shaped(k) && seen.insert(k).second);
    }

    // Only Navidrome advertising apiKeyAuthentication v1 can issue.
    ServerInfo nd; nd.type = "navidrome";
    ServerInfo other; other.type = "gonic";
    assert(can_issue_api_key(nd, {{"songLyrics", {1}}, {"apiKeyAuthentication", {1}}}));
    assert(can_issue_api_key(nd, {{"apiKeyAuthentication", {2, 1}}}));
    assert(!can_issue_api_key(nd, {{"apiKeyAuthentication", {2}}}));
    assert(!can_issue_api_key(nd, {{"songLyrics", {1}}}));
    assert(!can_issue_api_key(nd, {}));
    assert(!can_issue_api_key(other, {{"apiKeyAuthentication", {1}}}));

    const std::string password = "p\"w\\ \xc3\xa6";
    std::string key = "stale";
    {   // Success: login, create with the generated key, verify with that key only.
        Server s;
        assert(issue_api_key("https://music.example.test/nd/", "alice", password, s.get(), s.send(), &key) == IssueResult::Ok);
        assert(key_shaped(key) && s.sent.size() == 2 && s.gets.size() == 1);
        assert(s.sent[0].method == "POST" && s.sent[0].headers.empty());
        const std::string creds = "{\"username\":\"alice\",\"password\":\"p\\\"w\\\\ \xc3\xa6\"}";
        assert(s.sent[0].body == creds);
        assert(s.sent[1].method == "POST" && s.sent[1].url == "https://music.example.test/nd/api/player");
        assert(s.sent[1].headers == std::vector<std::string>{"X-ND-Authorization: Bearer aaa.bbb-ccc_ddd"});
        const std::string player = std::string("{\"name\":\"Navi5 (PS5)\",\"apiKey\":\"") + key + "\"}";
        assert(s.sent[1].body == player);
        assert(s.sent[1].body.find("p\\\"w") == std::string::npos); // Password goes to login only.
        const std::string &ping = s.gets[0];
        assert(ping.find("https://music.example.test/nd/rest/ping?apiKey=" + key + "&") == 0);
        assert(ping.find("u=") == std::string::npos && ping.find("&t=") == std::string::npos &&
               ping.find("&s=") == std::string::npos && ping.find("&p=") == std::string::npos);
    }
    {   // Wrong password / old server without the route: nothing created.
        for (HttpResponse login : {HttpResponse{401, R"({"error":"Invalid username or password"})", {}},
                                   HttpResponse{404, "", {}}, HttpResponse{200, "not json", {}},
                                   HttpResponse{200, R"({"token":7})", {}}, HttpResponse{200, R"({"id":"u"})", {}},
                                   HttpResponse{0, {}, "transport", LoginFailure::Connection},
                                   // Token must be header-safe: no injection through a hostile server.
                                   HttpResponse{200, "{\"token\":\"a\\r\\nX-Evil: 1\"}", {}},
                                   HttpResponse{200, R"({"token":"a b"})", {}},
                                   HttpResponse{200, "{\"token\":\"" + std::string(5000, 'a') + "\"}", {}}}) {
            Server s; s.login = login; key = "stale";
            assert(issue_api_key("https://music.example.test/nd", "alice", password, s.get(), s.send(), &key) == IssueResult::Login);
            assert(key.empty() && s.sent.size() == 1 && s.gets.empty());
        }
    }
    {   // Creation refused (e.g. validation, permission): no key, no verify.
        for (HttpResponse create : {HttpResponse{400, R"({"errors":{"apiKey":"ra.validation.unique"}})", {}},
                                    HttpResponse{403, "", {}}, HttpResponse{200, "{}", {}},
                                    HttpResponse{200, R"({"id":"../user/1"})", {}}}) {
            Server s; s.create = create; key = "stale";
            assert(issue_api_key("https://music.example.test/nd", "alice", password, s.get(), s.send(), &key) == IssueResult::Create);
            assert(key.empty() && s.sent.size() == 2 && s.gets.empty());
        }
    }
    {   // Created key does not authenticate: remove the player, report failure.
        Server s; s.ping = kRefused; key = "stale";
        assert(issue_api_key("https://music.example.test/nd", "alice", password, s.get(), s.send(), &key) == IssueResult::Verify);
        assert(key.empty() && s.sent.size() == 3);
        assert(s.sent[2].method == "DELETE" && s.sent[2].url == "https://music.example.test/nd/api/player/player-1");
        assert(s.sent[2].headers == s.sent[1].headers && s.sent[2].body.empty());
    }
    {   // Missing inputs send nothing.
        Server s;
        assert(issue_api_key("https://music.example.test/nd", "", password, s.get(), s.send(), &key) == IssueResult::Login);
        assert(issue_api_key("https://music.example.test/nd", "alice", "", s.get(), s.send(), &key) == IssueResult::Login);
        assert(issue_api_key("https://music.example.test/nd", "alice", password, {}, s.send(), &key) == IssueResult::Login);
        assert(issue_api_key("https://music.example.test/nd", "alice", password, s.get(), {}, &key) == IssueResult::Login);
        assert(s.sent.empty() && s.gets.empty());
    }
    std::puts("api_key ok");
}
