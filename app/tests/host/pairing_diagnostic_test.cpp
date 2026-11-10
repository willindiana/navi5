/* Navi5 — synthetic syscall failures, no sockets or entropy fallback.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing_listener.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cassert>
#include <algorithm>
using pairing::Stage;
namespace {
Stage gate = Stage::None;
int failure(Stage stage) { if (gate != stage) return 0; errno = EIO; return -1; }
int fake_socket(int, int, int) { return failure(Stage::Socket) ? -1 : 123; }
[[maybe_unused]] int fake_fcntl(int, int, ...) { return failure(Stage::Nonblocking); }
int fake_setsockopt(int, int, int option, const void *, socklen_t) {
    return failure(option == 0x1200 ? Stage::Nonblocking : Stage::NoSigpipe);
}
int fake_bind(int, const sockaddr *, socklen_t) { return failure(Stage::Bind); }
int fake_listen(int, int) { return failure(Stage::Listen); }
int fake_getsockname(int, sockaddr *address, socklen_t *) {
    if (failure(Stage::Getsockname)) return -1;
    reinterpret_cast<sockaddr_in *>(address)->sin_port = htons(12345); return 0;
}
int fake_open(const char *, int, ...) { return failure(Stage::EntropyOpen) ? -1 : 124; }
ssize_t fake_read(int, void *bytes, size_t size) {
    if (failure(Stage::EntropyRead)) return -1;
    if (gate != Stage::Poll && gate != Stage::Accept) return 0;
    std::fill_n(static_cast<unsigned char *>(bytes), size, 0); // Synthetic fixture only.
    return static_cast<ssize_t>(size);
}
int fake_close(int) { errno = EBADF; return 0; } // Cleanup must not replace captured errno.
int fake_poll(pollfd *, nfds_t, int) { return failure(Stage::Poll); }
int fake_accept(int, sockaddr *, socklen_t *) { errno = EIO; return -1; }
}
#define socket fake_socket
#define fcntl fake_fcntl
#define setsockopt fake_setsockopt
#define bind fake_bind
#define listen fake_listen
#define getsockname fake_getsockname
#define open fake_open
#define read fake_read
#define close fake_close
#define poll fake_poll
#define accept fake_accept
#include "../../src/subsonic/pairing.cpp"
#include "../../src/subsonic/pairing_listener.cpp"
#undef socket
#undef fcntl
#undef setsockopt
#undef bind
#undef listen
#undef getsockname
#undef open
#undef read
#undef close
#undef poll
#undef accept
int main() {
    pairing::Diagnostic first;
    first.record(Stage::Bind, EIO); first.record(Stage::Listen, EBADF);
    assert(first.stage == Stage::Bind && first.error == EIO);
    auto validator = [](const auto &, subsonic::LoginFailure &) { return true; };
    for (auto stage : {Stage::Socket, Stage::Nonblocking, Stage::NoSigpipe, Stage::Bind,
                       Stage::Listen, Stage::Getsockname, Stage::EntropyOpen, Stage::EntropyRead}) {
#ifndef SO_NOSIGPIPE
        if (stage == Stage::NoSigpipe) continue;
#endif
        gate = stage;
        pairing::Listener listener;
        assert(!listener.run(validator));
        auto diagnostic = listener.diagnostic();
        assert(diagnostic.stage == stage && diagnostic.error == EIO);
        listener.cancel();
        assert(listener.diagnostic().stage == stage && listener.diagnostic().error == EIO);
        assert(!listener.ready());
    }
    for (auto stage : {Stage::Poll, Stage::Accept}) {
        gate = stage;
        pairing::Listener listener;
        assert(listener.run(validator));
        assert(listener.diagnostic().stage == stage && listener.diagnostic().error == EIO);
        listener.cancel();
        assert(listener.diagnostic().stage == stage);
    }
    gate = Stage::None; // Zero-byte entropy read, not stale cleanup errno.
    pairing::Session session;
    pairing::Diagnostic eof;
    assert(!session.start("127.0.0.1", 12345, false, std::chrono::seconds(120), &eof));
    assert(eof.stage == Stage::EntropyRead && eof.error == 0 && !session.ready());
    pairing::Listener cancelled;
    cancelled.cancel();
    assert(!cancelled.run(validator));
    assert(cancelled.diagnostic().stage == Stage::None);
    pairing::Listener parse;
    assert(!parse.run(validator, "invalid"));
    assert(parse.diagnostic().stage == Stage::AddressParse && parse.diagnostic().error == 0);
    pairing::Listener policy;
    assert(!policy.run(validator, "0.0.0.0"));
    assert(policy.diagnostic().stage == Stage::AddressPolicy && policy.diagnostic().error == 0);
}
