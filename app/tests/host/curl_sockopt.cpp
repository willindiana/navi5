/* Navi5 — release-path SO_NBIO callback: success and fail closed.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <cassert>
#include <initializer_list>
#include <cerrno>
#include <sys/socket.h>
static int fail, calls;
static int mock_setsockopt(int fd, int level, int opt, const void *value, socklen_t len) {
    assert(fd == 7 && level == SOL_SOCKET && opt == 0x1200 && len == sizeof(int) && *static_cast<const int *>(value) == 1);
    ++calls;
    if (fail) { errno = fail; return -1; }
    return 0;
}
#define setsockopt mock_setsockopt
#include "../../src/curl_sockopt.h"
#undef setsockopt
int main() {
    assert(jf::curl_sockopt(nullptr, 7, CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_OK && calls == 1);
    for (int e : {EINVAL, ENOPROTOOPT, EBADF}) {
        fail = e;
        assert(jf::curl_sockopt(nullptr, 7, CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_ERROR);
    }
    assert(calls == 4);
}
