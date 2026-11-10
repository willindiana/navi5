/* Navi5 — offline pairing checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing.h"
#include <cassert>
#include <thread>
#include <atomic>

int main()
{
    pairing::Session s;
    assert(!s.start("0.0.0.0", 8000, true));
    assert(!s.start("8.8.8.8", 8000, true));
    assert(!s.start("192.168.0.10", 8000, false));
    assert(!s.start("127.0.0.1", 0, false));
    assert(!s.start("127.0.0.1", 8000, false, std::chrono::seconds(301)));
    auto start = [&] { assert(s.start("127.0.0.1", 8000, false)); };
    assert(!s.ready());
    auto before_start = pairing::Session::Clock::now();
    start(); assert(!s.start("127.0.0.1", 8000, false));
    auto snapshot = s.ready(); assert(snapshot && snapshot->url == s.url());
    assert(snapshot->deadline >= before_start + std::chrono::seconds(300));
    assert(s.ready()->deadline == snapshot->deadline);
    auto request = [&](bool post, std::string host = "127.0.0.1:8000", std::string origin = "http://127.0.0.1:8000") {
        std::string path = s.url().substr(std::string("http://127.0.0.1:8000").size());
        std::string body = "csrf=" + path.substr(6) + "&consent=yes&server=https%3A%2F%2Fexample.invalid&username=fake&password=generated-fake&api_key=";
        return std::string(post ? "POST " : "GET ") + path + " HTTP/1.1\r\nHost: " + host + "\r\nOrigin: " + origin +
            (post ? "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) : "") +
            "\r\n\r\n" + (post ? body : "");
    };
    int calls = 0;
    auto good = [&](const pairing::Credentials &c, subsonic::LoginFailure &) { ++calls; return c.username == "fake" && c.server == "https://example.invalid"; };
    auto status = [&](const std::string &req, int code) { assert(s.handle(req, good).find("HTTP/1.1 " + std::to_string(code) + " ") == 0); };
    status(request(false), 200);
    const auto page = s.handle(request(false), good);
    assert(page.find("HTTP music servers send login and music unencrypted.") < page.find("<form"));
    assert(page.find("HTTP or HTTPS server") != std::string::npos);
    assert(page.find("Successful password login is saved unencrypted on the console; API keys stay in memory only.") < page.find("<form"));
    assert(page.find("Credentials stay in memory only.") == std::string::npos);
    assert(page.find("Referrer-Policy: same-origin\r\n") != std::string::npos);
    assert(page.find("style-src 'unsafe-inline'") != std::string::npos);
    assert(page.find("autocomplete=current-password") != std::string::npos);
    status(request(true, "127.0.0.1:8000", "null"), 403);
    assert(calls == 0 && s.active()); // Fetch's no-referrer POST must still fail closed.
    status(request(true, "evil.invalid"), 400);
    status(request(true, "127.0.0.1:8000", "http://evil.invalid"), 403);
    status(std::string(5000, 'x'), 413);
    auto oversized = request(true) + std::string(5000, 'x'); status(oversized, 413);
    auto valid = request(true);
    auto duplicate = valid; duplicate.insert(duplicate.find("\r\n\r\n"), "\r\nHost: 127.0.0.1:8000"); status(duplicate, 400);
    auto chunked = valid; chunked.insert(chunked.find("\r\n\r\n"), "\r\nTransfer-Encoding: chunked"); status(chunked, 400);
    status(valid + "GET / HTTP/1.1\r\n\r\n", 400);
    auto csrf = valid; csrf[csrf.find("csrf=") + 5] = '!'; status(csrf, 403);
    assert(page.find("name=consent type=checkbox value=yes required") < page.find("<button"));
    auto reform = [&](std::string from, std::string to) {
        auto r = valid; r.replace(r.find(from), from.size(), to);
        auto at = r.find("Content-Length: ") + 16;
        r.replace(at, r.find("\r\n", at) - at, std::to_string(r.size() - r.find("\r\n\r\n") - 4));
        return r;
    };
    status(reform("&consent=yes", ""), 403);                // Checkbox missing.
    status(reform("&consent=yes", "&consent=no"), 403);
    status(reform("&consent=yes", "&consent=yes&extra=1"), 403); // Unknown field.
    status(reform("&consent=yes", "&extra=yes"), 403);
    assert(calls == 0 && s.active()); // Pre-validation rejections leave nonce live.
    status(valid, 200); assert(calls == 1 && !s.active() && s.url().empty() && !s.ready());
    status(valid, 410); assert(calls == 1);
    assert(!s.start("127.0.0.1", 8000, false));
    assert(s.take()->password == "generated-fake"); assert(!s.take());
    // Removing connected-screen reminders must not bypass phone HTTP consent.
    start(); valid = request(true);
    valid = reform("server=https%3A", "server=http%3A");
    status(reform("&consent=yes", ""), 403);
    status(reform("&consent=yes", "&consent=no"), 403);
    assert(calls == 1 && s.active() && !s.take());
    assert(s.handle(valid, [&](const pairing::Credentials &c, subsonic::LoginFailure &) {
        ++calls;
        return c.server == "http://example.invalid";
    }).find("HTTP/1.1 200 ") == 0);
    assert(calls == 2 && !s.active());
    assert(s.take()->server == "http://example.invalid" && !s.take());
    start(); valid = request(true); s.cancel(); status(valid, 410); assert(!s.ready());
    start(); valid = request(true);
    assert(s.handle(valid, [](const pairing::Credentials &, subsonic::LoginFailure &) -> bool { throw 1; }).find("HTTP/1.1 403 ") == 0);
    assert(s.active() && s.ready() && !s.take());
    s.cancel();
    // Throwing validation restored validating_ and cannot strand next session.
    start(); s.cancel();
    assert(s.start("127.0.0.1", 8000, false, std::chrono::seconds(1)));
    valid = request(true); std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    status(valid, 410); assert(!s.active() && !s.ready());
    start(); valid = request(true);
    std::atomic<bool> entered{false}, release{false};
    std::thread worker([&] {
        auto result = s.handle(valid, [&](const pairing::Credentials &, subsonic::LoginFailure &) {
            entered = true;
            while (!release) std::this_thread::yield();
            return true;
        });
        assert(result.find("HTTP/1.1 410 ") == 0);
    });
    while (!entered) std::this_thread::yield();
    assert(!s.start("127.0.0.1", 8000, false));
    assert(s.active() && s.ready());
    assert(s.handle(valid, good).starts_with("HTTP/1.1 409"));
    s.cancel(); release = true; worker.join(); assert(!s.take());
    start(); valid = request(true);
    assert(s.handle(valid, [](const pairing::Credentials &, subsonic::LoginFailure &) { return false; }).find("HTTP/1.1 403 ") == 0);
    assert(s.active()); s.cancel();
    assert(s.start("127.0.0.1", 8000, false, std::chrono::seconds(1)));
    valid = request(true);
    assert(s.handle(valid, [](const pairing::Credentials &, subsonic::LoginFailure &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1050)); return true;
    }).find("HTTP/1.1 410 ") == 0);
    assert(!s.take());
    start(); valid = request(true);
    auto api = valid.find("api_key="); valid.replace(api, 8, "api_key=fake-key");
    auto length = valid.find("Content-Length: ") + 16;
    valid.replace(length, valid.find("\r\n", length) - length,
                  std::to_string(valid.size() - valid.find("\r\n\r\n") - 4));
    assert(s.handle(valid, [](const pairing::Credentials &c, subsonic::LoginFailure &) {
        return c.api_key == "fake-key" && c.password.empty();
    }).find("HTTP/1.1 200 ") == 0);
    assert(s.take()->password.empty());

    // Failed validation retries preserve nonce/deadline; only public fields reappear.
    start(); valid = request(true);
    const auto retry_url = s.url(); const auto retry_deadline = s.ready()->deadline;
    valid = reform("username=fake", "username=%27%3E%3Cscript%3E%26");
    auto failed = [](const pairing::Credentials &, subsonic::LoginFailure &f) {
        f = subsonic::LoginFailure::Authentication; return false;
    };
    for (unsigned i = 1; i <= pairing::Session::max_failures; ++i) {
        const auto error = s.handle(valid, failed);
        assert(error.starts_with("HTTP/1.1 403") && error.find("<main>") != std::string::npos);
        assert(error.find("generated-fake") == std::string::npos);
        if (i < pairing::Session::max_failures) {
            assert(error.find("Wrong username or password.") != std::string::npos);
            assert(error.find("&#39;&gt;&lt;script&gt;&amp;") != std::string::npos);
            assert(s.url() == retry_url && s.ready()->deadline == retry_deadline);
        } else assert(error.find("Press Cross on the TV") != std::string::npos);
    }
    assert(!s.active() && !s.take());
    assert(s.handle(valid, failed).starts_with("HTTP/1.1 410"));
    start(); valid = request(true);
    assert(s.handle(valid, failed).starts_with("HTTP/1.1 403") && s.active());
    status(valid, 200); assert(s.take()); // Same code succeeds after a failed attempt.

    int requests = 0;
    subsonic::HttpGet mock = [&](const std::string &url) {
        ++requests;
        assert(url.find("password") == std::string::npos);
        if (url.find("getOpenSubsonicExtensions") != std::string::npos)
            return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","openSubsonicExtensions":[{"name":"apiKeyAuthentication","versions":[1]}]}})", ""};
        return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok","version":"1.16.1"}})", ""};
    };
    assert(pairing::validate({"https://example.invalid", "fake", "generated-fake", ""}, mock));
    assert(pairing::validate({"https://example.invalid", "", "", "generated-fake-key"}, mock));
    for (auto scheme : {"http://", "https://"}) {
        const std::string server = std::string(scheme) + "example.invalid/base";
        auto same_scheme = [&](const std::string &url) {
            assert(url.compare(0, server.size(), server) == 0);
            return mock(url);
        };
        assert(pairing::validate({server, "fake", "generated-fake", ""}, same_scheme));
        assert(pairing::validate({server, "", "", "generated-fake-key"}, same_scheme));
        const int before = requests;
        for (auto suffix : {"user@example.invalid", "user:pass@example.invalid", "example.invalid/?p=secret", "example.invalid/#secret", ""})
            assert(!pairing::validate({std::string(scheme) + suffix, "fake", "generated-fake", ""}, mock));
        assert(requests == before);
    }
    assert(!pairing::validate({"ftp://example.invalid", "fake", "generated-fake", ""}, mock));
    int failed_requests = 0;
    assert(!pairing::validate({"https://example.invalid", "fake", "generated-fake", ""},
        [&](const std::string &url) {
            ++failed_requests;
            assert(url.compare(0, 8, "https://") == 0);
            return subsonic::HttpResponse{0, "", "TLS failure"};
        }));
    assert(failed_requests == 1); // Never retry failed HTTPS as HTTP.
    assert(!pairing::validate({"https://example.invalid", "", "", "generated-fake-key"},
        [](const std::string &) { return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", ""}; }));
    using F = subsonic::LoginFailure;
    for (auto f : {F::Connection, F::Timeout, F::Untrusted, F::Tls, F::Redirect}) {
        F reported = F::None;
        assert(!pairing::validate({"https://example.invalid", "fake", "fake", ""},
            [f](const std::string &) { return subsonic::HttpResponse{0, "", "SECRET", f}; }, &reported));
        assert(reported == f);
        start(); valid = request(true);
        auto result = s.handle(valid, [f](const auto &, F &out) { out = f; return false; });
        assert(result.find("SECRET") == std::string::npos && result.find("<form") != std::string::npos);
        s.cancel();
    }
    for (auto f : {F::Http401, F::Http403, F::Timeout, F::Redirect, F::Response, F::ApiKeyUnsupported}) {
        F reported = F::None;
        auto extensions_fail = [f](const std::string &url) {
            if (url.find("getOpenSubsonicExtensions") == std::string::npos)
                return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", ""};
            if (f == F::Http401 || f == F::Http403)
                return subsonic::HttpResponse{f == F::Http401 ? 401 : 403, "SECRET", ""};
            if (f == F::Response) return subsonic::HttpResponse{200, "SECRET", ""};
            if (f == F::ApiKeyUnsupported) return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", ""};
            return subsonic::HttpResponse{0, "", "SECRET", f};
        };
        assert(!pairing::validate({"https://example.invalid", "", "", "fake-key"}, extensions_fail, &reported));
        assert(reported == f);
    }
    for (auto versions : {"[1.9]", "[1]"}) {
        auto mock_versions = [&](const std::string &url) {
            if (url.find("getOpenSubsonicExtensions") == std::string::npos)
                return subsonic::HttpResponse{200, R"({"subsonic-response":{"status":"ok"}})", ""};
            return subsonic::HttpResponse{200, std::string(R"({"subsonic-response":{"status":"ok","openSubsonicExtensions":[{"name":"apiKeyAuthentication","versions":)") +
                versions + "}]}}", ""};
        };
        assert(pairing::validate({"https://example.invalid", "", "", "generated-fake-key"}, mock_versions) ==
               (std::string(versions) == "[1]"));
    }
}
