/* Navi5 — numeric-only, request-local transport diagnostics.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdio>
#include <string>
#include "../socket_trace.h"

namespace jf {
struct CurlDiag {
    // -1 means not measured. getinfo results are CURLcode values; 0 is success.
    int code = -1;
    int http_info = -1, os_info = -1, lookup_info = -1, connect_info = -1, total_info = -1;
    long http = -1, os = -1;
    double lookup = -1, connect = -1, total = -1;
#ifdef NAVI5_DEV_TOOLS
    SocketTrace sockets;
    int log_status = -1; // -1 not attempted, 0 failed, 1 write+close succeeded
#endif
    std::string line() const {
        char out[320];
        std::snprintf(out, sizeof out,
            "curl=%d http=%ld/%d os=%ld/%d lookup=%.6f/%d connect=%.6f/%d total=%.6f/%d",
            code, http, http_info, os, os_info, lookup, lookup_info,
            connect, connect_info, total, total_info);
#ifdef NAVI5_DEV_TOOLS
        return std::string(out) + " " + sockets.summary() + " log=" + std::to_string(log_status);
#else
        return out;
#endif
    }
};
// Only worker owning request reads this; copied through existing result mailbox.
inline thread_local CurlDiag curl_diag;
// build.sh injects `git describe --always --dirty`; hand-edited labels went stale.
#ifndef NAVI5_BUILD_ID
#define NAVI5_BUILD_ID "unknown"
#endif
#ifdef NAVI5_DEV_TOOLS
constexpr const char *kDiagBuild = NAVI5_BUILD_ID " / dev";
#else
constexpr const char *kDiagBuild = NAVI5_BUILD_ID;
#endif
}
