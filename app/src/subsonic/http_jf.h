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
inline HttpResponse http_get_jf(const std::string &url)
{
    const auto r = jf::http_request("GET", url, {}, "", 15);
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

} // namespace subsonic
