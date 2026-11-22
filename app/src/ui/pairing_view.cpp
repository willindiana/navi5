/* Navi5 — temporary trusted-LAN pairing sheet.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/pairing_view.h"
#include "app/i18n.h"
#include "nuvio_input.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
void PairingView::clear_code()
{
    qr_.fill(0);
    deadline_ = 0;
}
void PairingView::reset()
{
    ++generation_;
    clear_code();
    diagnostic_ = {};
    choices_.clear();
    choice_ = 0;
    cancel_pending_ = false;
    state_ = State::Ready;
    drop_.hide();
}
void PairingView::expire(double now)
{
    if ((state_ == State::Waiting || state_ == State::Checking) &&
        (!std::isfinite(now) || now >= deadline_)) {
        clear_code();
        ++generation_;
        state_ = State::Expired;
        cancel_pending_ = true;
    }
}
PairingView::Action PairingView::input(uint32_t pressed, double now)
{
    expire(now);
    if (pressed & NUVIO_BTN_CIRCLE) {
        if (state_ == State::Closed) return Action::None;
        ++generation_;
        clear_code();
        cancel_pending_ = false;
        state_ = State::Closed;
        return Action::Cancel;
    }
    if (cancel_pending_) { cancel_pending_ = false; return Action::Cancel; }
    if (state_ == State::Ready && !choices_.empty()) {
        if ((pressed & NUVIO_BTN_UP) && choice_ > 0) --choice_;
        if ((pressed & NUVIO_BTN_DOWN) && choice_ + 1 < choices_.size()) ++choice_;
    }
    if (!(pressed & NUVIO_BTN_CROSS)) return Action::None;
    if (state_ == State::Failed || state_ == State::Expired) {
        start(); // Invalidates old worker generation; caller starts a fresh session.
        return Action::Retry;
    }
    if (state_ == State::Ready) {
        ++generation_;
        clear_code();
        state_ = State::Starting;
        return Action::Start;
    }
    return Action::None;
}
bool PairingView::show_code(uint64_t generation, const std::string &url, double deadline, double now)
{
    if (generation != generation_ || state_ != State::Starting) return false;
    // Caller supplies only reviewed listener's numeric private-LAN HTTP URL.
    // Bound encoding work and reject text that could truncate or hide its payload.
    const bool valid = std::isfinite(now) && std::isfinite(deadline) && deadline > now &&
        url.size() <= 512 && url.compare(0, 7, "http://") == 0 && url.size() > 7 &&
        std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c < 127; });
    std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(10)> scratch{};
    if (!valid || !qrcodegen_encodeText(url.c_str(), scratch.data(), qr_.data(),
            qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) {
        clear_code();
        state_ = State::Failed;
        diagnostic_.record(valid ? pairing::Stage::QrEncode : pairing::Stage::QrInput);
        cancel_pending_ = true;
        return false;
    }
    deadline_ = deadline;
    state_ = State::Waiting;
    return true;
}
void PairingView::complete(uint64_t generation, State result, double now)
{
    expire(now);
    if (generation != generation_ || (state_ != State::Starting &&
        state_ != State::Waiting && state_ != State::Checking)) return;
    if (result == State::Checking && state_ == State::Waiting) {
        qr_.fill(0);
        state_ = result;
    } else if (result == State::Success || result == State::Failed) {
        clear_code();
        state_ = result;
    }
}
void PairingView::draw(double now, float dt, float cx, bool focused)
{
    expire(now);
    animating_ = false;
    const char *warning = T("HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett.");
    gfx::text(cx, 285, T("Koble til med QR-kode"), {gfx::SemiBold, 30, 700}, kText, 1);
    if (state_ == State::Ready) {
        // Shown before a listener is started by hand (picker or typed address).
        gfx::text_centered(cx, 345, warning, {gfx::Bold, 23, 700, 3}, kText);
        gfx::text_centered(cx, 445, T("Andre på nettverket kan stjele innloggingen. Ikke åpne porter mot internett."),
                  {gfx::Medium, 22, 700, 3}, kText);
        gfx::text_centered(cx, 545, T("HTTP-musikkservere sender innlogging og musikk ukryptert. Kun betrodd lokalnett."),
                  {gfx::Medium, 22, 700, 3}, kText2);
        for (size_t i = 0; i < choices_.size(); ++i)
            gfx::text(cx, 650.f + i * 34, (i == choice_ ? "> " : "") + choices_[i], {gfx::Medium, 27}, kText, 1);
    } else if (state_ == State::Waiting) {
        const int size = qrcodegen_getSize(qr_.data());
        // Integer module size, four-module quiet zone, opaque high contrast.
        const float module = std::floor(400.f / (size + 8));
        const float side = (size + 8) * module, left = cx - side / 2;
        gfx::fill({left, 310, side, side}, 0xffffffff);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (qrcodegen_getModule(qr_.data(), x, y))
                    gfx::fill({left + (x + 4) * module, 310 + (y + 4) * module, module, module}, 0xff000000);
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, " %.0f", std::ceil(std::min(9999.0, deadline_ - now)));
        gfx::text(cx, 748, std::string(T("Utløper om (sekunder):")) + seconds,
                  {gfx::Medium, 24}, kText, 1);
        gfx::text_centered(cx, 784, warning, {gfx::Medium, 22, 700, 2}, kText2);
    } else {
        const char *message = T("Starter paring …");
        switch (state_) {
        case State::Checking: message = T("Kontrollerer innlogging …"); break;
        case State::Success: message = T("Paring fullført."); break;
        case State::Expired: message = T("Paring utløpt."); break;
        case State::Failed: message = T("Paring mislyktes."); break;
        case State::Closed: message = T("QR-paring er stoppet."); break;
        default: break;
        }
        gfx::text_centered(cx, 500, message, {gfx::SemiBold, 30, 700, 2}, kText);
        if (state_ == State::Checking)
            gfx::text_centered(cx, 784, warning, {gfx::Medium, 22, 700, 2}, kText2);
#ifdef NAVI5_DEV_TOOLS
        if (state_ == State::Failed && diagnostic_.stage != pairing::Stage::None) {
            char code[64];
            std::snprintf(code, sizeof code, "%u / %d", static_cast<unsigned>(diagnostic_.stage), diagnostic_.error);
            gfx::text(cx, 570, code, {gfx::Medium, 27}, kText2, 1);
        }
#endif
    }
    const bool button = state_ == State::Ready || state_ == State::Failed || state_ == State::Expired ||
        state_ == State::Closed;
    if (button && focused) drop_.to({cx - 300, 752, 600, 60}, static_cast<int>(state_));
    else drop_.hide();
    drop_.draw(dt, 1, &animating_, 24);
    // Button glyphs like every other hint row, never the words "Cross"/"Circle".
    if (button) draw_pad_hints(cx, 782, {{PadButton::Cross, state_ == State::Failed || state_ == State::Expired ?
        T("Prøv igjen") : T("Start paring")}}, 1, 28, focused ? 1.f : .7f);
    if (focused && state_ != State::Closed) draw_pad_hints(cx, 830, {{PadButton::Circle, T("Avbryt paring")}}, 1, 24, .8f);
}
} // namespace ui
