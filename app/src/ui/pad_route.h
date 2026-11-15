/* Navi5 — which global action a button press means while signed in.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plain C so the controller host test (input_test.c) checks the same table the
 * app uses. Pure: no state, no I/O.
 *
 *   Triangle  Play Next for the focused item (browse only)
 *   Touchpad  open the fullscreen player (never Stop)
 *   Options   menu (Log out)
 *   Square    search backspace in Search, remove in the queue view, else pause/resume
 *   Play/Pause (remote) pause/resume everywhere
 * Stop is a control in the fullscreen player, not a button.
 */
#pragma once
#include "nuvio_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_pad_context {
    int fullscreen;  /* fullscreen player owns input */
    int queue_open;  /* queue view over the fullscreen player */
    int menu_open;   /* Options menu or its confirmation */
    int search;      /* Search screen is the browse view */
    int has_song;    /* something is loaded to show in the player */
} ui_pad_context;

typedef struct ui_pad_route {
    int menu;             /* open the Options menu */
    int play_next;        /* enqueue the focused item */
    int open_player;      /* show the fullscreen player */
    int pause_toggle;     /* pause or resume */
    int search_backspace; /* delete one search character */
    uint32_t rest;        /* presses left for the focused view */
} ui_pad_route;

static inline ui_pad_route ui_route_pad(uint32_t pressed, ui_pad_context c)
{
    ui_pad_route r = {0, 0, 0, 0, 0, 0};
    const uint32_t global = NUVIO_BTN_OPTIONS | NUVIO_BTN_TRIANGLE | NUVIO_BTN_TOUCHPAD |
        NUVIO_BTN_SQUARE | NUVIO_BTN_PLAYPAUSE;
    if (pressed & NUVIO_BTN_PLAYPAUSE) r.pause_toggle = 1;
    if (c.menu_open) {
        /* The menu is modal: only it sees the pad, Options included (closes it). */
        r.rest = pressed & ~(global & ~NUVIO_BTN_OPTIONS);
        return r;
    }
    r.rest = pressed & ~global;
    if (pressed & NUVIO_BTN_OPTIONS) {
        r.menu = 1; /* Nothing else acts on the frame the menu opens. */
        r.rest = 0; r.pause_toggle = 0;
        return r;
    }
    if (pressed & NUVIO_BTN_SQUARE) {
        if (c.queue_open) r.rest |= NUVIO_BTN_SQUARE;
        else if (c.search && !c.fullscreen) r.search_backspace = 1;
        else r.pause_toggle = 1;
    }
    if ((pressed & NUVIO_BTN_TRIANGLE) && !c.fullscreen && !c.queue_open) r.play_next = 1;
    if ((pressed & NUVIO_BTN_TOUCHPAD) && c.has_song && !c.fullscreen) r.open_player = 1;
    return r;
}

#ifdef __cplusplus
}
#endif
