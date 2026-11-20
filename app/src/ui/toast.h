/* Navi5 — where short confirmations (Play Next, queue failures) are drawn.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pure layout on the 1920x1080 logical canvas, no gfx. Browse puts the glass
 * pill right-aligned under the header row (tabs, Search, gear: y 58..134), so
 * it never covers a header control; the fullscreen player has no header and
 * its queue panel starts at y 110, so the pill sits above that instead.
 */
#pragma once
#include <algorithm>
#include <cmath>

namespace ui {
struct ToastBox { float x, y, w, h; };
constexpr float kToastRight = 1820;  // Gear's right edge.
constexpr float kToastPad = 28, kToastHeight = 56, kToastMaxText = 760;
constexpr float kHeaderBottom = 134; // Header controls are 76 tall from y 58.
inline ToastBox toast_box(float text_width, bool fullscreen) {
    const float text = std::isfinite(text_width) ? std::clamp(text_width, 0.f, kToastMaxText) : kToastMaxText;
    const float w = text + 2 * kToastPad;
    return {kToastRight - w, fullscreen ? 40.f : kHeaderBottom + 16, w, kToastHeight};
}
} // namespace ui
