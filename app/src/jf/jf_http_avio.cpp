/*
 * Navi5 — generic credential-bearing HTTP transport (legacy filename).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Use curl, not FFmpeg: FFmpeg 7.1 OpenSSL does not verify hostnames and
 * its HTTP protocol cannot disable automatic redirects.
 */
#include "jf_http.h"
#include "curl_diag.h"
#include "../curl_sockopt.h"

#include <curl/curl.h>
#include <strings.h>
#include <cstdlib>
#include <cstring>

// Provision separately; never fall back to an absent PS5 system trust store.
#ifndef NAVI5_CA_BUNDLE
#define NAVI5_CA_BUNDLE "/data/navi5/ca-bundle.pem"
#endif

namespace jf {
namespace {
constexpr size_t kMaxBody = 16u << 20;

size_t on_body(char *p, size_t size, size_t n, void *ud)
{
    auto &body = static_cast<HttpResponse *>(ud)->body;
    if (size && n > (kMaxBody - body.size()) / size)
        return 0;
    body.append(p, size * n);
    return size * n;
}

size_t on_header(char *p, size_t size, size_t n, void *ud)
{
    static const char kSetCookie[] = "Set-Cookie:";
    const size_t len = size * n, k = sizeof kSetCookie - 1;
    if (len > k && strncasecmp(p, kSetCookie, k) == 0) {
        std::string v(p + k, len - k);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' '))
            v.pop_back();
        while (!v.empty() && v.front() == ' ')
            v.erase(0, 1);
        if (!v.empty())
            static_cast<HttpResponse *>(ud)->cookies.push_back(v);
    }
    return len;
}
} // namespace

HttpResponse http_request(const std::string &method, const std::string &url,
                          const std::vector<std::string> &headers, const std::string &body,
                          int timeout_s)
{
    HttpResponse res;
#ifdef NAVI5_DEV_TOOLS
    live_progress.publish(5, 11, 0, 0, 0, 0);
#endif
    curl_diag = {};
    // C++ static initialization serializes curl initialization across callers.
    static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) {
        res.failure = TransportFailure::Initialization;
        res.error = "HTTP initialization failed";
        return res;
    }
    CURL *c = curl_easy_init();
    if (!c) {
        res.failure = TransportFailure::Initialization;
        res.error = "HTTP initialization failed";
        return res;
    }
    curl_slist *hl = nullptr;
    bool configured = true;
    auto header = [&](const char *value) {
        curl_slist *next = curl_slist_append(hl, value);
        if (next) hl = next;
        else configured = false;
    };
    for (const auto &h : headers) {
        if (h.find_first_of("\r\n") != std::string::npos || h.find('\0') != std::string::npos)
            configured = false;
        else header(h.c_str());
    }
    auto option = [&](CURLoption key, auto value) {
        if (curl_easy_setopt(c, key, value) != CURLE_OK) configured = false;
    };
    if (!body.empty()) {
        header("Content-Type: application/json");
        option(CURLOPT_POSTFIELDS, body.data());
        option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    option(CURLOPT_URL, url.c_str());
    option(CURLOPT_CUSTOMREQUEST, method.c_str());
    if (method.empty() || method.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos ||
        url.find('\0') != std::string::npos || timeout_s <= 0)
        configured = false;
    option(CURLOPT_HTTPHEADER, hl);
    option(CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    option(CURLOPT_NOSIGNAL, 1L);
#ifdef EVO_TARGET_PS5
    option(CURLOPT_SOCKOPTFUNCTION, jf::curl_sockopt);
#endif
    option(CURLOPT_PROTOCOLS_STR, "http,https");
    option(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    option(CURLOPT_FOLLOWLOCATION, 0L); // No replay to any redirect destination.
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
    option(CURLOPT_CAINFO, NAVI5_CA_BUNDLE);
    option(CURLOPT_CAPATH, static_cast<const char *>(nullptr));
    option(CURLOPT_PROXY, ""); // Ignore process proxy environment for credentials.
    option(CURLOPT_VERBOSE, 0L); // Never log URLs, headers or server error text.
    option(CURLOPT_WRITEFUNCTION, on_body);
    option(CURLOPT_WRITEDATA, &res);
    option(CURLOPT_HEADERFUNCTION, on_header);
    option(CURLOPT_HEADERDATA, &res);
#ifdef NAVI5_DEV_TOOLS
    trace_begin();
#endif
    const CURLcode rc = configured ? curl_easy_perform(c) : CURLE_BAD_FUNCTION_ARGUMENT;
#ifdef NAVI5_DEV_TOOLS
    trace_live(5, 1, 0, 0, static_cast<int>(rc), 0);
    curl_diag.sockets = socket_trace;
#endif
    curl_diag.code = static_cast<int>(rc);
    auto measure = [&](CURLINFO key, auto &value) {
        auto measured = value;
        const auto info_rc = curl_easy_getinfo(c, key, &measured);
        if (info_rc == CURLE_OK) value = measured;
        return static_cast<int>(info_rc);
    };
    curl_diag.http_info = measure(CURLINFO_RESPONSE_CODE, curl_diag.http);
    curl_diag.os_info = measure(CURLINFO_OS_ERRNO, curl_diag.os);
    curl_diag.lookup_info = measure(CURLINFO_NAMELOOKUP_TIME, curl_diag.lookup);
    curl_diag.connect_info = measure(CURLINFO_CONNECT_TIME, curl_diag.connect);
    curl_diag.total_info = measure(CURLINFO_TOTAL_TIME, curl_diag.total);
#ifdef NAVI5_DEV_TOOLS
    trace_live(5, 2, 0, 0, static_cast<int>(curl_diag.http), 0);
#endif
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        res.status = static_cast<int>(code);
    } else {
        // Fixed enum only; never forward URL-bearing library diagnostics.
        switch (rc) {
        case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_CONNECT: res.failure = TransportFailure::Connection; break;
        case CURLE_OPERATION_TIMEDOUT: res.failure = TransportFailure::Timeout; break;
        case CURLE_SSL_CACERT_BADFILE: res.failure = TransportFailure::CaBundle; break;
        case CURLE_PEER_FAILED_VERIFICATION: res.failure = TransportFailure::Untrusted; break;
        case CURLE_SSL_CONNECT_ERROR: res.failure = TransportFailure::Tls; break;
        case CURLE_BAD_FUNCTION_ARGUMENT: res.failure = TransportFailure::Configuration; break;
        default: res.failure = TransportFailure::Other; break;
        }
        res.error = "HTTP transport failed";
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
#ifdef NAVI5_DEV_TOOLS
    trace_live(5, 3, 0, 0, 0, 0);
#endif
    if (res.status >= 200 && res.status < 300) {
        res.status = 200;
    } else {
        if (res.status >= 300 && res.status < 400) {
            res.failure = TransportFailure::Redirect;
            res.error = "HTTP redirect refused";
        } else if (res.status >= 400) {
            if (res.status >= 500) res.status = 500;
            else if (res.status != 400 && res.status != 401 && res.status != 403 && res.status != 404)
                res.status = 499;
            res.error = "HTTP error " + std::to_string(res.status);
        }
        res.body.clear();
        res.cookies.clear();
    }
    return res;
}
} // namespace jf

/* C bridge for existing off-thread image decoder. No FFmpeg HTTP fallback. */
extern "C" uint8_t *navi5_image_fetch(const char *url, size_t *len)
{
    *len = 0;
    auto response = jf::http_request("GET", url, {}, {}, 15);
    if (response.status != 200 || response.body.empty() || !response.error.empty()) return nullptr;
    auto *data = static_cast<uint8_t *>(std::malloc(response.body.size()));
    if (!data) return nullptr;
    *len = response.body.size();
    std::memcpy(data, response.body.data(), *len);
    return data;
}
