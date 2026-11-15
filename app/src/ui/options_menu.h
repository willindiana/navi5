/* Navi5 — Options menu: Log out behind an explicit confirmation.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "nuvio_input.h"
#include <cstdint>

namespace ui {
// UI thread only, no I/O. Parent draws it and performs the logout.
struct OptionsMenu {
    enum class State { Closed, Menu, Confirm };
    enum class Action { None, Logout };
    State state = State::Closed;
    bool open() const { return state != State::Closed; }
    void show() { state = State::Menu; }
    void close() { state = State::Closed; }
    // Circle wins ties, so a combined press can never confirm.
    Action input(uint32_t pressed) {
        if (state == State::Closed) return Action::None;
        if (pressed & NUVIO_BTN_OPTIONS) { state = State::Closed; return Action::None; }
        if (pressed & NUVIO_BTN_CIRCLE) {
            state = state == State::Confirm ? State::Menu : State::Closed;
            return Action::None;
        }
        if (!(pressed & NUVIO_BTN_CROSS)) return Action::None;
        if (state == State::Menu) { state = State::Confirm; return Action::None; }
        state = State::Closed;
        return Action::Logout;
    }
};
} // namespace ui
