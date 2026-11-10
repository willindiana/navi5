/* Navi5 — pinned curl-only native socket CLOEXEC compatibility.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef EVO_TARGET_PS5
#include "socket_trace.h"
#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <sys/socket.h>

extern "C" int navi5_curl_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    int result, arg = 0;
    auto entry = [&](int value) {
#ifdef NAVI5_DEV_TOOLS
        jf::trace_live(1, 2, cmd, value, 0, 0);
#else
        (void)value;
#endif
    };
    switch (cmd) {
    case F_GETFD: case F_GETFL: case F_GETOWN:
        entry(0);
        result = fcntl(fd, cmd); break;
    case F_GETLK: case F_SETLK: case F_SETLKW:
        entry(0);
        result = fcntl(fd, cmd, va_arg(ap, struct flock *)); break;
    case F_SETFD: case F_SETFL:
        arg = va_arg(ap, int);
        entry(arg);
        result = fcntl(fd, cmd, arg); break;
    default:
        // ponytail: only pinned curl callers; re-audit before adding commands.
        va_end(ap);
        errno = EINVAL;
        return -1;
    }
    const int saved = errno;
    va_end(ap);
#ifdef NAVI5_DEV_TOOLS
    jf::trace_live(2, 2, cmd, arg, result, saved);
    jf::socket_trace.record(2, cmd, result, saved);
#endif
    if (result == -1 && saved == EINVAL && cmd == F_SETFD && arg == FD_CLOEXEC) {
        int type = 0;
        socklen_t len = sizeof type;
        // Never mask file/pipe/invalid-fd errors. Native title never execs.
#ifdef NAVI5_DEV_TOOLS
        jf::trace_live(1, 8, SO_TYPE, 0, 0, 0);
#endif
        const int probe = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len);
#ifdef NAVI5_DEV_TOOLS
        jf::trace_live(2, 8, SO_TYPE, 0, probe, errno);
#endif
        if (probe == 0 && len == sizeof type && type > 0) {
#ifdef NAVI5_DEV_TOOLS
            jf::trace_live(2, 7, cmd, arg, 0, 0);
            jf::socket_trace.record(7, cmd, 0, 0); // explicit CLOEXEC fallback
#endif
            errno = saved;
            return 0;
        }
    }
    errno = saved;
    return result;
}
#endif
