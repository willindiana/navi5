/* Navi5 — temporary pairing protocol core.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <map>
#include <curl/curl.h>

namespace pairing {
namespace {
bool server_ok(const std::string &s)
{
    if (s.empty() || s.size() > 1024 || s.find_first_of("\r\n\t ") != std::string::npos ||
        s.find('\0') != std::string::npos) return false;
    CURLU *u = curl_url();
    if (!u) return false;
    bool ok = curl_url_set(u, CURLUPART_URL, s.c_str(), 0) == CURLUE_OK;
    char *scheme = nullptr, *host = nullptr, *extra = nullptr;
    ok = ok && curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
         (std::string(scheme) == "https" || std::string(scheme) == "http") &&
         curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && host[0];
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT}) {
        if (curl_url_get(u, part, &extra, 0) == CURLUE_OK) ok = false;
        curl_free(extra); extra = nullptr;
    }
    curl_free(scheme); curl_free(host); curl_url_cleanup(u);
    return ok;
}
std::string response(int status, const std::string &body)
{
    // same-origin, not no-referrer: Fetch serializes a form POST's Origin as
    // "null" under no-referrer, which the strict Origin check must refuse.
    return "HTTP/1.1 " + std::to_string(status) + " Result\r\nConnection: close\r\n"
        "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
        "Referrer-Policy: same-origin\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
std::string escape(const std::string &s)
{
    std::string out;
    for (char c : s) switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
    }
    return out;
}
// Dark UI, sage accent from app/assets/branding/icon.svg; inline CSS only, no scripts.
std::string page(int status, const std::string &content)
{
    return response(status, "<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><meta name=color-scheme content=dark>"
        "<title>Navi5 pairing</title><style>"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
        "padding:20px;background:#07070a;color:#f5f5f7;font:17px/1.45 -apple-system,system-ui,sans-serif}"
        "main{width:100%;max-width:440px;background:#15181a;border:1px solid #29483b;border-radius:20px;padding:28px 22px}"
        "h1{margin:0 0 4px;font-size:30px;color:#b6c8be}h2{margin:0 0 16px;font-size:19px;font-weight:600}"
        "label{display:block;margin:14px 0 6px;font-weight:600;font-size:15px}"
        "input[type=url],input[type=text],input[type=password]{display:block;width:100%;min-height:50px;padding:12px 14px;"
        "font-size:17px;color:#f5f5f7;background:#07070a;border:1px solid #344e43;border-radius:12px}"
        "input:focus{outline:2px solid #758f80;border-color:#758f80}"
        ".check{display:flex;gap:10px;align-items:flex-start;font-weight:400}.check input{width:22px;height:22px;margin:2px 0 0;accent-color:#758f80}"
        "button{display:block;width:100%;min-height:52px;margin-top:22px;font-size:18px;font-weight:700;color:#07070a;"
        "background:#b6c8be;border:0;border-radius:12px}"
        ".hint,.note{color:rgba(235,235,245,.6);font-size:13px}.note{margin:0 0 6px}"
        ".err{margin:0 0 14px;padding:12px 14px;border-radius:12px;background:#3a1d1d;color:#ffc9c2;border:1px solid #7a3b35}"
        ".ok{color:#b6c8be}</style><main><h1>Navi5</h1>" + content + "</main></html>");
}
std::string message(int status, const std::string &heading, const std::string &text)
{
    return page(status, "<h2>" + heading + "</h2><p>" + text + "</p>");
}
// Fixed viewer-safe text, modelled on smoke::login_failure_key; never server text.
const char *reason(subsonic::LoginFailure failure)
{
    using F = subsonic::LoginFailure;
    switch (failure) {
    case F::Authentication: return "Wrong username or password.";
    case F::Http401: return "HTTP 401: the server or a proxy refused sign-in. Check your account and proxy.";
    case F::Http403: return "HTTP 403: access denied by the server or a proxy. Check account access and proxy.";
    case F::Connection: return "Can't reach the server. Check the address, port and network.";
    case F::Timeout: return "The server took too long to answer. Check the port and network.";
    case F::CaBundle: case F::Untrusted: case F::Tls:
        return "Secure connection failed (TLS/certificate). Check the HTTPS address and certificate.";
    case F::Redirect: return "The server redirected. Enter its final address.";
    case F::Http: case F::Response: return "This doesn't look like a Subsonic server. Check the address and path.";
    case F::ApiKeyUnsupported: return "This server doesn't support API key sign-in. Use username and password.";
    case F::Configuration: return "Check the server address, then enter an API key or username and password.";
    default: return "Sign-in failed. Check the details and try again.";
    }
}
int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decode(const std::string &s, std::string &out)
{
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == '%') {
            if (i + 2 >= s.size() || hex(s[i+1]) < 0 || hex(s[i+2]) < 0) return false;
            c = hex(s[i+1]) * 16 + hex(s[i+2]); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) return false;
        out += static_cast<char>(c);
    }
    return true;
}
bool form(const std::string &s, std::map<std::string, std::string> &out)
{
    size_t pos = 0;
    while (pos < s.size()) {
        auto end = s.find('&', pos);
        if (end == std::string::npos) end = s.size();
        auto eq = s.find('=', pos);
        if (eq == std::string::npos || eq >= end) return false;
        std::string key, value;
        if (!decode(s.substr(pos, eq-pos), key) || !decode(s.substr(eq+1, end-eq-1), value) ||
            !out.emplace(key, value).second) return false;
        pos = end + 1;
    }
    return !s.empty() && s.back() != '&';
}
} // namespace
bool validate(const Credentials &c, const subsonic::HttpGet &http, subsonic::LoginFailure *failure)
{
    subsonic::LoginFailure ignored;
    if (!failure) failure = &ignored;
    *failure = subsonic::LoginFailure::Configuration;
    if (!http || !server_ok(c.server) || c.username.size() > 256 || c.password.size() > 1024 ||
        c.api_key.size() > 1024 || c.username.find('\0') != std::string::npos ||
        c.password.find('\0') != std::string::npos || c.api_key.find('\0') != std::string::npos ||
        (c.api_key.empty() && (c.username.empty() || c.password.empty()))) return false;
    subsonic::Client client(c.server, c.username, c.api_key.empty() ? c.password : "", http);
    if (!c.api_key.empty()) client.set_api_key(c.api_key);
    subsonic::ServerInfo info;
    std::string error; // Untrusted server text must never reach UI/logs.
    if (!client.ping(&info, &error, failure)) return false;
    if (!c.api_key.empty()) {
        std::vector<subsonic::Extension> extensions;
        if (!client.get_extensions(&extensions, &error, failure)) return false;
        for (const auto &e : extensions)
            if (e.name == "apiKeyAuthentication")
                for (int version : e.versions) if (version == 1) return true;
        *failure = subsonic::LoginFailure::ApiKeyUnsupported;
        return false;
    }
    return true;
}
bool Session::live()
{
    if (active_ && Clock::now() >= deadline_) { active_ = false; nonce_.clear(); }
    return active_;
}
bool Session::start(const std::string &address, unsigned port, bool allow_lan,
                    std::chrono::seconds ttl, Diagnostic *diagnostic)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto fail = [&](Stage stage, int error = 0) {
        if (diagnostic) diagnostic->record(stage, error);
        return false;
    };
    if (live() || validating_ || accepted_ || !port || port > 65535 || ttl.count() <= 0 || ttl.count() > 300) return fail(Stage::Arguments);
    in_addr ip{};
    if (inet_pton(AF_INET, address.c_str(), &ip) != 1) return fail(Stage::AddressParse);
    uint32_t n = ntohl(ip.s_addr);
    bool loopback = (n >> 24) == 127;
    bool lan = (n >> 24) == 10 || (n >> 20) == 0xac1 || (n >> 16) == 0xc0a8;
    if (!loopback && !(allow_lan && lan)) return fail(Stage::AddressPolicy);
    unsigned char bytes[32];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return fail(Stage::EntropyOpen, errno);
    size_t used = 0;
    int read_error = 0;
    while (used < sizeof bytes) {
        ssize_t got = read(fd, bytes + used, sizeof bytes - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { read_error = got < 0 ? errno : 0; break; }
        used += static_cast<size_t>(got);
    }
    close(fd);
    if (used != sizeof bytes) return fail(Stage::EntropyRead, read_error); // No weak random fallback.
    nonce_.clear();
    for (unsigned char b : bytes) { nonce_ += "0123456789abcdef"[b >> 4]; nonce_ += "0123456789abcdef"[b & 15]; }
    authority_ = address + ":" + std::to_string(port);
    deadline_ = Clock::now() + ttl;
    ++generation_; active_ = true; failures_ = 0;
    return true;
}
void Session::cancel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_; active_ = false; nonce_.clear(); accepted_.reset();
}
bool Session::active() { std::lock_guard<std::mutex> lock(mutex_); return live(); }
std::optional<Session::Ready> Session::ready()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!live()) return std::nullopt;
    return Ready{"http://" + authority_ + "/pair/" + nonce_, deadline_};
}
std::string Session::url()
{
    auto snapshot = ready();
    return snapshot ? std::move(snapshot->url) : "";
}
std::optional<Credentials> Session::take()
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = std::move(accepted_); accepted_.reset(); return result;
}
namespace {
const char *expired_heading = "Code expired — use the new QR code on the TV";
std::string expired() { return message(410, expired_heading, "This pairing code is no longer valid. If the TV shows no code, press Cross on the TV."); }
std::string bad(int status)
{
    return message(status, "Request not accepted", "Scan the QR code on the TV and try again.");
}
// Re-fill only server and username; never password or API key.
std::string form_page(int status, const std::string &path, const std::string &nonce,
                      const std::string &error = {}, const std::string &server = {}, const std::string &username = {})
{
    return page(status, "<h2>Sign in to your music server</h2>" +
        (error.empty() ? std::string() : "<p class=err role=alert>" + error + "</p>") +
        "<p class=note>Trusted LAN only. HTTP sends secrets unencrypted. "
        "Nonce prevents unsolicited use, not network sniffing. Successful password login is saved unencrypted on the console; API keys stay in memory only.</p>"
        "<p class=note>HTTP music servers send login and music unencrypted. Use only a trusted LAN. "
        "HTTPS verifies certificates; Navi5 never downgrades HTTPS to HTTP.</p>"
        "<form method=post action='" + path + "'><input type=hidden name=csrf value='" + nonce + "'>"
        "<label for=server>HTTP or HTTPS server</label><input id=server name=server type=url inputmode=url autocomplete=url "
        "autocapitalize=off spellcheck=false required maxlength=1024 placeholder='https://music.example.com' value='" + escape(server) + "'>"
        "<label for=username>Username</label><input id=username name=username type=text autocomplete=username "
        "autocapitalize=off spellcheck=false maxlength=256 value='" + escape(username) + "'>"
        "<label for=password>Password</label><input id=password name=password type=password autocomplete=current-password maxlength=1024>"
        "<label for=api_key>API key (preferred)</label><input id=api_key name=api_key type=password autocomplete=off maxlength=1024>"
        "<p class=hint>An API key replaces username and password.</p>"
        "<label class=check><input name=consent type=checkbox value=yes required> I accept unencrypted HTTP pairing on this trusted LAN</label>"
        "<button>Sign in</button></form>");
}
} // namespace
std::string reject_request(int status) { return bad(status); }
std::string Session::handle(const std::string &request, const Validator &validator)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!live()) return expired();
    const auto split = request.find("\r\n\r\n");
    if (split == std::string::npos || split + 4 > max_header || request.size() - split - 4 > max_body)
        return bad(413);
    const auto first = request.find("\r\n");
    std::string line = request.substr(0, first);
    std::string path = "/pair/" + nonce_;
    bool post = line == "POST " + path + " HTTP/1.1";
    if (!post && line != "GET " + path + " HTTP/1.1") return bad(400);
    std::map<std::string, std::string> headers;
    size_t pos = first + 2;
    while (pos < split) {
        auto end = request.find("\r\n", pos), colon = request.find(':', pos);
        if (end == std::string::npos || colon == std::string::npos || colon >= end) return bad(400);
        std::string name = request.substr(pos, colon-pos), value = request.substr(colon+1, end-colon-1);
        for (char &c : name) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (!(c >= 'a' && c <= 'z') && c != '-') return bad(400);
        }
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        for (unsigned char c : value) if (c < 32 || c == 127) return bad(400);
        if (name.empty() || !headers.emplace(name, value).second) return bad(400);
        pos = end + 2;
    }
    if (headers["host"] != authority_ || headers.count("transfer-encoding") ||
        headers.count("expect") || headers.count("upgrade")) return bad(400);
    const std::string origin = "http://" + authority_;
    const auto refused = [] { return message(403, "Request blocked", "Open the page by scanning the QR code on the TV, then try again."); };
    if ((headers.count("origin") && headers["origin"] != origin) ||
        (headers.count("sec-fetch-site") && headers["sec-fetch-site"] != "same-origin" &&
         headers["sec-fetch-site"] != "none")) return refused();
    std::string body = request.substr(split + 4);
    if (!post) {
        if (!body.empty() || (headers.count("content-length") && headers["content-length"] != "0")) return bad(400);
        return form_page(200, path, nonce_);
    }
    if (headers["origin"] != origin) return refused(); // Strict: "null" and missing refused.
    if (headers["content-type"] != "application/x-www-form-urlencoded" ||
        headers["content-length"] != std::to_string(body.size())) return bad(400);
    std::map<std::string, std::string> fields;
    // Exactly csrf, required consent checkbox, server, username, password, api_key.
    // Missing consent or unknown field: 403 before validation, nonce not consumed.
    if (!form(body, fields) || fields.size() < 5 || fields.size() > 6 || fields["csrf"] != nonce_ ||
        !fields.count("server") || !fields.count("username") || !fields.count("password") ||
        !fields.count("api_key") || (fields.size() == 6 && !fields.count("consent"))) return refused();
    if (fields["consent"] != "yes")
        return form_page(403, path, nonce_, "Tick the trusted-LAN box to continue.", fields["server"], fields["username"]);
    if (validating_) return message(409, "Already checking", "A sign-in is being checked. Wait a moment, then reload this page.");
    Credentials credentials{fields["server"], fields["username"], fields["password"], fields["api_key"]};
    if (!credentials.api_key.empty()) credentials.password.clear();
    // Session stays live (no new TTL) while validating so a failure can retry.
    // Cancellation can invalidate an in-flight result without waiting under mutex.
    const auto generation = generation_;
    validating_ = true;
    lock.unlock();
    bool ok = false;
    subsonic::LoginFailure failure = subsonic::LoginFailure::None;
#ifdef __cpp_exceptions
    try {
        ok = validator && validator(credentials, failure);
    } catch (...) {
        // Untrusted callback failure counts as a failed attempt; never expose exception text.
    }
#else
    ok = validator && validator(credentials, failure);
#endif
    lock.lock();
    validating_ = false;
    if (generation != generation_ || !live()) return expired();
    if (ok) {
        active_ = false; nonce_.clear();
        accepted_ = std::move(credentials);
        return message(200, "<span class=ok>Signed in</span> — you can close this page", "Navi5 is connecting on the TV.");
    }
    if (++failures_ >= max_failures) {
        active_ = false; nonce_.clear();
        return message(403, "Too many attempts", "Press Cross on the TV to get a new code, then scan it again.");
    }
    return form_page(403, path, nonce_, reason(failure), credentials.server, credentials.username);
}
} // namespace pairing
