/*
 * Navi5 — local play queue
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "subsonic/subsonic.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace app {

enum class Repeat { off, all, one };

// Single-threaded model. IDs are opaque; duplicate songs remain distinct entries.
class PlayQueue {
public:
    // Hard cap on retained occurrences, shared with artist assembly.
    static constexpr std::size_t max_tracks = 20000;
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

    // Play order (what will be heard), not canonical order. position() is the
    // current entry; entries before it are history. uid is unique across every
    // queue of this process and survives reordering, so stale edits never match.
    std::size_t size() const { return order_.size(); }
    std::size_t position() const { return position_; }
    const subsonic::Song &at(std::size_t order_position) const { return tracks_[order_[order_position]]; }
    uint64_t uid_at(std::size_t order_position) const { return ids_[order_[order_position]]; }
    std::optional<uint64_t> successor_uid() const;
    // Changes whenever content or order changes; equal values mean equal queues.
    uint64_t revision() const { return revision_; }

    // Play Next: inserts after the entries already played-next (pinned), so
    // repeated calls keep insertion order. Each advance consumes one pinned
    // entry; going back or wrapping forgets the rest. An empty queue selects the
    // first inserted entry. False adds nothing: empty input or cap exceeded.
    bool play_next(std::vector<subsonic::Song> tracks);
    std::size_t pinned() const { return pinned_; }
    // Reorder/remove upcoming entries only. uid must be after current; anchor is
    // the entry it will follow (current allowed). False changes nothing.
    // Without shuffle the new order also becomes the canonical order.
    bool move_after(uint64_t uid, uint64_t anchor);
    bool remove(uint64_t uid);

private:
    std::vector<subsonic::Song> tracks_;
    std::vector<uint64_t> ids_; // Parallel to tracks_.
    std::vector<std::size_t> order_;
    std::size_t position_ = 0, pinned_ = 0;
    uint64_t revision_ = 0;
    void touch();
    std::optional<std::size_t> find(uint64_t uid, std::size_t from) const;
    Repeat repeat_ = Repeat::off;
    bool shuffled_ = false;
};

} // namespace app
