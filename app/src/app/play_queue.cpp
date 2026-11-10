/*
 * Navi5 — local play queue
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "play_queue.h"
#include <algorithm>
#include <numeric>
#include <utility>

namespace app {

PlayQueue::PlayQueue(std::vector<subsonic::Song> tracks)
    : tracks_(std::move(tracks)), order_(tracks_.size()) {
    std::iota(order_.begin(), order_.end(), std::size_t{0});
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
        return true;
    }
    if (repeat_ != Repeat::all) return false;
    position_ = 0;
    return true;
}

bool PlayQueue::previous() {
    if (order_.empty()) return false;
    if (position_ > 0) {
        --position_;
        return true;
    }
    if (repeat_ != Repeat::all) return false;
    position_ = order_.size() - 1;
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
    shuffled_ = enabled;
}

void PlayQueue::set_live_shuffle(bool enabled, std::mt19937 &rng) {
    if (enabled == shuffled_) return;
    if (!order_.empty()) {
        auto begin = order_.begin() + position_ + 1;
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
}

void PlayQueue::append(subsonic::Song track) {
    tracks_.push_back(std::move(track));
    order_.push_back(tracks_.size() - 1);
}

void PlayQueue::insert_next(subsonic::Song track) {
    if (order_.empty()) {
        append(std::move(track));
        return;
    }
    const auto index = order_[position_] + 1;
    tracks_.insert(tracks_.begin() + index, std::move(track));
    for (auto &entry : order_) {
        if (entry >= index) ++entry;
    }
    order_.insert(order_.begin() + position_ + 1, index);
}

} // namespace app
