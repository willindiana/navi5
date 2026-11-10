/* Navi5 — bounded, account-scoped explicit play history.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include "ui/music_state.h"

namespace app {
class RecentContexts {
public:
    static constexpr size_t limit = 6;
    // Empty scope means RAM only (API-key identity cannot safely be inferred).
    static std::string scope(const std::string &server, const std::string &user);
    explicit RecentContexts(std::string scope = {}, const char *path = "/download0/navi5/recent-contexts.json");
    void promote(ui::music::Item context);
    bool save() const;
    const std::vector<ui::music::Item> &items() const { return items_; }
private:
    std::string scope_, path_;
    std::vector<ui::music::Item> items_; // artwork_url holds cover-art ID, never authenticated URL.
};
}
