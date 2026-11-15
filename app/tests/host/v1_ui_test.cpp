/* Navi5 — v1.0 controls: button routing, Options/logout menu, login-screen
 * focus, fullscreen control row and the queue view. No renderer or worker.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-private-field" // Drop is never drawn here.
#endif
#include "ui/login_view.h"
#include "ui/options_menu.h"
#include "ui/pad_route.h"
#include "ui/player_state.h"
#include "ui/queue_view.h"
#include <cassert>
#include <iostream>

using ui::music::QueueRow;
using ui::music::QueueWindow;

// A worker stand-in: the whole queue lives here, the view only sees windows.
struct FakeQueue {
    std::vector<QueueRow> all;
    size_t position = 0;
    uint64_t serial = 0;
    bool pending = false;
    size_t builds = 0, largest = 0;
    QueueWindow window(size_t first) {
        QueueWindow w;
        w.serial = ++serial; w.position = position; w.total = all.size(); w.pending = pending;
        w.first = std::min(std::max(first, position), all.size());
        for (size_t i = w.first; i < std::min(all.size(), w.first + QueueWindow::capacity); ++i) w.rows.push_back(all[i]);
        ++builds; largest = std::max(largest, w.rows.size());
        return w;
    }
    void apply(const ui::QueueView::Action &action) {
        const auto find = [&](uint64_t uid) {
            for (size_t i = 0; i < all.size(); ++i) if (all[i].uid == uid) return i;
            assert(false); return size_t(0);
        };
        if (action.kind == ui::QueueView::Action::Remove) { const auto at = find(action.uid); assert(at > position); all.erase(all.begin() + at); }
        else if (action.kind == ui::QueueView::Action::Move) {
            const auto from = find(action.uid);
            assert(from > position);
            const auto row = all[from];
            all.erase(all.begin() + from);
            const auto anchor = find(action.anchor);
            assert(anchor >= position);
            all.insert(all.begin() + anchor + 1, row);
        }
    }
};
static FakeQueue queue_of(size_t n, size_t position = 0)
{
    FakeQueue q; q.position = position;
    for (size_t i = 0; i < n; ++i) q.all.push_back({1000 + i, "T" + std::to_string(i), "Artist", 60});
    return q;
}

int main()
{
    // --- Button routing (ui/pad_route.h), the table main.cpp acts on. ---
    const ui_pad_context browse{0, 0, 0, 0, 1}, search{0, 0, 0, 1, 1}, player{1, 0, 0, 0, 1},
        queue{1, 1, 0, 0, 1}, menu{0, 0, 1, 0, 1}, idle{0, 0, 0, 0, 0};
    auto r = ui_route_pad(NUVIO_BTN_TRIANGLE, browse);
    assert(r.play_next && !r.open_player && !r.menu && !r.pause_toggle && r.rest == 0);
    // Triangle is Play Next only: never forwarded, so it cannot open the player or the top nav.
    for (auto c : {browse, search, player, queue, menu, idle})
        assert(!(ui_route_pad(NUVIO_BTN_TRIANGLE, c).rest & NUVIO_BTN_TRIANGLE) && !ui_route_pad(NUVIO_BTN_TRIANGLE, c).open_player);
    assert(!ui_route_pad(NUVIO_BTN_TRIANGLE, player).play_next && !ui_route_pad(NUVIO_BTN_TRIANGLE, queue).play_next);
    assert(!ui_route_pad(NUVIO_BTN_TRIANGLE, menu).play_next);
    assert(ui_route_pad(NUVIO_BTN_TRIANGLE, search).play_next); // Focused search result.
    // Touchpad opens the fullscreen player; it is never Stop and never forwarded.
    r = ui_route_pad(NUVIO_BTN_TOUCHPAD, browse);
    assert(r.open_player && !r.pause_toggle && !r.play_next && r.rest == 0);
    assert(!ui_route_pad(NUVIO_BTN_TOUCHPAD, idle).open_player);   // Nothing loaded.
    assert(!ui_route_pad(NUVIO_BTN_TOUCHPAD, player).open_player); // Already open.
    assert(!ui_route_pad(NUVIO_BTN_TOUCHPAD, menu).open_player);
    // Options opens the menu and swallows everything else pressed with it.
    r = ui_route_pad(NUVIO_BTN_OPTIONS | NUVIO_BTN_CROSS | NUVIO_BTN_SQUARE | NUVIO_BTN_TRIANGLE | NUVIO_BTN_PLAYPAUSE, browse);
    assert(r.menu && r.rest == 0 && !r.pause_toggle && !r.play_next && !r.search_backspace);
    assert(ui_route_pad(NUVIO_BTN_OPTIONS, player).menu && ui_route_pad(NUVIO_BTN_OPTIONS, queue).menu);
    // Open menu is modal: it alone sees Cross/Circle/Options; Square and Triangle do nothing.
    r = ui_route_pad(NUVIO_BTN_OPTIONS | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE | NUVIO_BTN_SQUARE | NUVIO_BTN_TRIANGLE | NUVIO_BTN_TOUCHPAD, menu);
    assert(!r.menu && r.rest == (NUVIO_BTN_OPTIONS | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE));
    assert(!r.pause_toggle && !r.search_backspace && !r.play_next && !r.open_player);
    // Square: backspace in Search, remove in the queue view, pause/resume elsewhere.
    r = ui_route_pad(NUVIO_BTN_SQUARE, search);
    assert(r.search_backspace && !r.pause_toggle && r.rest == 0);
    r = ui_route_pad(NUVIO_BTN_SQUARE, browse);
    assert(r.pause_toggle && !r.search_backspace && r.rest == 0);
    const ui_pad_context search_player{1, 0, 0, 1, 1};
    r = ui_route_pad(NUVIO_BTN_SQUARE, search_player); // Fullscreen over Search: pause, not backspace.
    assert(r.pause_toggle && !r.search_backspace);
    r = ui_route_pad(NUVIO_BTN_SQUARE, queue);
    assert(!r.pause_toggle && !r.search_backspace && r.rest == NUVIO_BTN_SQUARE);
    // Square+Triangle together in Search: one backspace and one Play Next, no pause.
    r = ui_route_pad(NUVIO_BTN_SQUARE | NUVIO_BTN_TRIANGLE, search);
    assert(r.search_backspace && r.play_next && !r.pause_toggle);
    // Remote Play/Pause works everywhere, including under the menu and queue view.
    for (auto c : {browse, search, player, queue, menu}) assert(ui_route_pad(NUVIO_BTN_PLAYPAUSE, c).pause_toggle);
    // Navigation is untouched.
    const uint32_t nav = NUVIO_BTN_DPAD | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE | NUVIO_BTN_L1 | NUVIO_BTN_R1;
    for (auto c : {browse, search, player, queue}) assert(ui_route_pad(nav, c).rest == nav);

    // --- Options menu: Log out needs two Cross presses; anything else backs out. ---
    using Menu = ui::OptionsMenu;
    Menu m;
    assert(!m.open() && m.input(NUVIO_BTN_CROSS) == Menu::Action::None && !m.open());
    m.show();
    assert(m.open() && m.state == Menu::State::Menu);
    assert(m.input(0) == Menu::Action::None && m.input(NUVIO_BTN_UP | NUVIO_BTN_DOWN) == Menu::Action::None);
    assert(m.input(NUVIO_BTN_CROSS) == Menu::Action::None && m.state == Menu::State::Confirm);
    assert(m.input(NUVIO_BTN_CIRCLE) == Menu::Action::None && m.state == Menu::State::Menu);
    assert(m.input(NUVIO_BTN_CIRCLE) == Menu::Action::None && !m.open());
    m.show(); m.input(NUVIO_BTN_CROSS);
    assert(m.input(NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE) == Menu::Action::None && m.state == Menu::State::Menu); // Cancel wins.
    m.input(NUVIO_BTN_CROSS);
    assert(m.input(NUVIO_BTN_OPTIONS | NUVIO_BTN_CROSS) == Menu::Action::None && !m.open()); // Options closes.
    m.show(); m.input(NUVIO_BTN_CROSS);
    assert(m.input(NUVIO_BTN_CROSS) == Menu::Action::Logout && !m.open());
    assert(m.input(NUVIO_BTN_CROSS) == Menu::Action::None); // Exactly once.

    // --- Login screen focus: entry form left, QR area right. ---
    using Login = ui::LoginView;
    Login login;
    uint32_t qr = 99;
    assert(login.focus == Login::Focus::Server);
    // Each row opens only its own field; rows clamp at both ends.
    assert(login.input(NUVIO_BTN_UP, true, qr) == Login::Action::None && login.focus == Login::Focus::Server);
    assert(login.input(NUVIO_BTN_CROSS, true, qr) == Login::Action::Server && qr == 0);
    const Login::Action expected[] = {Login::Action::Username, Login::Action::Password, Login::Action::ToggleShow, Login::Action::Connect};
    for (auto action : expected) {
        assert(login.input(NUVIO_BTN_DOWN, true, qr) == Login::Action::None);
        assert(login.input(NUVIO_BTN_CROSS, true, qr) == action && qr == 0);
    }
    assert(login.focus == Login::Focus::Connect);
    assert(login.input(NUVIO_BTN_DOWN, true, qr) == Login::Action::None && login.focus == Login::Focus::Connect);
    assert(login.input(NUVIO_BTN_CROSS, true, qr) == Login::Action::Connect);
    // Form presses never reach the pairing view; QR-area presses never trigger the form.
    assert(login.input(NUVIO_BTN_CIRCLE, true, qr) == Login::Action::None && qr == 0 && login.focus == Login::Focus::Connect);
    assert(login.input(NUVIO_BTN_LEFT, true, qr) == Login::Action::None && login.focus == Login::Focus::Connect); // Nothing left of the form.
    assert(login.input(NUVIO_BTN_RIGHT | NUVIO_BTN_CROSS, true, qr) == Login::Action::None && login.focus == Login::Focus::Qr && qr == 0);
    assert(login.input(NUVIO_BTN_CROSS | NUVIO_BTN_UP | NUVIO_BTN_SQUARE, true, qr) == Login::Action::None);
    assert(qr == (NUVIO_BTN_CROSS | NUVIO_BTN_UP) && login.focus == Login::Focus::Qr);
    assert(login.input(NUVIO_BTN_CIRCLE, true, qr) == Login::Action::None && qr == NUVIO_BTN_CIRCLE);
    assert(login.input(NUVIO_BTN_RIGHT, true, qr) == Login::Action::None && login.focus == Login::Focus::Qr);
    // Left returns to the row it was left from, without activating it.
    assert(login.input(NUVIO_BTN_LEFT | NUVIO_BTN_CROSS, true, qr) == Login::Action::None && qr == 0 && login.focus == Login::Focus::Connect);
    // No private address: the form only; the QR area cannot take focus.
    assert(login.input(NUVIO_BTN_RIGHT, false, qr) == Login::Action::None && login.focus == Login::Focus::Connect);
    login.attention(false); assert(login.focus == Login::Focus::Connect);
    login.input(NUVIO_BTN_UP, true, qr);
    login.attention(true); assert(login.focus == Login::Focus::Qr && login.row == Login::Focus::Show);
    login.attention(true); assert(login.row == Login::Focus::Show); // Repeated attention keeps the return row.
    // QR area vanished under the focus: the press aimed at it does nothing to the form.
    assert(login.input(NUVIO_BTN_CROSS, false, qr) == Login::Action::None && qr == 0 && login.focus == Login::Focus::Show);

    // --- Saved login failed: Retry first, Forget offered, Circle signs in again. ---
    using Failed = ui::SavedFailedView;
    Failed failed;
    assert(failed.input(0) == Failed::Action::None && failed.row == 0);
    assert(failed.input(NUVIO_BTN_CROSS) == Failed::Action::Retry);
    assert(failed.input(NUVIO_BTN_DOWN) == Failed::Action::None && failed.row == 1);
    assert(failed.input(NUVIO_BTN_DOWN) == Failed::Action::None && failed.row == 1);
    assert(failed.input(NUVIO_BTN_CROSS) == Failed::Action::Forget && failed.row == 0); // Never armed twice.
    failed.input(NUVIO_BTN_DOWN);
    assert(failed.input(NUVIO_BTN_CIRCLE | NUVIO_BTN_CROSS) == Failed::Action::Login && failed.row == 0);
    failed.input(NUVIO_BTN_DOWN); failed.input(NUVIO_BTN_UP);
    assert(failed.input(NUVIO_BTN_CROSS) == Failed::Action::Retry);

    // --- Fullscreen control row: previous, play, next, shuffle, stop, queue. ---
    ui::PlayerState p;
    p.open(0);
    std::vector<int> walked{p.focus};
    for (int i = 0; i < 6; ++i) { p.direction(1, 1 + i); walked.push_back(p.focus); }
    assert((walked == std::vector<int>{1, 2, 4, 5, 6, 6, 6}));
    for (int i = 0; i < 6; ++i) p.direction(-1, 2);
    assert(p.focus == 0);
    assert(ui::PlayerState::slot(4) == 3 && ui::PlayerState::slot(5) == 4 && ui::PlayerState::slot(6) == 5);
    p.focus = 5; p.navigate(0, 1, 3, true, 2, 30); assert(p.focus == 3); // Down: seek bar.
    p.navigate(0, -1, 3, true, 2, 30); assert(p.focus == 1);
    p.focus = 6; p.direction(1, 20); assert(p.focus == 6 && p.controls(20)); // Idle press only wakes.

    // --- Queue view. ---
    using View = ui::QueueView;
    using Act = View::Action;
    View v;
    auto q = queue_of(6, 1); // T0 played, T1 current.
    assert(v.input(NUVIO_BTN_CROSS, 0).kind == Act::None); // Closed: inert.
    v.open();
    assert(v.visible() && v.wanted_first() == 0);
    v.sync(q.window(v.wanted_first()), 0);
    assert(v.position() == 1 && v.cursor() == 1 && v.total() == 6 && v.row(1)->title == "T1");
    assert(v.row(0) == nullptr); // History is not part of the view.
    // Current track cannot be picked up or removed.
    assert(v.input(NUVIO_BTN_CROSS, 0).kind == Act::None && !v.holding());
    assert(v.input(NUVIO_BTN_SQUARE, 0).kind == Act::None && !v.busy());
    assert(v.input(NUVIO_BTN_UP, 0).kind == Act::None && v.cursor() == 1);
    v.input(NUVIO_BTN_DOWN, 0); v.input(NUVIO_BTN_DOWN, 0);
    assert(v.cursor() == 3);
    // Pick up T3, carry it up to the successor slot, drop: one Move, anchored on current.
    assert(v.input(NUVIO_BTN_CROSS, 0).kind == Act::None && v.holding());
    assert(v.input(NUVIO_BTN_SQUARE, 0).kind == Act::None && v.holding()); // No remove while carrying.
    v.input(NUVIO_BTN_UP, 0);
    assert(v.cursor() == 2 && v.row(2)->title == "T3" && v.row(3)->title == "T2" && v.row(4)->title == "T4");
    v.input(NUVIO_BTN_UP, 0);
    assert(v.cursor() == 2); // Cannot be carried above the current track.
    auto action = v.input(NUVIO_BTN_CROSS, 1);
    assert(action.kind == Act::Move && action.uid == 1003 && action.anchor == 1001 && v.busy());
    // Until the worker confirms, the rehearsed order stays on screen and edits wait.
    assert(v.row(2)->title == "T3" && v.input(NUVIO_BTN_SQUARE, 1).kind == Act::None);
    assert(v.input(NUVIO_BTN_CROSS, 1).kind == Act::None);
    q.apply(action);
    v.sync(q.window(v.wanted_first()), 1.1);
    assert(!v.busy() && !v.holding() && v.cursor() == 2 && v.row(2)->title == "T3" && v.row(3)->title == "T2");
    // Carry down past two rows: anchor is the row that ends up above it.
    v.input(NUVIO_BTN_CROSS, 2);
    v.input(NUVIO_BTN_DOWN, 2); v.input(NUVIO_BTN_DOWN, 2);
    assert(v.cursor() == 4 && v.row(2)->title == "T2" && v.row(3)->title == "T4" && v.row(4)->title == "T3");
    action = v.input(NUVIO_BTN_CROSS, 2);
    assert(action.kind == Act::Move && action.uid == 1003 && action.anchor == 1004);
    q.apply(action); v.sync(q.window(v.wanted_first()), 2.1);
    assert(v.row(4)->title == "T3" && v.cursor() == 4);
    // Circle while carrying cancels the carry, not the view; dropping in place sends nothing.
    v.input(NUVIO_BTN_CROSS, 3); v.input(NUVIO_BTN_UP, 3);
    assert(v.input(NUVIO_BTN_CIRCLE, 3).kind == Act::None && v.visible() && !v.holding() && v.cursor() == 4);
    assert(v.row(4)->title == "T3" && v.row(3)->title == "T4");
    v.input(NUVIO_BTN_CROSS, 3);
    assert(v.input(NUVIO_BTN_CROSS, 3).kind == Act::None && !v.holding() && !v.busy());
    // Remove the focused upcoming row; focus stays on the slot.
    action = v.input(NUVIO_BTN_SQUARE, 4);
    assert(action.kind == Act::Remove && action.uid == 1003 && v.busy());
    q.apply(action); v.sync(q.window(v.wanted_first()), 4.1);
    assert(!v.busy() && v.total() == 5 && v.cursor() == 4 && v.row(4)->title == "T5");
    // A refused or lost edit: view recovers after the timeout instead of locking up.
    action = v.input(NUVIO_BTN_SQUARE, 5);
    assert(action.kind == Act::Remove && v.busy());
    v.sync(q.window(v.wanted_first()), 6); assert(!v.busy()); // New publication, not pending.
    action = v.input(NUVIO_BTN_SQUARE, 7);
    const auto same = q.window(v.wanted_first());
    v.sync(same, 7.5); assert(!v.busy());                       // Different serial clears it too.
    action = v.input(NUVIO_BTN_SQUARE, 8);
    assert(v.busy());
    q.pending = true; v.sync(q.window(v.wanted_first()), 20); assert(v.busy()); // Worker still waiting to swap.
    assert(v.input(NUVIO_BTN_CROSS, 20).kind == Act::None && !v.holding());
    q.pending = false; v.sync(q.window(v.wanted_first()), 21); assert(!v.busy());
    // Playback advancing under a carry that it overtakes cancels the carry.
    v.input(NUVIO_BTN_UP, 22); v.input(NUVIO_BTN_UP, 22);
    assert(v.cursor() == 2);
    v.input(NUVIO_BTN_CROSS, 22); assert(v.holding());
    q.position = 2; v.sync(q.window(v.wanted_first()), 23);
    assert(!v.holding() && v.cursor() == 2 && v.position() == 2);
    // The cursor follows its entry when the queue changes around it.
    v.input(NUVIO_BTN_DOWN, 24);
    const auto followed = v.row(v.cursor())->uid;
    q.all.insert(q.all.begin() + 3, {5000, "Next", "", 0}); // Play Next from elsewhere.
    v.sync(q.window(v.wanted_first()), 25);
    assert(v.row(v.cursor())->uid == followed && v.cursor() == 4);
    // A structural change under a carry cancels it rather than moving the wrong row.
    v.input(NUVIO_BTN_CROSS, 26); assert(v.holding());
    q.all.pop_back(); v.sync(q.window(v.wanted_first()), 27);
    assert(!v.holding());
    assert(v.input(NUVIO_BTN_CIRCLE, 28).kind == Act::Close && !v.visible());

    // 20000 entries: every window stays bounded, paging re-requests only near the
    // loaded edge, and a long carry is a single edit.
    auto big = queue_of(20000, 0);
    View long_view; long_view.open();
    long_view.sync(big.window(long_view.wanted_first()), 0);
    size_t requests = 0, wanted = long_view.wanted_first();
    long_view.input(NUVIO_BTN_DOWN, 0);
    long_view.input(NUVIO_BTN_CROSS, 0); // Carry T1.
    assert(long_view.holding());
    size_t moves = 0;
    for (int i = 0; i < 2600; ++i) {
        long_view.input(NUVIO_BTN_R1, 0);
        if (long_view.wanted_first() != wanted) { wanted = long_view.wanted_first(); ++requests; }
        long_view.sync(big.window(wanted), 0);
        const auto *at = long_view.row(long_view.cursor());
        assert(at && at->title == "T1"); // Carried row is always drawable.
        if (long_view.cursor() > 1) assert(long_view.row(long_view.cursor() - 1) != nullptr);
    }
    assert(long_view.cursor() == 19999 && long_view.row(19998)->title == "T19999");
    action = long_view.input(NUVIO_BTN_CROSS, 1);
    if (action.kind == Act::Move) ++moves;
    assert(moves == 1 && action.uid == 1001 && action.anchor == 1000 + 19999);
    assert(big.largest <= QueueWindow::capacity && requests < 2600 / 2);
    big.apply(action);
    long_view.sync(big.window(long_view.wanted_first()), 1.1);
    assert(big.all.back().uid == 1001 && long_view.row(19999)->uid == 1001 && long_view.cursor() == 19999);
    // Paging back to the top lands on the current entry, never above it.
    for (int i = 0; i < 2600; ++i) { long_view.input(NUVIO_BTN_L1, 2); long_view.sync(big.window(long_view.wanted_first()), 2); }
    assert(long_view.cursor() == 0 && long_view.row(0)->title == "T0");

    std::cout << "PASS: v1 button routing, logout menu, login focus, control row, queue view carry/remove and 20000-entry window\n";
}
