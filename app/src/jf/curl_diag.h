/* Navi5 — numeric-only, request-local transport diagnostics.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdio>
#include <string>
#include "../socket_trace.h"
#ifdef NAVI5_DEV_TOOLS
#include <chrono>
#include <mutex>
#endif

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
#ifdef NAVI5_DEV_TOOLS
// DEV: the last few requests of every thread, newest last, numbers and the
// endpoint name only (the last path segment: never host, query or credentials).
// tag names the thread: B browse worker, P playback/login worker, - others (art).
inline thread_local char request_tag = '-';
struct RequestLog {
    struct Row {
        char tag = 0, endpoint[18] = {};
        int code = -1, inflight = 0;
        long http = -1, start = 0, resolve = -1, connect = -1, tls = -1, first = -1, total = -1;
    };
    std::mutex mutex;
    Row rows[6];
    unsigned count = 0;
    const std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    long now_ms() const {
        return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - epoch).count());
    }
    static void endpoint(const std::string &url, char (&out)[18]) {
        const auto query = url.find_first_of("?#");
        const auto path = url.substr(0, query);
        const auto slash = path.rfind('/');
        const auto name = slash == std::string::npos ? std::string{} : path.substr(slash + 1);
        std::snprintf(out, sizeof out, "%s", name.c_str());
    }
    void record(const Row &row) {
        std::lock_guard<std::mutex> lock(mutex);
        if (count == 6) { for (unsigned i = 1; i < 6; ++i) rows[i - 1] = rows[i]; --count; }
        rows[count++] = row;
    }
    // One line per row: tag endpoint @start +resolve c=connect t=tls f=first total, curl/http, inflight.
    std::string lines() {
        std::lock_guard<std::mutex> lock(mutex);
        std::string out;
        for (unsigned i = 0; i < count; ++i) {
            const auto &r = rows[i];
            char line[200];
            std::snprintf(line, sizeof line, "%c %s @%ld res=%ld c=%ld t=%ld f=%ld tot=%ld curl=%d http=%ld in=%d\n",
                          r.tag, r.endpoint, r.start, r.resolve, r.connect, r.tls, r.first, r.total,
                          r.code, r.http, r.inflight);
            out += line;
        }
        return out;
    }
};
inline RequestLog request_log;
#endif
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
