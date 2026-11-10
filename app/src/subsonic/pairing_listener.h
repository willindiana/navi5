/* Navi5 — bounded native pairing listener.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "pairing.h"
#include <atomic>
#include <memory>

namespace pairing {
// Single-use owner of one Session. Keep shared ownership throughout run().
// Call run() ONLY on a worker (e.g. jelly5::spawn with a shared_ptr capture).
// URL stays empty until exact-interface bind(port=0), listen and Session::start
// succeed. Never log URL. Trusted-LAN consent is the phone form's required checkbox.
class Listener {
public:
    Listener();
    ~Listener();
    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    // Blocks until consumed/cancelled/expired/error. false means startup failure.
    // Validator must enforce bounded upstream I/O; cancellation invalidates its
    // result but cannot interrupt arbitrary caller code. Its shared captures must
    // outlive that work. No sockets remain open while waiting after cancellation.
    bool run(const Session::Validator &validator, const std::string &address = "127.0.0.1",
             bool allow_lan = false, std::chrono::seconds ttl = std::chrono::seconds(300));
    void cancel();
    enum class Status { Idle, Running, Finished };
    Status status() const { return status_.load(); }
    // Read only after Status::Finished (or worker completion publication).
    Diagnostic diagnostic() const { return diagnostic_; }
    std::optional<Session::Ready> ready() { return session_->ready(); }
    std::string url() { return session_->url(); }
    bool active() { return session_->active(); }
    std::optional<Credentials> take() { return session_->take(); }
private:
    Diagnostic diagnostic_; // Worker-owned; cancel() never mutates diagnostics.
    std::shared_ptr<Session> session_;
    std::atomic<bool> used_{false}, cancelled_{false};
    std::atomic<Status> status_{Status::Idle};
};
} // namespace pairing
