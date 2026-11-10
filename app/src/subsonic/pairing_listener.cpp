/* Navi5 — bounded native pairing listener.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pairing_listener.h"
#include "../app/spawn.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <string_view>

namespace pairing {
namespace {
using Clock = Session::Clock;
constexpr auto io_timeout = std::chrono::seconds(3);
struct Socket {
    int fd = -1;
    ~Socket() { reset(); }
    void reset() { if (fd >= 0) { close(fd); fd = -1; } }
};
bool setup(int fd, Diagnostic &diagnostic)
{
    auto fail = [&](Stage stage, int error) { diagnostic.record(stage, error); return false; };
#ifdef EVO_TARGET_PS5
    // PS5 ignores O_NONBLOCK for I/O. SO_NBIO must succeed, no fallback.
    const int one = 1;
    if (setsockopt(fd, SOL_SOCKET, 0x1200, &one, sizeof one)) return fail(Stage::Nonblocking, errno);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) return fail(Stage::Nonblocking, errno);
#endif
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe)) return fail(Stage::NoSigpipe, errno);
#elif !defined(MSG_NOSIGNAL)
#error Native pairing sockets require per-socket SIGPIPE suppression
#endif
    return true;
}
bool private_ip(uint32_t n)
{
    return (n >> 24) == 10 || (n >> 20) == 0xac1 || (n >> 16) == 0xc0a8;
}
bool retry() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
// Framing only; Session::handle remains authority for HTTP/security validation.
// Fixed storage is filled before any request-sized allocation. Canonical decimal
// Content-Length only; ambiguous framing fails closed, never waits for its body.
bool length(std::string_view header, size_t &body)
{
    body = 0;
    bool seen = false;
    auto pos = header.find("\r\n");
    if (pos == header.npos) return false;
    pos += 2;
    while (pos + 2 < header.size()) {
        auto end = header.find("\r\n", pos), colon = header.find(':', pos);
        if (end == header.npos || colon == header.npos || colon >= end) return false;
        auto name = header.substr(pos, colon - pos);
        std::array<char, Session::max_header> lower{};
        for (size_t i = 0; i < name.size(); ++i) {
            unsigned char c = name[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (!(c >= 'a' && c <= 'z') && c != '-') return false;
            lower[i] = c;
        }
        std::string_view key(lower.data(), name.size());
        if (key == "transfer-encoding" || key == "expect" || key == "upgrade") return false;
        if (key == "content-length") {
            if (seen) return false;
            seen = true;
            auto value = header.substr(colon + 1, end - colon - 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
            for (char c : value) {
                if (c < '0' || c > '9' || body > Session::max_body / 10) return false;
                body = body * 10 + c - '0';
                if (body > Session::max_body) return false;
            }
        }
        pos = end + 2;
    }
    return true;
}
struct Reply {
    std::mutex mutex;
    bool done = false;
    std::string response;
};
} // namespace
Listener::Listener() : session_(std::make_shared<Session>()) {}
Listener::~Listener() { cancel(); }
void Listener::cancel() { cancelled_ = true; session_->cancel(); }
bool Listener::run(const Session::Validator &validator, const std::string &address,
                   bool allow_lan, std::chrono::seconds ttl)
{
    if (used_.exchange(true)) return false;
    status_ = Status::Running;
    struct Finished {
        std::atomic<Status> &status;
        ~Finished() { status = Status::Finished; }
    } finished{status_};
    auto fail = [&](Stage stage, int error = 0) { diagnostic_.record(stage, error); return false; };
    if (cancelled_) return false;
    if (!validator || ttl.count() <= 0 || ttl.count() > 300) return fail(Stage::Arguments);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (address.find('\0') != std::string::npos || inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) return fail(Stage::AddressParse);
    const auto ip = ntohl(local.sin_addr.s_addr);
    const bool loopback = (ip >> 24) == 127;
    if (!loopback && !(allow_lan && private_ip(ip))) return fail(Stage::AddressPolicy);
    Socket listener;
    listener.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener.fd < 0) return fail(Stage::Socket, errno);
    if (!setup(listener.fd, diagnostic_)) return false;
    if (bind(listener.fd, reinterpret_cast<sockaddr *>(&local), sizeof local)) return fail(Stage::Bind, errno);
    if (listen(listener.fd, 1)) return fail(Stage::Listen, errno);
    socklen_t size = sizeof local;
    if (getsockname(listener.fd, reinterpret_cast<sockaddr *>(&local), &size)) return fail(Stage::Getsockname, errno);
    if (!session_->start(address, ntohs(local.sin_port), allow_lan, ttl, &diagnostic_)) return false;
    auto ready = session_->ready();
    if (cancelled_ || !ready) { session_->cancel(); return false; }
    const auto session_deadline = ready->deadline;
    ready.reset(); // Do not retain nonce snapshot throughout listener lifetime.
    // Only this worker closes descriptors. cancel() changes state, never touches
    // descriptors: no close/reuse/shutdown races. 25ms polling bounds wake latency.
    auto live = [&] { return !cancelled_ && session_->active(); };
    auto wait = [&](int fd, short events, Clock::time_point deadline, bool consumed = false) {
        if (cancelled_ || Clock::now() >= deadline || Clock::now() >= session_deadline ||
            (!consumed && !session_->active())) return false;
        pollfd p{fd, events, 0};
        int r = poll(&p, 1, 25);
        if (r < 0 && errno != EINTR) return fail(Stage::Poll, errno);
        return true;
    };
    bool pending = false;
    for (unsigned attempt = 0; attempt < 32 && live();) {
        if (!wait(listener.fd, POLLIN, Clock::now() + io_timeout)) break;
        sockaddr_in peer{};
        size = sizeof peer;
        Socket client;
        client.fd = accept(listener.fd, reinterpret_cast<sockaddr *>(&peer), &size);
        if (client.fd < 0) { if (retry()) continue; fail(Stage::Accept, errno); break; }
        ++attempt;
        const auto remote = ntohl(peer.sin_addr.s_addr);
        if (peer.sin_family != AF_INET || ((remote >> 24) != 127 && (loopback || !private_ip(remote)))) continue;
        if (!setup(client.fd, diagnostic_)) break;
        std::array<char, Session::max_header + Session::max_body + 1> buffer{};
        size_t used = 0, target = 0;
        const auto read_deadline = Clock::now() + io_timeout;
        bool complete = false;
        while (live() && wait(client.fd, POLLIN, read_deadline)) {
            // Header cannot grow beyond its own bound, even before terminator.
            size_t cap = target ? target + 1 : Session::max_header;
            ssize_t n = recv(client.fd, buffer.data() + used, cap - used, 0);
            if (n < 0) { if (retry()) continue; break; }
            if (!n) break;
            used += static_cast<size_t>(n);
            if (!target) {
                std::string_view bytes(buffer.data(), used);
                auto end = bytes.find("\r\n\r\n");
                if (end == bytes.npos) { if (used == Session::max_header) break; continue; }
                size_t body = 0;
                if (!length(bytes.substr(0, end + 4), body)) break;
                target = end + 4 + body;
            }
            if (used > target) break; // Pipelined extra bytes already received.
            if (used == target) { complete = true; break; }
        }
        if (!live()) continue;
        // Reject framing/queued extra bytes without invoking validator.
        char extra;
        ssize_t peek = recv(client.fd, &extra, 1, MSG_PEEK);
        if (peek > 0 || (peek < 0 && !retry())) complete = false;
        auto reply = std::make_shared<Reply>();
        auto session = session_;
        if (!complete) {
            reply->response = reject_request(used >= Session::max_header ? 413 : 400);
            reply->done = true;
        } else if (!jelly5::spawn_media([session, reply, validator, request = std::string(buffer.data(), used)] {
            auto response = session->handle(request, validator);
            std::lock_guard<std::mutex> lock(reply->mutex);
            reply->response = std::move(response);
            reply->done = true;
        })) { fail(Stage::WorkerSpawn); break; }
        pending = true;
        bool done = false;
        // Session stays live during validation so a failed attempt can retry;
        // close listening socket once consumed, expired or cancelled.
        while (!cancelled_) {
            if (!session_->active()) listener.reset();
            { std::lock_guard<std::mutex> lock(reply->mutex); done = reply->done; }
            if (done) break;
            if (Clock::now() >= session_deadline) break;
            poll(nullptr, 0, 25);
        }
        if (!done || cancelled_) break;
        pending = false;
        const auto write_deadline = Clock::now() + io_timeout;
        size_t sent = 0;
        while (sent < reply->response.size() && wait(client.fd, POLLOUT, write_deadline, true)) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            ssize_t n = send(client.fd, reply->response.data() + sent, reply->response.size() - sent, flags);
            if (n < 0) { if (retry()) continue; break; }
            if (!n) break;
            sent += static_cast<size_t>(n);
        }
    }
    listener.reset();
    // Preserve successful take(); invalidate pending work on timeout/error.
    if (session_->active() || cancelled_ || pending) session_->cancel();
    return true;
}
} // namespace pairing
