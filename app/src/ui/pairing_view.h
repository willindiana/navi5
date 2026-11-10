/* Navi5 — temporary trusted-LAN pairing sheet.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ui/screen.h"
#include "qrcodegen.h"
#include "subsonic/pairing.h"
#include <array>
#include <vector>

namespace ui {
// UI thread only. Parent owns backdrop, frame, focus routing and worker lifetime.
// No credentials enter this view. Never log QR payloads or persist pairing data.
class PairingView {
public:
    // Consent is a required checkbox on the phone form, enforced by Session.
    // Ready is the manual (IME address) entry point; start() skips it.
    enum class State { Ready, Starting, Waiting, Checking, Success, Expired, Failed, Closed };
    // Retry: Cross on Failed/Expired, view already Starting with a new generation.
    enum class Action { None, Start, Cancel, Retry };
    State state() const { return state_; }
    uint64_t generation() const { return generation_; }
    // Call every frame, even with pressed=0, to drain expiry cancellation.
    // Monotonic seconds, same clock for input/draw/show_code. Circle wins ties.
    Action input(uint32_t pressed, double now);
    void draw(double now, float dt);
    // Marshal worker results to UI thread. Ignore stale generations. Caller must
    // stop listener on cancel/retry/expiry/success/failure; this view cannot stop workers.
    // Deadline is listener's actual expiry, not a new TTL created on delivery.
    bool show_code(uint64_t generation, const std::string &url, double deadline, double now);
    void complete(uint64_t generation, State result, double now); // Checking, Success or Failed only
    void reset(); // Reopen only after old listener has stopped. Ready state.
    void start() { reset(); state_ = State::Starting; } // Known address: no Ready step.
    // Ready-state picker for several discovered console addresses. Never log them.
    void choose(std::vector<std::string> addresses) { choices_ = std::move(addresses); choice_ = 0; }
    std::string address() const { return choices_.empty() ? std::string() : choices_[choice_]; }
    void diagnose(uint64_t generation, pairing::Diagnostic diagnostic) {
        if (generation == generation_ && state_ == State::Failed)
            diagnostic_.record(diagnostic.stage, diagnostic.error);
    }
    pairing::Diagnostic diagnostic() const { return diagnostic_; }
    bool animating() const { return animating_; }
private:
    void clear_code();
    void expire(double now);
    pairing::Diagnostic diagnostic_;
    std::vector<std::string> choices_;
    size_t choice_ = 0;
    State state_ = State::Ready;
    uint64_t generation_ = 0;
    double deadline_ = 0;
    // ponytail: version 10 caps QR density; enlarge only if listener URLs outgrow it.
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(10)> qr_{};
    Drop drop_;
    bool animating_ = false, cancel_pending_ = false;
};
} // namespace ui
