/* Navi5 — DEV live numeric syscall observation; no transport policy.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef NAVI5_DEV_TOOLS
#include "socket_trace.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jf {
namespace {
thread_local int live_fd = -1;
thread_local unsigned live_count = 0;
thread_local bool emitting = false;
}
void trace_live(int phase, int op, int cmd, long long arg, long long result, int error) {
    const int saved = errno;
    if (!emitting && socket_trace.active) live_progress.publish(phase, op, cmd, arg, result, error);
    if (!emitting && socket_trace.active && live_count <= 128) {
        emitting = true;
        // ponytail: 128 events/request plus one cap marker; raise only with evidence.
        if (live_count == 128) { phase = 4; op = cmd = arg = result = error = 0; }
        char row[128];
        const int n = std::snprintf(row, sizeof row, "live=%u/%d/%d/%d/%lld/%lld/%d\n",
                                    live_count++, phase, op, cmd, arg, result, error);
#ifdef JELLY5_LOG_HOST
        struct sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(JELLY5_LOG_PORT);
        if (live_fd >= 0 && n > 0 && n < static_cast<int>(sizeof row) &&
            inet_pton(AF_INET, JELLY5_LOG_HOST, &a.sin_addr) == 1)
            // Original libc symbols, NOT renamed curl archive refs. Never EVO log:
            // EVO flushes stdout, takes locks, lazily opens a socket and sends blocking.
            sendto(live_fd, row, n, MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&a), sizeof a);
#else
        (void)n;
#endif
        emitting = false;
    }
    errno = saved;
}
void trace_begin() {
    const int saved = errno;
    if (live_fd >= 0) close(live_fd);
    live_fd = -1;
    socket_trace = {};
    live_count = 0;
    // Open before tracing. No address/credential contents enter trace rows.
#ifdef JELLY5_LOG_HOST
    live_fd = socket(AF_INET, SOCK_DGRAM, 0);
#endif
    socket_trace.active = true;
    trace_live(0, 0, 0, 0, live_fd >= 0 ? 0 : -1, live_fd >= 0 ? 0 : errno);
    errno = saved;
}
void trace_end() {
    const int saved = errno;
    trace_live(3, 0, 0, 0, 0, 0);
    socket_trace.active = false;
    if (live_fd >= 0) close(live_fd);
    live_fd = -1;
    errno = saved;
}
}
namespace {
int observed(int op, int cmd, int result) {
    const int saved = errno;
    jf::trace_live(2, op, cmd, 0, result, saved);
    jf::socket_trace.record(op, cmd, result, saved);
    errno = saved;
    return result;
}
}
extern "C" {
// Only audited libcurl archive references are renamed to these symbols.
int navi5_trace_socket(int domain, int type, int protocol) {
    jf::trace_live(1, 1, type, protocol, 0, 0);
    return observed(1, type, socket(domain, type, protocol));
}
int navi5_trace_setsockopt(int fd, int level, int option, const void *value, socklen_t len) {
    jf::trace_live(1, 3, option, level, 0, 0);
    return observed(3, option, setsockopt(fd, level, option, value, len));
}
int navi5_trace_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    jf::trace_live(1, 4, 0, 0, 0, 0);
    return observed(4, 0, connect(fd, addr, len));
}
int navi5_trace_getsockopt(int fd, int level, int option, void *value, socklen_t *len) {
    jf::trace_live(1, 5, option, level, 0, 0);
    const int result = getsockopt(fd, level, option, value, len);
    const int saved = errno;
    // Exit arg carries SO_ERROR only after successful, full-width output.
    const int error_value = result == 0 && level == SOL_SOCKET && option == SO_ERROR &&
        value && len && *len >= sizeof(int) ? *static_cast<int *>(value) : -1;
    jf::trace_live(2, 5, option, error_value, result, saved);
    jf::socket_trace.record(5, option, result, saved);
    errno = saved;
    return result;
}
// Preserve ssize_t/size_t ABI; never inspect buffers, addresses or descriptors.
ssize_t navi5_trace_send(int fd, const void *buf, size_t n, int flags) {
    jf::trace_live(1, 9, flags, n, 0, 0);
    const ssize_t r = send(fd, buf, n, flags); const int e = errno;
    jf::trace_live(2, 9, flags, n, r, e); errno = e; return r;
}
ssize_t navi5_trace_recv(int fd, void *buf, size_t n, int flags) {
    jf::trace_live(1, 10, flags, n, 0, 0);
    const ssize_t r = recv(fd, buf, n, flags); const int e = errno;
    jf::trace_live(2, 10, flags, n, r, e); errno = e; return r;
}
ssize_t navi5_trace_read(int fd, void *buf, size_t n) {
    jf::trace_live(1, 11, 0, n, 0, 0);
    const ssize_t r = read(fd, buf, n); const int e = errno;
    jf::trace_live(2, 11, 0, n, r, e); errno = e; return r;
}
ssize_t navi5_trace_write(int fd, const void *buf, size_t n) {
    jf::trace_live(1, 12, 0, n, 0, 0);
    const ssize_t r = write(fd, buf, n); const int e = errno;
    jf::trace_live(2, 12, 0, n, r, e); errno = e; return r;
}
int navi5_trace_poll(struct pollfd *fds, nfds_t count, int timeout) {
    jf::trace_live(1, 6, 0, timeout, 0, 0);
    return observed(6, 0, poll(fds, count, timeout));
}
}
#endif
