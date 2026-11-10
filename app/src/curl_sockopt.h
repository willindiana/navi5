/* Navi5 — PS5 curl socket nonblocking switch (production, all curl handles).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#ifdef EVO_TARGET_PS5
#include <curl/curl.h>
#include <sys/socket.h>
#ifdef NAVI5_DEV_TOOLS
extern "C" int navi5_trace_setsockopt(int fd, int level, int option, const void *value, socklen_t len);
#endif
namespace jf {
// Hardware: SOCK_STREAM|SOCK_NONBLOCK socket, F_GETFL=6, yet recv blocked >30 s.
// PS5 ignores SOCK_NONBLOCK/O_NONBLOCK for I/O; SO_NBIO is its own switch.
constexpr int kSoNbio = 0x1200;
// CURLOPT_SOCKOPTFUNCTION. Fail closed: curl must never use a blocking socket.
inline int curl_sockopt(void *, curl_socket_t fd, curlsocktype) {
    const int one = 1;
#ifdef NAVI5_DEV_TOOLS
    const int r = navi5_trace_setsockopt(fd, SOL_SOCKET, kSoNbio, &one, sizeof one); // op3 cmd 4608
#else
    const int r = setsockopt(fd, SOL_SOCKET, kSoNbio, &one, sizeof one);
#endif
    return r == 0 ? CURL_SOCKOPT_OK : CURL_SOCKOPT_ERROR;
}
}
#endif
