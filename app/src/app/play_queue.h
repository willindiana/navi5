/*
 * Navi5 — local play queue
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "subsonic/subsonic.h"
#include <cstddef>
#include <optional>
#include <random>
#include <vector>

namespace app {

enum class Repeat { off, all, one };

// Single-threaded model. IDs are opaque; duplicate songs remain distinct entries.
class PlayQueue {
public:
    explicit PlayQueue(std::vector<subsonic::Song> tracks = {});

    // Canonical playlist order, restored when shuffle is disabled.
    const std::vector<subsonic::Song> &tracks() const { return tracks_; }
    std::optional<std::size_t> current_index() const;
    // Pointer is invalidated by queue mutation; copy before retaining it.
    const subsonic::Song *current() const;
    // Truthful natural successor; repeat-one has no distinct next-track notice.
    const subsonic::Song *up_next() const;

    // Manual navigation ignores repeat-one. Only repeat-all wraps boundaries.
    // False means no destination; current remains unchanged (including at end).
    bool next();
    bool previous();
    // Natural completion honors repeat-one, returning true to replay current.
    bool on_track_end();
    // Exact canonical successor occurrence, without mutating playback position.
    std::optional<std::size_t> successor_index() const;

    Repeat repeat() const { return repeat_; }
    void set_repeat(Repeat repeat) { repeat_ = repeat; }
    bool shuffled() const { return shuffled_; }
    // Enabling starts a new order with current first, shuffling all other entries.
    // Disabling restores canonical order and the exact current occurrence.
    // Repeated setting is a no-op. RNG is caller-owned, used only during this call.
    void set_shuffle(bool enabled, std::mt19937 &rng);
    // Live toggle retains visited prefix/current; only unplayed suffix changes.
    // OFF orders remaining occurrences canonically from current's successor, then
    // wraps to earlier unplayed ones. History is neither replayed nor skipped.
    void set_live_shuffle(bool enabled, std::mt19937 &rng);

    // Append goes to both tails, even when shuffled. Insert-next goes immediately
    // after current in both orders. Either operation selects first if empty.
    void append(subsonic::Song track);
    void insert_next(subsonic::Song track);

private:
    std::vector<subsonic::Song> tracks_;
    std::vector<std::size_t> order_;
    std::size_t position_ = 0;
    Repeat repeat_ = Repeat::off;
    bool shuffled_ = false;
};

} // namespace app
