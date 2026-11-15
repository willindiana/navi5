/* Navi5 — fullscreen presentation state; browsing remains untouched.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace ui {
struct PlayerState {
    bool visible = false;
    int focus = 1;
    double activity = 0;
    bool preview = false, seek_error = false;
    double target = 0;
    uint64_t revision = 0, track = 0;
    static bool seekable(double duration, double position, bool active, bool busy) {
        return active && !busy && std::isfinite(duration) && duration > 0 &&
            std::isfinite(position) && position >= 0 && position < duration;
    }
    void sync(uint64_t r, uint64_t t, bool active) {
        if (r != revision || t != track || !active) { preview = seek_error = false; if (focus == 3) focus = 1; }
        revision = r; track = t;
    }
    void open(double now) { visible = true; activity = now; preview = seek_error = false; }
    void dismiss() { visible = false; preview = false; }
    void adjust(int delta, double position, double duration) {
        target = std::clamp((preview ? target : position) + delta * 10.0, 0.0,
                            std::max(0.0, duration - 1.0 / 48000));
        preview = true; seek_error = false;
    }
    bool controls(double now) const { return now - activity < 5; }
    void navigate(int horizontal, int vertical, double now, bool available, double position, double duration) {
        const bool awake = controls(now);
        activity = now;
        if (!awake) return;
        if (vertical) { focus = vertical > 0 ? 3 : 1; preview = false; }
        else if (focus == 3) { if (available) adjust(horizontal, position, duration); }
        else direction(horizontal, now);
    }
    // Caller sends a safe worker toggle only on true; snapshot owns enabled/pending.
    bool toggle_shuffle(double now, bool pending) {
        const bool toggle = visible && controls(now) && focus == 4 && !pending;
        activity = now;
        return toggle;
    }
    // First direction after idle wakes, never changes focus.
    // Control row, left to right: previous, play/pause, next, shuffle, stop, queue
    // (focus 0, 1, 2, 4, 5, 6). Focus 3 is the seek bar below the row.
    static constexpr int row[] = {0, 1, 2, 4, 5, 6};
    static constexpr int row_size = 6;
    static int slot(int focus) {
        for (int i = 0; i < row_size; ++i) if (row[i] == focus) return i;
        return 1;
    }
    void direction(int delta, double now) {
        const bool awake = controls(now);
        activity = now;
        if (awake) focus = row[std::clamp(slot(focus) + (delta < 0 ? -1 : 1), 0, row_size - 1)];
    }
};
inline bool show_up_next(double position, int duration, bool has_next, bool repeat_one, bool playing) {
    return playing && has_next && !repeat_one && duration > 0 && std::isfinite(position) &&
        position >= 0 && position >= duration - 10.0 && position < duration;
}
} // namespace ui
