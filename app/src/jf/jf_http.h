/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One blocking HTTP request using shared libcurl implementation on console
 * and host. HTTPS verifies certificate and hostname; redirects are refused.
 * See docs/TRANSPORT_SECURITY.md for CA provisioning.
 */
#pragma once

#include <string>
#include <vector>

namespace jf {

enum class TransportFailure { None, Initialization, Configuration, Connection, Timeout, CaBundle, Untrusted, Tls, Redirect, Other };

struct HttpResponse {
    int status = 0;          /* HTTP status; 0 when no response arrived */
    std::string body;
    std::string error;       /* transport error text, if any */
    std::vector<std::string> cookies;   /* Set-Cookie values received ("name=value; Path=/ ...") */
    TransportFailure failure = TransportFailure::None;
    bool ok() const { return status >= 200 && status < 300; }
};

/* headers: "Name: value" lines without line endings. A non-empty body is
 * sent as application/json. */
HttpResponse http_request(const std::string &method, const std::string &url,
                          const std::vector<std::string> &headers, const std::string &body,
                          int timeout_s);

} // namespace jf
