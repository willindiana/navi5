/* Navi5 — login screen focus: entry form on the left, QR area on the right.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "nuvio_input.h"
#include <cstdint>

namespace ui {
// UI thread only, no I/O. Parent owns pairing, keyboard, consent and login.
// One screen, two ways in; whichever completes first wins (parent: a phone
// sign-in discards the form attempt, Connect stops the listener).
struct LoginView {
    enum class Focus { Server, Username, Password, Show, Connect, Qr };
    enum class Action { None, Server, Username, Password, ToggleShow, Connect };
    Focus focus = Focus::Server;
    Focus row = Focus::Server; // Form row to return to from the QR area.
    // Picker, retry or restart needs the pad: move focus to the QR area.
    void attention(bool qr_available) {
        if (qr_available && focus != Focus::Qr) { row = focus; focus = Focus::Qr; }
    }
    // qr_pressed receives the presses meant for the pairing view (zero unless
    // the QR area is focused). Left/Right switch sides; they never reach it.
    Action input(uint32_t pressed, bool qr_available, uint32_t &qr_pressed) {
        qr_pressed = 0;
        // QR area vanished under the focus: a press aimed at it must not hit the form.
        if (!qr_available && focus == Focus::Qr) { focus = row; return Action::None; }
        if (focus == Focus::Qr) {
            if (pressed & NUVIO_BTN_LEFT) { focus = row; return Action::None; }
            qr_pressed = pressed & (NUVIO_BTN_UP | NUVIO_BTN_DOWN | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE);
            return Action::None;
        }
        if ((pressed & NUVIO_BTN_RIGHT) && qr_available) { row = focus; focus = Focus::Qr; return Action::None; }
        const int at = static_cast<int>(focus);
        if (pressed & NUVIO_BTN_UP) focus = static_cast<Focus>(at > 0 ? at - 1 : 0);
        else if (pressed & NUVIO_BTN_DOWN) focus = static_cast<Focus>(at < 4 ? at + 1 : 4);
        else if (pressed & NUVIO_BTN_CROSS) {
            switch (focus) {
            case Focus::Server: return Action::Server;
            case Focus::Username: return Action::Username;
            case Focus::Password: return Action::Password;
            case Focus::Show: return Action::ToggleShow;
            default: return Action::Connect;
            }
        }
        return Action::None;
    }
};
// "Saved login failed": Retry stays first; Forget is offered here because the
// normal login screen no longer has it (signed-in users log out instead).
struct SavedFailedView {
    enum class Action { None, Retry, Forget, Login };
    int row = 0; // 0: Retry, 1: Forget saved login.
    Action input(uint32_t pressed) {
        if (pressed & NUVIO_BTN_CIRCLE) { row = 0; return Action::Login; }
        if (pressed & NUVIO_BTN_UP) row = 0;
        else if (pressed & NUVIO_BTN_DOWN) row = 1;
        else if (pressed & NUVIO_BTN_CROSS) {
            const auto action = row ? Action::Forget : Action::Retry;
            row = 0;
            return action;
        }
        return Action::None;
    }
};
} // namespace ui
