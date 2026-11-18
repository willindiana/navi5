/*
 * Jelly5 — Subsonic transport adapter
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "subsonic.h"
#include "../jf/jf_http.h"

namespace subsonic {

/* Console: jf_http_avio.cpp; host: tests/host/jf_http_curl.cpp.
 * No logging: URL contains credentials. Client receives generic transport
 * errors rather than forwarding diagnostics that might contain the URL. */
inline HttpResponse http_send_jf(const std::string &method, const std::string &url,
                                const std::vector<std::string> &headers, const std::string &body)
{
    const auto r = jf::http_request(method, url, headers, body, 15);
    LoginFailure failure = LoginFailure::None;
    switch (r.failure) {
    case jf::TransportFailure::None: break;
    case jf::TransportFailure::Initialization: failure = LoginFailure::Initialization; break;
    case jf::TransportFailure::Configuration: failure = LoginFailure::Configuration; break;
    case jf::TransportFailure::Connection: failure = LoginFailure::Connection; break;
    case jf::TransportFailure::Timeout: failure = LoginFailure::Timeout; break;
    case jf::TransportFailure::CaBundle: failure = LoginFailure::CaBundle; break;
    case jf::TransportFailure::Untrusted: failure = LoginFailure::Untrusted; break;
    case jf::TransportFailure::Tls: failure = LoginFailure::Tls; break;
    case jf::TransportFailure::Redirect: failure = LoginFailure::Redirect; break;
    case jf::TransportFailure::Other: failure = LoginFailure::Transport; break;
    }
    return {r.status, r.body, r.error.empty() ? "" : "HTTP transport failed", failure};
}

/* A server waking from idle can take longer than one request's timeout:
 * hardware, 2026-11-18, the first request after a quiet spell took 8-15 s
 * behind an HTTPS proxy (a Mac saw 8.3 s, later ones 0.03 s), so the first
 * library page or saved login after launch failed. Read-only requests are tried
 * once more after a timeout; the first attempt wakes the server. */
template <class Get>
HttpResponse retry_after_timeout(const Get &once)
{
    HttpResponse r = once();
    if (r.failure == LoginFailure::Timeout) r = once();
    return r;
}

inline HttpResponse http_get_jf(const std::string &url)
{
    return retry_after_timeout([&] { return http_send_jf("GET", url, {}, ""); });
}

} // namespace subsonic
