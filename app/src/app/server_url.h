/* Navi5 — server input validation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <string>
namespace app::smoke {
inline bool valid_server(const std::string &s)
{
    size_t start = s.compare(0, 8, "https://") == 0 ? 8 : s.compare(0, 7, "http://") == 0 ? 7 : 0;
    if (!start || s.size() > 2048 || s.size() == start) return false;
    for (unsigned char c : s) if (c <= 32 || c == 127 || c == '@' || c == '?' || c == '#' || c == '\\') return false;
    const auto end = s.find('/', start);
    const auto authority = s.substr(start, end - start);
    if (authority.empty() || authority.front() == ':' || authority.back() == ':') return false;
    const auto colon = authority.rfind(':');
    const auto bracket = authority.find(']');
    if (authority.front() == '[') {
        if (bracket == std::string::npos || bracket == 1 || (bracket + 1 < authority.size() && authority[bracket + 1] != ':')) return false;
    } else if (authority.find_first_of("[]") != std::string::npos || authority.find(':') != colon) return false;
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
        unsigned port = 0;
        for (size_t i = colon + 1; i < authority.size(); ++i) {
            if (authority[i] < '0' || authority[i] > '9') return false;
            port = port * 10 + unsigned(authority[i] - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    for (unsigned char c : authority)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == ':' || c == '[' || c == ']')) return false;
    return true;
}
// Artist-info URLs only. Conservative origin comparison; never attach auth.
inline bool same_origin_portrait(const std::string &server, const std::string &url)
{
    // ui_image.c accepts URLs shorter than its 1024-byte buffer, including query.
    if (url.size() >= 1024 || url.find('#') != std::string::npos) return false;
    for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\') return false;
    if (!valid_server(server) || !valid_server(url.substr(0, url.find('?')))) return false;
    auto origin = [](const std::string &s) {
        const auto start = s.find("://") + 3;
        auto value = s.substr(0, s.find_first_of("/?", start));
        for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const std::string port = value.compare(0, 8, "https://") == 0 ? ":443" : ":80";
        if (value.size() >= port.size() && value.compare(value.size() - port.size(), port.size(), port) == 0)
            value.resize(value.size() - port.size());
        return value;
    };
    return origin(server) == origin(url);
}
}
