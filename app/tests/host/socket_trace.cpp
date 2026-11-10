/* Navi5 — offline pinned curl ABI and exact CLOEXEC fallback checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define NAVI5_DEV_TOOLS 1
#define EVO_TARGET_PS5 1
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
static int last_fd, last_cmd, last_int, calls, probes;
static int real_result = -1, real_error = EINVAL;
static struct flock *last_lock;
extern "C" int mock_fcntl(int fd, int cmd, ...) {
    ++calls; last_fd = fd; last_cmd = cmd;
    va_list ap; va_start(ap, cmd);
    if (cmd == F_SETFL || cmd == F_SETFD) last_int = va_arg(ap, int);
    else if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW) last_lock = va_arg(ap, struct flock *);
    else assert(cmd == F_GETFD || cmd == F_GETFL || cmd == F_GETOWN);
    va_end(ap); errno = real_error; return real_result;
}
static int mock_getsockopt(int fd, int level, int opt, void *value, socklen_t *len) {
    ++probes;
    assert(level == SOL_SOCKET && opt == SO_TYPE && *len == sizeof(int));
    if (fd != 7) { errno = fd == -1 ? EBADF : ENOTSOCK; return -1; }
    *static_cast<int *>(value) = SOCK_STREAM;
    errno = EIO; // probe must not replace original errno, even on success
    return 0;
}
#define fcntl mock_fcntl
#define getsockopt mock_getsockopt
#include "../../src/curl_fcntl.cpp"
#undef fcntl
#undef getsockopt
#include <arpa/inet.h>
#include <unistd.h>
#define JELLY5_LOG_HOST "127.0.0.1"
#define JELLY5_LOG_PORT 5555
static int sends;
static std::string last_row;
static int mock_socket(int domain, int type, int protocol) {
    assert(!jf::socket_trace.active);
    assert(domain == AF_INET && type == SOCK_DGRAM && protocol == 0);
    errno = EIO;
    return 99;
}
static int mock_close(int fd) { assert(fd == 99); errno = EIO; return 0; }
static ssize_t mock_sendto(int fd, const void *buf, size_t n, int flags, const sockaddr *, socklen_t) {
    assert(fd == 99 && flags == MSG_DONTWAIT);
    ++sends;
    last_row.assign(static_cast<const char *>(buf), n);
    // Simulate accidental logger re-entry. Guard must stop nested emission.
    jf::trace_live(1, 99, 0, 0, 0, 0);
    errno = ENOBUFS;
    return -1;
}
static int mock_connect(int, const sockaddr *, socklen_t) {
    assert(last_row.find("/1/4/0/0/0/0\n") != std::string::npos); // entry already sent
    assert(errno == EBUSY);
    errno = EINPROGRESS;
    return -1;
}
static int mock_trace_getsockopt(int, int level, int opt, void *value, socklen_t *len) {
    assert(level == SOL_SOCKET && opt == SO_ERROR && *len == sizeof(int));
    *static_cast<int *>(value) = ECONNREFUSED;
    errno = EINVAL; // stale errno on successful query
    return 0;
}
static ssize_t mock_send(int, const void *, size_t n, int flags) {
    assert(n == (size_t(1) << 33) && flags == 3); errno = EAGAIN; return -1;
}
static ssize_t mock_recv(int, void *, size_t n, int flags) {
    assert(n == 4096 && flags == 2); errno = EIO; return 17;
}
static ssize_t mock_read(int, void *, size_t n) { assert(n == 23); errno = EINTR; return -1; }
static ssize_t mock_write(int, const void *, size_t n) { assert(n == 29); errno = EIO; return 29; }
static int nbio_error, nbio_calls;
static int mock_setsockopt(int fd, int level, int opt, const void *value, socklen_t len) {
    assert(fd == 7 && level == SOL_SOCKET && opt == 0x1200 && len == sizeof(int) && *static_cast<const int *>(value) == 1);
    ++nbio_calls;
    if (nbio_error) { errno = nbio_error; return -1; }
    return 0;
}
#define setsockopt mock_setsockopt
#define send mock_send
#define recv mock_recv
#define read mock_read
#define write mock_write
#define getsockopt mock_trace_getsockopt
#define connect mock_connect
#define socket mock_socket
#define close mock_close
#define sendto mock_sendto
#include "../../src/socket_trace.cpp"
#include "../../src/curl_sockopt.h"
#undef setsockopt
#undef getsockopt
#undef connect
#undef socket
#undef close
#undef sendto
#undef send
#undef recv
#undef read
#undef write
int main() {
    auto &t = jf::socket_trace;
    assert(t.summary() == "trace=0/0 first=-1/-1/-1/-1");
    t.active = true;
    assert(navi5_curl_fcntl(7, F_SETFD, FD_CLOEXEC) == 0 && errno == EINVAL);
    assert(t.count == 2 && t.events[0].op == 2 && t.events[0].result == -1 && t.events[0].error == EINVAL);
    assert(t.events[1].op == 7 && t.events[1].cmd == F_SETFD && t.events[1].result == 0 && t.events[1].error == 0);
    assert(t.summary().find("first=2/") != std::string::npos);
    for (int fd : {8, -1}) {
        assert(navi5_curl_fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 && errno == EINVAL);
        assert(t.events[t.count - 1].op == 2);
    }
    int before = probes;
    for (int cmd : {F_GETFD, F_GETFL, F_GETOWN}) {
        assert(navi5_curl_fcntl(7, cmd) == -1 && errno == EINVAL);
        assert(last_fd == 7 && last_cmd == cmd);
    }
    for (int cmd : {F_SETFL, F_SETFD}) {
        assert(navi5_curl_fcntl(7, cmd, 123) == -1 && errno == EINVAL && last_int == 123);
    }
    struct flock lock{};
    for (int cmd : {F_GETLK, F_SETLK, F_SETLKW}) {
        assert(navi5_curl_fcntl(7, cmd, &lock) == -1 && errno == EINVAL && last_lock == &lock);
    }
    assert(probes == before);
    for (int error : {EBADF, EIO, EPERM}) {
        real_error = error;
        assert(navi5_curl_fcntl(7, F_SETFD, FD_CLOEXEC) == -1 && errno == error);
    }
    real_result = 23; real_error = EIO;
    assert(navi5_curl_fcntl(7, F_SETFD, FD_CLOEXEC) == 23 && errno == EIO && probes == before);
    before = calls;
    assert(navi5_curl_fcntl(7, -999) == -1 && errno == EINVAL && calls == before);
    std::thread other([] { assert(jf::socket_trace.count == 0); navi5_curl_fcntl(1, F_GETFL); assert(jf::socket_trace.count == 0); });
    other.join();
    for (int i = 0; i < 100; ++i) t.record(3, 4, -1, EINVAL);
    assert(t.count == 16 && t.dropped > 0);
    for (char c : t.lines()) assert((c >= '0' && c <= '9') || c == '-' || c == '/' || c == '=' || c == '\n' || c == 's' || c == 'o' || c == 'c' || c == 'k');
    assert(t.lines().size() < 2048);
    t.active = false;
    before = t.dropped;
    navi5_curl_fcntl(1, F_GETFD);
    assert(t.dropped == unsigned(before));
    sends = 0; errno = EBUSY;
    jf::trace_begin();
    assert(errno == EBUSY && sends == 1 && last_row == "live=0/0/0/0/0/0/0\n");
    real_result = 0; real_error = EIO;
    assert(navi5_curl_fcntl(7, F_SETFL, 123) == 0 && errno == EIO);
    assert(sends == 3 && last_row == "live=2/2/2/" + std::to_string(F_SETFL) + "/123/0/" + std::to_string(EIO) + "\n");
    for (int i = 0; i < 200; ++i) jf::trace_live(1, 4, 0, 0, 0, 0);
    assert(sends == 129 && last_row == "live=128/4/0/0/0/0/0\n");
    errno = EBUSY;
    jf::trace_end();
    assert(errno == EBUSY && !t.active && sends == 129);
    jf::trace_begin();
    jf::trace_end();
    assert(sends == 131 && last_row == "live=1/3/0/0/0/0/0\n");
    errno = EBUSY;
    jf::trace_begin();
    assert(navi5_trace_connect(1, nullptr, 0) == -1 && errno == EINPROGRESS);
    assert(last_row == "live=2/2/4/0/0/-1/" + std::to_string(EINPROGRESS) + "\n");
    int socket_error = -1;
    socklen_t len = sizeof(socket_error);
    assert(navi5_trace_getsockopt(1, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0);
    assert(socket_error == ECONNREFUSED && errno == EINVAL);
    assert(last_row == "live=4/2/5/" + std::to_string(SO_ERROR) + "/" +
           std::to_string(ECONNREFUSED) + "/0/" + std::to_string(EINVAL) + "\n");
    jf::trace_live(5, 1, 0, 0, 7, 0);
    assert(last_row == "live=5/5/1/0/0/7/0\n");
    jf::trace_end();
    jf::trace_begin();
    assert(navi5_trace_send(1, nullptr, size_t(1) << 33, 3) == -1 && errno == EAGAIN);
    assert(jf::live_progress.line().find("/2/9/3/8589934592/-1/") != std::string::npos);
    assert(navi5_trace_recv(1, nullptr, 4096, 2) == 17 && errno == EIO);
    assert(navi5_trace_read(1, nullptr, 23) == -1 && errno == EINTR);
    assert(navi5_trace_write(1, nullptr, 29) == 29 && errno == EIO);
    // UI progress survives UDP cap/loss and never needs mailbox mutex.
    for (int i = 0; i < 200; ++i) jf::trace_live(1, 10, 0, i, 0, 0);
    assert(jf::live_progress.line().find("/1/10/0/199/0/0") != std::string::npos);
    jf::trace_end();
    // SO_NBIO goes through traced setsockopt: op3 cmd 4608, fail closed.
    jf::trace_begin();
    assert(jf::curl_sockopt(nullptr, 7, CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_OK && nbio_calls == 1);
    assert(last_row.find("/2/3/4608/0/0/") != std::string::npos);
    nbio_error = EINVAL;
    assert(jf::curl_sockopt(nullptr, 7, CURLSOCKTYPE_IPCXN) == CURL_SOCKOPT_ERROR && errno == EINVAL);
    assert(last_row.find("/2/3/4608/0/-1/" + std::to_string(EINVAL) + "\n") != std::string::npos);
    nbio_error = 0;
    // Ring: last 6 only, oldest first, numeric only.
    for (int i = 0; i < 20; ++i) jf::trace_live(1, 6, 0, i, 0, 0);
    const std::string ring = jf::live_progress.ring_line();
    assert(ring.find("/1/6/0/13/") == std::string::npos && ring.find("/1/6/0/14/") != std::string::npos);
    assert(ring.find("/1/6/0/14/") < ring.find("/1/6/0/19/"));
    unsigned spaces = 0;
    for (char c : ring.substr(8)) { assert((c >= '0' && c <= '9') || c == '-' || c == '/' || c == ' '); spaces += c == ' '; }
    assert(spaces == 6 && ring.size() < 512);
    jf::trace_end();
    jf::LiveProgress fresh;
    assert(fresh.ring_line() == "DEV ring");
    fresh.publish(1, 10, 0, 1024, 0, 0);
    assert(fresh.ring_line() == "DEV ring 0/1/10/0/1024/0/0");
}
