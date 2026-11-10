/* Navi5 — UI-thread pairing orchestration, no I/O.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ui/pairing_view.h"
#include "subsonic/pairing.h"
#include "nuvio_input.h"

namespace app {
struct PairingFlow {
    uint64_t generation = 0;
    bool address_pending = false;
    int restarts = 2; // Automatic fresh sessions after expiry, per sheet opening. Cross retry is unlimited.
    bool begin(ui::PairingView &view, bool worker_running, double now, bool ask_address = true) {
        generation = view.generation();
        if (worker_running) {
            view.complete(generation, ui::PairingView::State::Failed, now);
            return false;
        }
        address_pending = ask_address;
        return true;
    }
    // Expired with known address and drained worker: fresh nonce/QR, budget allowing.
    bool auto_restart(ui::PairingView &view, bool worker_running) {
        if (view.state() != ui::PairingView::State::Expired || worker_running || restarts <= 0) return false;
        --restarts;
        view.start();
        return true;
    }
    void address_closed(ui::PairingView &view, bool keyboard_active, double now, bool cancelled = false) {
        if (address_pending && !keyboard_active) {
            address_pending = false;
            if (cancelled) { view.input(NUVIO_BTN_CIRCLE, now); return; }
            view.complete(generation, ui::PairingView::State::Failed, now);
            view.diagnose(generation, {pairing::Stage::ImeNoCallback, 0});
        }
    }
    // Same snapshot deadline/clock epoch as listener; never grant a fresh TTL.
    // Callback consumes credentials once. false means mailbox refused handoff.
    template<class Send>
    std::optional<bool> update(ui::PairingView &view,
            const std::optional<pairing::Session::Ready> &ready, bool /*checking*/,
            bool finished, std::optional<pairing::Credentials> &accepted,
            double now, Send send) {
        using State = ui::PairingView::State;
        if (ready) view.show_code(generation, ready->url,
            std::chrono::duration<double>(ready->deadline.time_since_epoch()).count(), now);
        // Keep QR visible during validation: failed attempts can retry with this code.
        if (!finished) return {};
        auto credentials = std::move(accepted);
        accepted.reset();
        const auto state = view.state();
        if (credentials && view.generation() == generation &&
            (state == State::Waiting || state == State::Checking)) {
            view.complete(generation, State::Success, now);
            if (view.state() == State::Success) return send(std::move(*credentials));
        }
        view.complete(generation, State::Failed, now);
        return {};
    }
};
} // namespace app
