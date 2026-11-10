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
void PairingView::draw(double now, float dt)
{
    expire(now);
    if (state_ == State::Closed) return;
    animating_ = false;
    glass_panel({260, 130, 1400, 820}, 48);
    gfx::text(960, 220, T("Koble til med QR-kode"), {gfx::Bold, 44}, kText, 1);
    if (state_ == State::Ready) {
        gfx::text(960, 340, T("HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett."),
                  {gfx::Bold, 29, 1240, 2}, kText, 1);
        gfx::text(960, 445, T("Andre på nettverket kan stjele innloggingen. Ikke åpne porter mot internett."),
                  {gfx::Medium, 27, 1160, 2}, kText, 1);
        gfx::text(960, 550, T("HTTP-musikkservere sender innlogging og musikk ukryptert. Kun betrodd lokalnett."),
                  {gfx::Medium, 27, 1160, 2}, kText2, 1);
        for (size_t i = 0; i < choices_.size(); ++i)
            gfx::text(960, 640.f + i * 34, (i == choice_ ? "> " : "") + choices_[i], {gfx::Medium, 27}, kText, 1);
    } else if (state_ == State::Waiting) {
        const int size = qrcodegen_getSize(qr_.data());
        // Integer module size, four-module quiet zone, opaque high contrast.
        const float module = std::floor(400.f / (size + 8));
        const float side = (size + 8) * module, left = 960 - side / 2;
        gfx::fill({left, 280, side, side}, 0xffffffff);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (qrcodegen_getModule(qr_.data(), x, y))
                    gfx::fill({left + (x + 4) * module, 280 + (y + 4) * module, module, module}, 0xff000000);
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, " %.0f", std::ceil(std::min(9999.0, deadline_ - now)));
        gfx::text(960, 730, std::string(T("Utløper om (sekunder):")) + seconds,
                  {gfx::Medium, 27}, kText, 1);
        gfx::text(960, 800, T("HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett."), {gfx::Medium, 25, 1240}, kText2, 1);
    } else {
        const char *message = T("Starter paring …");
        switch (state_) {
        case State::Checking: message = T("Kontrollerer innlogging …"); break;
        case State::Success: message = T("Paring fullført."); break;
        case State::Expired: message = T("Paring utløpt."); break;
        case State::Failed: message = T("Paring mislyktes."); break;
        default: break;
        }
        gfx::text(960, 500, message, {gfx::SemiBold, 34, 1200}, kText, 1);
        if (state_ == State::Checking)
            gfx::text(960, 800, T("HTTP-paring er ukryptert. Bruk kun et betrodd lokalnett."), {gfx::Medium, 25, 1240}, kText2, 1);
#ifdef NAVI5_DEV_TOOLS
        if (state_ == State::Failed && diagnostic_.stage != pairing::Stage::None) {
            char code[64];
            std::snprintf(code, sizeof code, "%u / %d", static_cast<unsigned>(diagnostic_.stage), diagnostic_.error);
            gfx::text(960, 570, code, {gfx::Medium, 27}, kText2, 1);
        }
#endif
    }
    const bool button = state_ == State::Ready || state_ == State::Failed || state_ == State::Expired;
    if (button) drop_.to({560, 775, 800, 70}, static_cast<int>(state_));
    else drop_.hide();
    drop_.draw(dt, 1, &animating_, 24);
    if (button) gfx::text(960, 822, state_ == State::Ready ? T("Cross: start paring") : T("Cross: prøv igjen"),
        {gfx::Bold, 27}, kText, 1);
    gfx::text(960, 895, T("Circle: avbryt paring"), {gfx::Medium, 25}, kText2, 1);
}
} // namespace ui
