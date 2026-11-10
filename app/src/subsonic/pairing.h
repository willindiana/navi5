/* Navi5 — temporary pairing protocol core.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "subsonic.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>

namespace pairing {
// Numeric-only diagnostics. First cause wins; cancellation never records a cause.
enum class Stage : unsigned {
    None = 0, ImeNoCallback = 1, WorkerSpawn = 2, Arguments = 3,
    AddressParse = 4, AddressPolicy = 5, Socket = 6, Nonblocking = 7,
    NoSigpipe = 8, Bind = 9, Listen = 10, Getsockname = 11,
    EntropyOpen = 12, EntropyRead = 13, Poll = 14, Accept = 15,
    QrInput = 16, QrEncode = 17
};
struct Diagnostic {
    Stage stage = Stage::None;
    int error = 0; // Captured errno, or 0 when no syscall failed (including EOF).
    void record(Stage cause, int code = 0) {
        if (stage == Stage::None) { stage = cause; error = code; }
    }
};
struct Credentials {
    std::string server, username, password, api_key;
};
// No persistence. Caller must keep accepted credentials in session memory only.
// Transport must verify TLS, refuse redirects, bound responses/time, and never log URLs.
// *failure (optional) gets a numeric reason; never untrusted server text.
bool validate(const Credentials &credentials, const subsonic::HttpGet &http,
              subsonic::LoginFailure *failure = nullptr);

// Native framing failures use the same styled, fixed-text pages.
std::string reject_request(int status);

class Session {
public:
    using Clock = std::chrono::steady_clock;
    // Set failure to explain a false result; it maps to fixed phone-page text.
    using Validator = std::function<bool(const Credentials &, subsonic::LoginFailure &)>;
    static constexpr size_t max_header = 4096, max_body = 4096;
    static constexpr unsigned max_failures = 5; // Failed validations before nonce is consumed.
    // Exact numeric IPv4 only: loopback, or explicit opt-in RFC1918 address.
    // Native listener MUST bind this exact address, never INADDR_ANY.
    bool start(const std::string &address, unsigned port, bool allow_lan,
               std::chrono::seconds ttl = std::chrono::seconds(300), Diagnostic *diagnostic = nullptr);
    void cancel();
    bool active();
    struct Ready { std::string url; Clock::time_point deadline; };
    std::optional<Ready> ready(); // Atomic live URL/deadline snapshot; never log.
    std::string url(); // QR payload; contains only local URL and random nonce. Never log.
    // One COMPLETE request only. Native adapter must enforce byte/deadline limits
    // while receiving, close every connection, and close listener when !active().
    // Session stays active during validation (concurrent POSTs refused) so a failed
    // attempt can re-render the form; success or max_failures consumes the nonce.
    std::string handle(const std::string &request, const Validator &validate);
    std::optional<Credentials> take();
private:
    bool live(); // mutex held
    std::mutex mutex_;
    bool active_ = false, validating_ = false;
    unsigned failures_ = 0;
    unsigned long generation_ = 0;
    Clock::time_point deadline_;
    std::string authority_, nonce_;
    std::optional<Credentials> accepted_;
};
} // namespace pairing
