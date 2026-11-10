/* Navi5 — offline pairing view state and rendering checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// Renderer stubs intentionally do not touch Drop's GPU-animation fields.
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#include "ui/pairing_view.h"
#include "app/pairing_flow.h"
#include "nuvio_input.h"
#include <cassert>
#include <algorithm>
#include <limits>
#include <vector>

namespace {
std::vector<std::string> texts;
unsigned white = 0, black = 0;
}
const char *T(const char *key) { return key; }
namespace gfx {
float text(float, float, const std::string &s, const TextStyle &, uint32_t, int)
{ texts.push_back(s); return 0; }
void fill(const Rect &r, uint32_t color, float)
{
    assert(r.w > 0 && r.h > 0);
    assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H);
    if (color == 0xffffffff) ++white;
    if (color == 0xff000000) ++black;
}
}
namespace ui {
void glass_panel(const gfx::Rect &, float, float, bool, float) {}
void Drop::to(const gfx::Rect &, int, float, float) {}
void Drop::draw(float, float, bool *, float) {}
}
int main()
{
    using V = ui::PairingView;
    V v;
    assert(!v.show_code(v.generation(), "http://127.0.0.1/#test", 120, 0));
    assert(v.state() == V::State::Ready);
    v.draw(0, 0.016f);
    assert(std::find(texts.begin(), texts.end(),
        "HTTP-musikkservere sender innlogging og musikk ukryptert. Kun betrodd lokalnett.") != texts.end());
    texts.clear();
    assert(v.input(0, 0) == V::Action::None);
    assert(v.input(NUVIO_BTN_CROSS, 0) == V::Action::Start);
    const auto first = v.generation();
    assert(v.input(NUVIO_BTN_CROSS, 0) == V::Action::None);
    assert(v.show_code(first, "http://127.0.0.1:8000/#test", 120, 0));
    v.draw(1, 0.016f);
    assert(white == 1 && black > 0);
    for (const auto &s : texts) assert(s.find("http://") == std::string::npos);
    assert(std::find(texts.begin(), texts.end(), "HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett.") != texts.end());
    white = black = 0;
    v.draw(120, 0.016f);
    assert(v.state() == V::State::Expired && white == 0 && black == 0);
    assert(v.input(NUVIO_BTN_CROSS, 120) == V::Action::Cancel); // Expiry cancel drains first.
    assert(v.input(NUVIO_BTN_CROSS, 120) == V::Action::Retry);  // No TV consent step.
    assert(v.state() == V::State::Starting);
    assert(!v.show_code(first, "http://127.0.0.1/#old", 240, 120));
    v.complete(first, V::State::Success, 120);
    assert(v.state() == V::State::Starting);
    assert(!v.show_code(v.generation(), std::string("http://x/\0hidden", 16), 240, 120));
    assert(v.state() == V::State::Failed);
    assert(v.diagnostic().stage == pairing::Stage::QrInput);
    assert(v.input(0, 120) == V::Action::Cancel);
    assert(v.input(NUVIO_BTN_CROSS, 120) == V::Action::Retry);
    assert(v.show_code(v.generation(), "http://127.0.0.1/#new", 240, 120));
    v.complete(v.generation(), V::State::Checking, 121);
    texts.clear(); v.draw(121, 0.016f);
    assert(white == 0 && black == 0);
    assert(std::find(texts.begin(), texts.end(), "HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett.") != texts.end());
    assert(v.input(0, std::numeric_limits<double>::quiet_NaN()) == V::Action::Cancel);
    assert(v.state() == V::State::Expired);
    assert(v.input(NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE, 121) == V::Action::Cancel);
    assert(v.state() == V::State::Closed);
    v.reset();
    assert(v.state() == V::State::Ready);
    v.input(NUVIO_BTN_CROSS, 0);
    v.complete(v.generation(), V::State::Success, 1);
    assert(v.state() == V::State::Success);

    // Auto-start (one discovered address) skips Ready entirely.
    v.start();
    assert(v.state() == V::State::Starting && v.address().empty());
    assert(v.show_code(v.generation(), "http://127.0.0.1/#auto", 120, 0));

    // Picker: up/down choose, Cross starts chosen address, clamped at ends.
    v.reset(); v.choose({"192.168.1.5", "10.0.0.7"});
    assert(v.address() == "192.168.1.5");
    v.input(NUVIO_BTN_UP, 0); assert(v.address() == "192.168.1.5");
    v.input(NUVIO_BTN_DOWN, 0); v.input(NUVIO_BTN_DOWN, 0); assert(v.address() == "10.0.0.7");
    assert(v.input(NUVIO_BTN_CROSS, 0) == V::Action::Start && v.address() == "10.0.0.7");
    v.reset(); assert(v.address().empty());

    // Each failed/expired retry invalidates old work; stale results ignored.
    for (auto failure : {V::State::Failed, V::State::Expired}) {
        v.start();
        const auto old_generation = v.generation();
        assert(v.show_code(old_generation, "http://127.0.0.1/#retry", 120, 0));
        if (failure == V::State::Failed) v.complete(old_generation, failure, 1);
        else {
            assert(v.input(0, 120) == V::Action::Cancel);
            assert(v.state() == V::State::Expired);
        }
        assert(v.input(NUVIO_BTN_CROSS, 120) == V::Action::Retry);
        assert(v.state() == V::State::Starting);
        assert(v.generation() > old_generation);
        v.complete(old_generation, V::State::Success, 120);
        assert(!v.show_code(old_generation, "http://127.0.0.1/#stale", 240, 120));
        assert(v.state() == V::State::Starting);
        assert(v.input(0, 120) == V::Action::None);
    }

    // Actual main orchestration seam: no socket, keyboard or scheduler required.
    app::PairingFlow flow;
    auto start = [&] {
        v.reset(); assert(v.input(NUVIO_BTN_CROSS, 0) == V::Action::Start);
        assert(flow.begin(v, false, 0));
    };
    start();
    assert(!v.show_code(v.generation(), "http://" + std::string(500, 'x'), 120, 0));
    assert(v.diagnostic().stage == pairing::Stage::QrEncode);
    v.input(0, 0); // QR failure cancels worker without replacing cause.
    v.diagnose(flow.generation, {pairing::Stage::Poll, 5});
    assert(v.diagnostic().stage == pairing::Stage::QrEncode);
    int sends = 0;
    auto send = [&](pairing::Credentials c) { ++sends; assert(c.api_key == "fixture"); return false; };
    std::optional<pairing::Credentials> credentials;
    const pairing::Session::Ready ready{"http://127.0.0.1/#fixture",
        pairing::Session::Clock::time_point(std::chrono::seconds(120))};
    start(); flow.address_closed(v, true, 0);
    assert(v.state() == V::State::Starting);
    flow.address_closed(v, false, 0); // IME cancel or failed open, no callback.
    assert(v.state() == V::State::Failed && !flow.address_pending);
    assert(v.diagnostic().stage == pairing::Stage::ImeNoCallback);
    v.diagnose(flow.generation, {pairing::Stage::WorkerSpawn, 5});
    assert(v.diagnostic().stage == pairing::Stage::ImeNoCallback && v.diagnostic().error == 0);
    texts.clear(); v.draw(0, 0.016f);
#ifdef NAVI5_DEV_TOOLS
    assert(std::find(texts.begin(), texts.end(), "1 / 0") != texts.end());
#else
    assert(std::find(texts.begin(), texts.end(), "1 / 0") == texts.end());
#endif
    start(); flow.address_closed(v, false, 0, true);
    assert(v.state() == V::State::Closed && v.diagnostic().stage == pairing::Stage::None);
    v.diagnose(flow.generation, {pairing::Stage::ImeNoCallback, 0});
    assert(v.diagnostic().stage == pairing::Stage::None);
    start(); flow.address_pending = false;
    assert(!flow.update(v, {}, false, true, credentials, 1, send)); // Spawn failure.
    assert(v.state() == V::State::Failed);
    v.diagnose(flow.generation, {pairing::Stage::WorkerSpawn, 0});
    assert(v.diagnostic().stage == pairing::Stage::WorkerSpawn);
    start();
    assert(!flow.update(v, {}, true, false, credentials, 1, send)); // Checking before ready.
    assert(v.state() == V::State::Starting);
    assert(!flow.update(v, ready, true, false, credentials, 2, send));
    assert(v.state() == V::State::Waiting);
    white = black = 0; v.draw(2, 0.016f); assert(white > 0 && black > 0);
    assert(!flow.update(v, ready, false, false, credentials, 2.5, send));
    assert(v.state() == V::State::Waiting);
    credentials = pairing::Credentials{"", "", "", "fixture"};
    auto refused = flow.update(v, {}, true, true, credentials, 3, send);
    assert(refused && !*refused && sends == 1 && !credentials); // Mailbox failure distinguished.
    start();
    credentials = pairing::Credentials{"", "", "", "fixture"};
    assert(!flow.update(v, {}, true, true, credentials, 1, send)); // Finished before ready fails closed.
    assert(v.state() == V::State::Failed && sends == 1);
    start(); flow.update(v, ready, false, false, credentials, 1, send);
    v.input(NUVIO_BTN_CIRCLE, 2); // Cancel wins over concurrent accepted publication.
    credentials = pairing::Credentials{"", "", "", "fixture"};
    assert(!flow.update(v, {}, true, true, credentials, 2, send));
    assert(sends == 1 && !credentials && v.state() == V::State::Closed);
    start();
    assert(!flow.begin(v, true, 1)); // Old worker must drain before retry starts new listener.
    assert(v.state() == V::State::Failed);
    start(); // Drained worker permits fresh attempt.
    credentials = pairing::Credentials{"", "", "", "fixture"};
    auto simultaneous = flow.update(v, ready, true, true, credentials, 1, send);
    assert(simultaneous && !*simultaneous && sends == 2);
    start(); flow.update(v, ready, false, false, credentials, 1, send);
    credentials = pairing::Credentials{"", "", "", "fixture"};
    assert(!flow.update(v, {}, true, true, credentials, 120, send));
    assert(v.state() == V::State::Expired && sends == 2 && !credentials);

    // Auto-start: known address, no TV consent, no IME; QR straight from listener snapshot.
    v.start();
    assert(flow.begin(v, false, 0, false) && !flow.address_pending);
    flow.address_closed(v, false, 0); // No keyboard pending: must not fail.
    assert(v.state() == V::State::Starting);
    assert(!flow.update(v, ready, false, false, credentials, 1, send));
    assert(v.state() == V::State::Waiting);
    // Restart budget: two fresh sessions after expiry, then stop at Expired for Cross.
    assert(flow.restarts == 2);
    assert(!flow.auto_restart(v, false)); // Not expired yet.
    double t = 120;
    for (int left : {1, 0}) {
        assert(v.input(0, t) == V::Action::Cancel && v.state() == V::State::Expired);
        assert(!flow.auto_restart(v, true)); // Old worker still draining.
        const auto stale = flow.generation;
        assert(flow.auto_restart(v, false) && flow.restarts == left);
        assert(v.state() == V::State::Starting && v.generation() != stale);
        assert(flow.begin(v, false, t, false));
        assert(!v.show_code(stale, "http://127.0.0.1/#stale", t + 120, t)); // Old QR ignored.
        v.complete(stale, V::State::Failed, t);
        assert(v.state() == V::State::Starting);
        assert(v.show_code(flow.generation, "http://127.0.0.1/#fresh", t + 120, t));
        t += 120;
    }
    assert(v.input(0, t) == V::Action::Cancel && v.state() == V::State::Expired);
    assert(!flow.auto_restart(v, false) && flow.restarts == 0 && v.state() == V::State::Expired);
    assert(v.input(NUVIO_BTN_CROSS, t) == V::Action::Retry); // Manual retry still works.

    // Worker callbacks can arrive before frame input/draw observes expiry.
    for (double callback_time : {120.0, 121.0}) {
        for (auto terminal : {V::State::Success, V::State::Failed}) {
            v.start();
            const auto generation = v.generation();
            assert(v.show_code(generation, "http://127.0.0.1/#expiry", 120, 0));
            if (callback_time > 120) v.complete(generation, V::State::Checking, 1);
            v.complete(generation, terminal, callback_time);
            assert(v.state() == V::State::Expired);
            assert(v.generation() == generation + 1);
            v.complete(generation, V::State::Success, callback_time);
            assert(v.state() == V::State::Expired);
            assert(v.generation() == generation + 1);
            assert(v.input(0, callback_time) == V::Action::Cancel);
            assert(v.input(0, callback_time) == V::Action::None);
            assert(v.generation() == generation + 1);
        }
    }
}
