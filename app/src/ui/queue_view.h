/* Navi5 — queue view over the fullscreen player: current track and what follows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ui/music_state.h"
#include "ui/screen.h"
#include "nuvio_input.h"
#include <algorithm>
#include <optional>

namespace ui {
// UI thread only, no I/O. The worker owns the queue and publishes a bounded
// window (music::QueueWindow); this view never holds more than that, whatever
// the queue length. A move is rehearsed locally and sent once, on drop, so a
// long carry costs one worker edit and at most one successor swap.
class QueueView {
public:
    struct Action {
        enum Kind { None, Close, Move, Remove } kind = None;
        uint64_t uid = 0, anchor = 0; // Move: place uid directly after anchor.
    };
    static constexpr size_t visible_rows = 9, page = 8;
    bool visible() const { return visible_; }
    void open() { *this = QueueView{}; visible_ = true; }
    void close() { visible_ = false; held_.reset(); busy_ = false; }
    // Latest published window; cheap when the serial is unchanged.
    void sync(const music::QueueWindow &window, double now);
    Action input(uint32_t pressed, double now);
    // Play-order position the worker should publish from.
    size_t wanted_first() const { return wanted_; }
    void draw(float dt);

    // Display model (also for tests). Indices are play-order positions as they
    // will be after the rehearsed move; position() is the fixed current entry.
    size_t position() const { return window_.position; }
    size_t total() const { return window_.total; }
    size_t cursor() const { return cursor_; }
    bool holding() const { return held_.has_value() && !busy_; }
    bool busy() const { return busy_ || window_.pending; }
    // Row shown at a display index; null while that part is not loaded.
    const music::QueueRow *row(size_t display) const;
private:
    struct Held { music::QueueRow row; size_t from = 0; };
    const music::QueueRow *source(size_t index) const {
        return index >= window_.first && index - window_.first < window_.rows.size()
            ? &window_.rows[index - window_.first] : nullptr;
    }
    void want();
    music::QueueWindow window_;
    std::optional<Held> held_;
    size_t cursor_ = 0, wanted_ = 0, held_total_ = 0;
    uint64_t cursor_uid_ = 0, busy_serial_ = 0;
    double busy_until_ = 0;
    bool visible_ = false, busy_ = false, placed_ = false;
    Drop drop_;
};

inline const music::QueueRow *QueueView::row(size_t display) const
{
    if (display < window_.position || display >= window_.total) return nullptr;
    if (!held_) return source(display);
    const size_t from = held_->from, to = cursor_;
    if (display == to) return &held_->row;
    if (from < to && display >= from && display < to) return source(display + 1);
    if (from > to && display > to && display <= from) return source(display - 1);
    return source(display);
}
inline void QueueView::want()
{
    // Re-request only near the loaded edge; one window spans several screens.
    const size_t first = window_.first, end = first + window_.rows.size();
    const bool above = cursor_ < first + page && first > window_.position;
    const bool below = cursor_ + page >= end && end < window_.total;
    if (above || below || cursor_ < first || cursor_ >= end)
        wanted_ = std::max(window_.position, cursor_ > 20 ? cursor_ - 20 : 0);
}
inline void QueueView::sync(const music::QueueWindow &window, double now)
{
    if (!visible_) return;
    if (window.serial != window_.serial) {
        window_ = window;
        if (busy_ && window_.serial != busy_serial_ && !window_.pending) { busy_ = false; held_.reset(); }
        // A carried entry is only meaningful against the queue it was picked from.
        if (held_ && !busy_ && (window_.total != held_total_ ||
                window_.position >= std::min(held_->from, cursor_))) held_.reset();
        if (!held_ && cursor_uid_) {
            for (size_t i = 0; i < window_.rows.size(); ++i)
                if (window_.rows[i].uid == cursor_uid_) { cursor_ = window_.first + i; break; }
        }
    }
    if (busy_ && now > busy_until_ && !window_.pending) { busy_ = false; held_.reset(); }
    if (!placed_ && window_.serial) { placed_ = true; cursor_ = window_.position; }
    const size_t last = window_.total ? window_.total - 1 : 0;
    cursor_ = std::clamp(cursor_, std::min(window_.position + (held_ ? 1 : 0), last), last);
    if (!held_) { const auto *at = source(cursor_); cursor_uid_ = at ? at->uid : 0; }
    want();
}
inline QueueView::Action QueueView::input(uint32_t pressed, double now)
{
    if (!visible_) return {};
    if (pressed & NUVIO_BTN_CIRCLE) {
        if (held_ && !busy_) { cursor_ = held_->from; held_.reset(); want(); return {}; }
        close();
        return {Action::Close};
    }
    const size_t last = window_.total ? window_.total - 1 : 0;
    const size_t top = std::min(window_.position + (held_ ? 1 : 0), last);
    size_t step = 0;
    bool up = false;
    if (pressed & NUVIO_BTN_UP) { step = 1; up = true; }
    else if (pressed & NUVIO_BTN_DOWN) step = 1;
    else if (pressed & NUVIO_BTN_L1) { step = page; up = true; }
    else if (pressed & NUVIO_BTN_R1) step = page;
    if (step && !busy_) {
        cursor_ = up ? (cursor_ > top + step ? cursor_ - step : top) : std::min(last, cursor_ + step);
        if (!held_) { const auto *at = source(cursor_); cursor_uid_ = at ? at->uid : 0; }
        want();
        return {};
    }
    if (busy() || !window_.total) return {}; // One edit at a time; worker confirms first.
    if (pressed & NUVIO_BTN_CROSS) {
        if (!held_) {
            const auto *at = source(cursor_);
            if (!at || cursor_ <= window_.position) return {}; // Current entry is fixed.
            held_ = Held{*at, cursor_}; held_total_ = window_.total;
            return {};
        }
        if (cursor_ == held_->from) { held_.reset(); return {}; }
        const auto *anchor = row(cursor_ - 1); // Never the held row: cursor_ > position.
        if (!anchor) return {};                // Neighbor not loaded yet: keep holding.
        Action action{Action::Move, held_->row.uid, anchor->uid};
        cursor_uid_ = held_->row.uid;
        busy_ = true; busy_serial_ = window_.serial; busy_until_ = now + 2;
        return action;
    }
    if ((pressed & NUVIO_BTN_SQUARE) && !held_) {
        const auto *at = source(cursor_);
        if (!at || cursor_ <= window_.position) return {};
        busy_ = true; busy_serial_ = window_.serial; busy_until_ = now + 2;
        return {Action::Remove, at->uid, 0};
    }
    return {};
}
} // namespace ui
