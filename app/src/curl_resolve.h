/* Navi5 — resolve server names before curl sees them (production, all curl handles).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <string>

namespace jf {
// Hardware, 2026-11-18: every request to a host name failed at once with
// CURLE_FAILED_INIT (lookup time 0) while IP addresses connected. curl 8.18's
// threaded resolver could not start in the app process (it creates a wakeup
// pipe and a thread per lookup), so a name never reached DNS. Resolve it here
// with getaddrinfo instead, which on PS5 is the console's own resolver
// (toolkit/tools/native-app/stubs/evo_dns.c), and hand the addresses to curl as
// a CURLOPT_RESOLVE entry. curl finds the name in its DNS cache and never starts
// a resolver; SNI and certificate checks still use the host name in the URL.
//
// Returns the list to keep until the transfer ends and then free with
// curl_slist_free_all (nullptr when the URL names an IP address and nothing is
// pinned). Returns CURLE_COULDNT_RESOLVE_HOST when the name does not resolve.
inline CURLcode pin_resolve(CURL *easy, const std::string &url, curl_slist **pinned)
{
    *pinned = nullptr;
    CURLU *u = curl_url();
    if (!u) return CURLE_OUT_OF_MEMORY;
    char *host = nullptr, *port = nullptr;
    CURLcode result = CURLE_OK;
    if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK ||
        curl_url_get(u, CURLUPART_HOST, &host, 0) != CURLUE_OK ||
        curl_url_get(u, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT) != CURLUE_OK || !host || !port) {
        result = CURLE_URL_MALFORMAT;
    } else {
        in_addr literal{};
        // IP literals (IPv6 ones come bracketed) need no lookup; curl takes them as they are.
        if (host[0] != '[' && inet_pton(AF_INET, host, &literal) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET; // The console resolver is IPv4 only.
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *found = nullptr;
            std::string entry = std::string(host) + ":" + port + ":";
            size_t count = 0;
            if (getaddrinfo(host, nullptr, &hints, &found) == 0) {
                for (const addrinfo *a = found; a && count < 8; a = a->ai_next) {
                    if (a->ai_family != AF_INET || !a->ai_addr) continue;
                    char text[INET_ADDRSTRLEN] = {};
                    const auto *in = reinterpret_cast<const sockaddr_in *>(a->ai_addr);
                    if (!inet_ntop(AF_INET, &in->sin_addr, text, sizeof text)) continue;
                    entry += (count++ ? "," : "");
                    entry += text;
                }
                freeaddrinfo(found);
            }
            if (!count) {
                result = CURLE_COULDNT_RESOLVE_HOST;
            } else {
                *pinned = curl_slist_append(nullptr, entry.c_str());
                result = *pinned ? curl_easy_setopt(easy, CURLOPT_RESOLVE, *pinned) : CURLE_OUT_OF_MEMORY;
            }
        }
    }
    curl_free(host);
    curl_free(port);
    curl_url_cleanup(u);
    return result;
}
}
