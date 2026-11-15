/* Navi5 — queue view rendering.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/queue_view.h"
#include "app/i18n.h"
#include <cstdio>

namespace ui {
void QueueView::draw(float dt)
{
    if (!visible_) return;
    gfx::fill({0, 0, gfx::W, gfx::H}, 0xa0000000);
    glass_panel({360, 110, 1200, 860}, 40);
    gfx::text(410, 186, T("Kø"), {gfx::Bold, 40}, kText);
    const size_t position = window_.position, total = window_.total;
    if (total > position) {
        char count[48];
        std::snprintf(count, sizeof count, "%zu / %zu", cursor_ - position + 1, total - position);
        gfx::text(1510, 186, count, {gfx::Medium, 24}, kText2, 2);
    }
    // Fixed row slots: only the visible rows are ever laid out or drawn.
    constexpr float top = 220, row_h = 72;
    const size_t span = total - std::min(total, position);
    size_t first = position;
    if (span > visible_rows) {
        const size_t offset = cursor_ - position;
        first = position + std::min(offset > visible_rows / 2 ? offset - visible_rows / 2 : 0, span - visible_rows);
    }
    bool animating = false;
    if (total > position) {
        drop_.to({384, top + float(cursor_ - first) * row_h + 3, 1152, row_h - 6}, int(cursor_ % 1024), 0, 0);
        drop_.draw(dt, 1, &animating, 20);
    } else {
        drop_.hide();
        gfx::text(960, 540, T("Køen er tom"), {gfx::Medium, 28, 1000}, kText2, 1);
    }
    for (size_t i = 0; i < visible_rows && first + i < total; ++i) {
        const size_t display = first + i;
        const float y = top + float(i) * row_h;
        const auto *entry = row(display);
        const bool current = display == position;
        const bool carried = held_ && display == cursor_;
        if (!entry) { gfx::text(430, y + 44, "…", {gfx::Medium, 26}, kText2); continue; }
        const float x = carried ? 470.f : 430.f; // Carried row is inset, not recolored.
        gfx::text(x, y + 32, entry->title.empty() ? T("Uten tittel") : entry->title,
                  {current ? gfx::Bold : gfx::SemiBold, 26, 1500 - x - 120}, kText);
        gfx::text(x, y + 59, current ? std::string(T("Spilles nå")) + (entry->artist.empty() ? "" : " · " + entry->artist) : entry->artist,
                  {gfx::Medium, 20, 1500 - x - 120}, kText2);
        if (entry->duration > 0) {
            char duration[32];
            std::snprintf(duration, sizeof duration, "%d:%02d", entry->duration / 60, entry->duration % 60);
            gfx::text(1500, y + 44, duration, {gfx::Medium, 22}, kText2, 2);
        }
    }
    if (busy()) gfx::text(1510, 930, T("Oppdaterer køen …"), {gfx::Medium, 22, 420}, kText2, 2);
    std::vector<PadHint> hints;
    if (held_ && !busy_) hints = {{PadButton::Cross, T("Slipp")}, {PadButton::Circle, T("Avbryt")}};
    else {
        if (cursor_ > position && !busy()) {
            hints.push_back({PadButton::Cross, T("Flytt")});
            hints.push_back({PadButton::Square, T("Fjern")});
        }
        hints.push_back({PadButton::Circle, T("Tilbake")});
    }
    draw_pad_hints(410, 924, hints, 0, 26);
}
} // namespace ui
