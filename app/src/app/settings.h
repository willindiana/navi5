/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Local language, UI refresh and audio settings. No server preferences.
 */
#pragma once



namespace settings {

struct Local {
    int language = 0;            /* i18n::Choice: 0 follow the PS5, 1 Norsk, 2 English */
    bool refresh_120 = true;
    int buffer_s = 30;           /* how far ahead a network stream reads (settings::kBufferChoices); memory caps it too */
    bool night_mode = false;     /* compress loud and quiet together, dialogue lifted */
    bool check_updates = false;  /* opt-in: ask GitHub for a newer release at start */
};

/* Innstillinger: Buffer, in seconds. */
constexpr int kBufferChoices[] = {30, 60, 120, 300};

struct All {
    Local local;
};

/* The current settings (thread-safe copy). */
All get();
void load_local();
/* Saves console settings atomically. */
void set_local(const Local &l);
} // namespace settings
