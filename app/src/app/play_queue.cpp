/*
 * Navi5 — local play queue
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "play_queue.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <numeric>
#include <utility>

namespace app {

namespace {
// Process-wide so a uid or revision from an old queue never matches a new one.
std::atomic<uint64_t> next_uid{1}, next_revision{1};
}

PlayQueue::PlayQueue(std::vector<subsonic::Song> tracks)
    : tracks_(std::move(tracks)), ids_(tracks_.size()), order_(tracks_.size()) {
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    const uint64_t first = next_uid.fetch_add(ids_.size());
    std::iota(ids_.begin(), ids_.end(), first);
    touch();
}

void PlayQueue::touch() { revision_ = next_revision.fetch_add(1); }

std::optional<std::size_t> PlayQueue::find(uint64_t uid, std::size_t from) const {
    for (std::size_t i = from; i < order_.size(); ++i)
        if (ids_[order_[i]] == uid) return i;
    return std::nullopt;
}

std::optional<uint64_t> PlayQueue::successor_uid() const {
    auto index = successor_index();
    return index ? std::optional<uint64_t>(ids_[*index]) : std::nullopt;
}

std::optional<std::size_t> PlayQueue::current_index() const {
    if (order_.empty()) return std::nullopt;
    return order_[position_];
}

const subsonic::Song *PlayQueue::current() const {
    auto index = current_index();
    return index ? &tracks_[*index] : nullptr;
}

const subsonic::Song *PlayQueue::up_next() const {
    if (order_.empty() || repeat_ == Repeat::one) return nullptr;
    if (position_ + 1 < order_.size()) return &tracks_[order_[position_ + 1]];
    return repeat_ == Repeat::all && order_.size() > 1 ? &tracks_[order_[0]] : nullptr;
}

bool PlayQueue::next() {
    if (order_.empty()) return false;
    if (position_ + 1 < order_.size()) {
        ++position_;
        if (pinned_) --pinned_;
        touch();
        return true;
    }
    if (repeat_ != Repeat::all) return false;
    position_ = 0; pinned_ = 0;
    touch();
    return true;
}

bool PlayQueue::previous() {
    if (order_.empty()) return false;
    if (position_ > 0) {
        --position_; pinned_ = 0;
        touch();
        return true;
    }
    if (repeat_ != Repeat::all) return false;
    position_ = order_.size() - 1; pinned_ = 0;
    touch();
    return true;
}

std::optional<std::size_t> PlayQueue::successor_index() const {
    if (order_.empty()) return std::nullopt;
    if (repeat_ == Repeat::one) return order_[position_];
    if (position_ + 1 < order_.size()) return order_[position_ + 1];
    if (repeat_ == Repeat::all) return order_[0];
    return std::nullopt;
}

bool PlayQueue::on_track_end() {
    if (repeat_ == Repeat::one) return current() != nullptr;
    return next();
}

void PlayQueue::set_shuffle(bool enabled, std::mt19937 &rng) {
    if (enabled == shuffled_) return;
    auto index = current_index();
    if (enabled && index) {
        std::swap(order_[0], order_[position_]);
        std::shuffle(order_.begin() + 1, order_.end(), rng);
        position_ = 0;
    } else if (!enabled) {
        std::iota(order_.begin(), order_.end(), std::size_t{0});
        position_ = index.value_or(0);
    }
    shuffled_ = enabled; pinned_ = 0;
    touch();
}

void PlayQueue::set_live_shuffle(bool enabled, std::mt19937 &rng) {
    if (enabled == shuffled_) return;
    if (!order_.empty()) {
        // Played-next entries keep their place ahead of the reshuffled tail.
        auto begin = order_.begin() + position_ + 1 + pinned_;
        if (enabled) std::shuffle(begin, order_.end(), rng);
        else {
            // Canonical order continuing after current, wrapping to earlier unplayed entries.
            const auto at = order_[position_], n = order_.size();
            std::sort(begin, order_.end(), [&](std::size_t a, std::size_t b) {
                return (a < at ? a + n : a) < (b < at ? b + n : b);
            });
        }
    }
    shuffled_ = enabled;
    touch();
}

void PlayQueue::append(subsonic::Song track) {
    tracks_.push_back(std::move(track));
    ids_.push_back(next_uid.fetch_add(1));
    order_.push_back(tracks_.size() - 1);
    touch();
}

void PlayQueue::insert_next(subsonic::Song track) {
    if (order_.empty()) {
        append(std::move(track));
        return;
    }
    const auto index = order_[position_] + 1;
    tracks_.insert(tracks_.begin() + index, std::move(track));
    ids_.insert(ids_.begin() + index, next_uid.fetch_add(1));
    for (auto &entry : order_) {
        if (entry >= index) ++entry;
    }
    order_.insert(order_.begin() + position_ + 1, index);
    touch();
}

bool PlayQueue::play_next(std::vector<subsonic::Song> tracks) {
    const auto n = tracks.size();
    if (!n || tracks_.size() > max_tracks || n > max_tracks - tracks_.size()) return false;
    const bool was_empty = order_.empty();
    // After the last pinned entry (or current), in play order and canonically.
    const auto at = was_empty ? 0 : position_ + pinned_;
    const auto index = was_empty ? 0 : order_[at] + 1;
    tracks_.insert(tracks_.begin() + index, std::make_move_iterator(tracks.begin()),
                   std::make_move_iterator(tracks.end()));
    ids_.insert(ids_.begin() + index, n, 0);
    const uint64_t first = next_uid.fetch_add(n);
    std::iota(ids_.begin() + index, ids_.begin() + index + n, first);
    for (auto &entry : order_) {
        if (entry >= index) entry += n;
    }
    order_.insert(order_.begin() + (was_empty ? 0 : at + 1), n, 0);
    std::iota(order_.begin() + (was_empty ? 0 : at + 1), order_.begin() + (was_empty ? 0 : at + 1) + n, index);
    pinned_ += was_empty ? n - 1 : n;
    touch();
    return true;
}

bool PlayQueue::move_after(uint64_t uid, uint64_t anchor) {
    if (order_.empty() || uid == anchor) return false;
    const auto from = find(uid, position_ + 1), after = find(anchor, position_);
    if (!from || !after) return false;
    auto to = *after;
    if (*from < to) --to; // Anchor shifts down once the entry leaves its slot.
    ++to;
    if (to != *from) {
        const auto entry = order_[*from];
        const auto last_pinned = position_ + pinned_;
        if (*from <= last_pinned) --pinned_;
        order_.erase(order_.begin() + *from);
        order_.insert(order_.begin() + to, entry);
        // Dropped strictly inside the played-next block: it joins that block.
        if (to <= position_ + pinned_) ++pinned_;
        if (!shuffled_) {
            // Manual order is the canonical order while unshuffled.
            std::vector<subsonic::Song> tracks;
            std::vector<uint64_t> ids;
            tracks.reserve(order_.size()); ids.reserve(order_.size());
            for (auto index : order_) { tracks.push_back(std::move(tracks_[index])); ids.push_back(ids_[index]); }
            tracks_ = std::move(tracks); ids_ = std::move(ids);
            std::iota(order_.begin(), order_.end(), std::size_t{0});
        }
    }
    touch();
    return true;
}

bool PlayQueue::remove(uint64_t uid) {
    if (order_.empty()) return false;
    const auto at = find(uid, position_ + 1);
    if (!at) return false;
    const auto index = order_[*at];
    if (*at <= position_ + pinned_) --pinned_;
    order_.erase(order_.begin() + *at);
    tracks_.erase(tracks_.begin() + index);
    ids_.erase(ids_.begin() + index);
    for (auto &entry : order_) {
        if (entry > index) --entry;
    }
    touch();
    return true;
}

} // namespace app
