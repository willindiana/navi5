/* Navi5 — music browsing state.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/music_state.h"
#include <utility>
#include <algorithm>
#include <iterator>

namespace ui::music {
void State::load()
{
    ++context_;
    frame_.request.token = ++serial_;
    frame_.request.limit = page_limit;
    pending_ = frame_.request;
    play_.reset();
    if (!frame_.request.home_retry) page_ = {};
    loading_ = true;
    direction_ = 0;
    continuation_error_ = false;
    desired_.reset();
    status_ = Status::Loading;
}
void State::search()
{
    if (frame_.request.view == View::Search) return;
    frame_.home_page = page_;
    frame_.restore = status_ == Status::Ready; // Back restores exact page; loading/error reloads.
    parents_.push_back(frame_);
    frame_ = {};
    frame_.request.view = View::Search;
    frame_.request.token = ++serial_;
    search_due_ = -1;
    page_ = {}; pending_.reset(); play_.reset(); status_ = Status::Ready;
}
bool State::query(const std::string &value, double now)
{
    if (frame_.request.view != View::Search || !Request::valid_query(value)) return false;
    if (value == frame_.request.query) return true;
    frame_.request.query = value;
    frame_.request.token = ++serial_;
    frame_.selected = 0;
    page_ = {}; pending_.reset(); play_.reset();
    search_due_ = value.find_first_not_of(' ') == std::string::npos ? -1 : now + .3;
    status_ = search_due_ < 0 ? Status::Ready : Status::Loading;
    return true;
}
void State::tick(double now)
{
    if (frame_.request.view == View::Search && search_due_ >= 0 && now >= search_due_) {
        search_due_ = -1;
        load();
    }
}
void State::activate() { if (status_ == Status::Idle) load(); }
void State::history_changed()
{
    for (auto &parent : parents_) if (parent.request.view == View::Home) parent.history_stale = true;
    if (frame_.request.view == View::Home) { frame_.request.home_retry = 0; load(); }
}
void State::tab(int index)
{
    if (index < 0 || index > 3 || (index == tab_ && parents_.empty() && frame_.request.view != View::Search)) return;
    // Root lists keep window/focus across tab return; Home refreshes as before.
    // Search/partial parents restore only when their saved page was Ready.
    if (tab_ && (parents_.empty() ? status_ == Status::Ready : parents_.front().restore)) {
        if (parents_.empty()) { frame_.home_page = page_; tabs_[tab_] = frame_; }
        else tabs_[tab_] = parents_.front();
    }
    tab_ = index;
    parents_.clear();
    if (tabs_[index]) {
        frame_ = std::move(*tabs_[index]);
        tabs_[index].reset();
        ++context_;
        page_ = std::move(frame_.home_page);
        frame_.request.token = ++serial_;
        pending_.reset(); loading_ = false; direction_ = 0; desired_.reset();
        continuation_error_ = false; status_ = Status::Ready;
        return;
    }
    retry_page_ = {};
    frame_ = {};
    frame_.request.view = static_cast<View>(index);
    load();
}
bool State::move(int delta)
{
    if (status_ != Status::Ready || page_.items.empty()) return false;
    const auto target = static_cast<long long>(frame_.selected) + delta;
    if (target < 0) {
        if (!frame_.first_page || continuation_error_) return false;
        desired_ = size_t(std::max<long long>(0, static_cast<long long>(page_.start_index) + target));
        if (!loading_) extend(-1);
        return true;
    }
    if (target >= static_cast<long long>(page_.items.size())) {
        if (frame_.request.view == View::Search || page_.next_cursor.empty() || at_ceiling() || continuation_error_) return false;
        desired_ = page_.start_index + size_t(target);
        if (!loading_) extend(1);
        return true;
    }
    frame_.selected = static_cast<size_t>(target);
    desired_.reset();
    return true;
}
void State::open()
{
    if (status_ != Status::Ready || frame_.selected >= page_.items.size()) return;
    const Item item = page_.items[frame_.selected];
    if (item.id.empty()) return;
    if (item.kind == Kind::Track) {
        play_ = intent(item.id, page_.start_index + frame_.selected);
        return;
    }
    frame_.home_page = page_;
    frame_.restore = true; // open() requires Ready: Back restores exact window.
    parents_.push_back(frame_);
    frame_ = {};
    frame_.request.view = item.kind == Kind::Artist ? View::ArtistAlbums :
                          item.kind == Kind::Album ? View::AlbumTracks : View::PlaylistTracks;
    frame_.request.parent_id = item.id;
    frame_.title = item.title;
    load();
}
void State::play_top_song(size_t occurrence)
{
    if (status_ != Status::Ready || frame_.request.view != View::ArtistAlbums ||
        !page_.top_songs_ok || occurrence >= page_.top_songs.size()) return;
    PlayIntent p;
    p.context = View::ArtistAlbums;
    p.context_id = frame_.request.parent_id;
    p.song_id = page_.top_songs[occurrence].id;
    p.title = page_.top_songs[occurrence].title;
    p.occurrence = occurrence;
    for (const auto &song : page_.top_songs) p.page_ids.push_back(song.id);
    p.top_snapshot = page_.top_songs;
    play_ = std::move(p);
}
void State::play_album(bool shuffle)
{
    if ((frame_.request.view == View::AlbumTracks || frame_.request.view == View::PlaylistTracks) &&
        status_ == Status::Ready && !page_.items.empty()) {
        play_ = intent("", 0);
        if (shuffle) play_->action = PlayAction::DetailShuffle;
    }
}
PlayIntent State::intent(std::string song_id, size_t occurrence) const
{
    PlayIntent p{std::move(song_id), frame_.request.parent_id, {}, frame_.request.view, occurrence, page_.start_index, {}};
    if (frame_.selected < page_.items.size()) p.title = page_.items[frame_.selected].title;
    if (frame_.request.view == View::Search) {
        p.context_id = frame_.request.query;
        p.occurrence = 0;
        for (size_t i = 0; i < page_.items.size(); ++i) {
            if (page_.items[i].kind != Kind::Track) continue;
            if (i < frame_.selected) ++p.occurrence;
            p.page_ids.push_back(page_.items[i].id);
        }
    } else for (const auto &item : page_.items) p.page_ids.push_back(item.id);
    return p;
}
bool State::back()
{
    if (parents_.empty()) return false;
    frame_ = std::move(parents_.back());
    parents_.pop_back();
    ++context_;
    if (frame_.request.view == View::Home || frame_.request.view == View::Search || frame_.restore) {
        frame_.restore = false;
        frame_.request.token = ++serial_;
        pending_.reset();
        play_.reset();
        page_ = std::move(frame_.home_page);
        loading_ = false; direction_ = 0; desired_.reset(); continuation_error_ = false;
        status_ = Status::Ready;
        if (frame_.history_stale) { frame_.history_stale = false; load(); }
    } else load();
    return true;
}
void State::extend(int direction)
{
    if (loading_ || status_ != Status::Ready) return;
    direction_ = direction;
    if (direction > 0) {
        if (page_.next_cursor.empty() || at_ceiling()) return;
        frame_.request.cursor = page_.next_cursor;
        frame_.offset = page_.start_index + page_.items.size();
    } else {
        if (!frame_.first_page) return;
        const auto &checkpoint = frame_.checkpoints[frame_.first_page - 1];
        frame_.request.cursor = checkpoint.cursor;
        frame_.offset = checkpoint.offset;
    }
    continuation_error_ = false;
    loading_ = true;
    frame_.request.token = ++serial_;
    pending_ = frame_.request;
}
void State::approach(unsigned distance)
{
    if (frame_.request.view == View::Home || frame_.request.view == View::Search || loading_ || continuation_error_) return;
    if (page_.items.size() <= frame_.selected + distance && !page_.next_cursor.empty()) extend(1);
}
bool State::next()
{
    if (loading_ || status_ != Status::Ready || page_.next_cursor.empty() || at_ceiling()) return false;
    extend(1);
    return loading_;
}
bool State::previous()
{
    if (loading_ || status_ != Status::Ready || !frame_.first_page) return false;
    extend(-1);
    return loading_;
}
void State::retry()
{
    if (continuation_error_) { extend(direction_); return; }
    if (frame_.request.view == View::Home && status_ == Status::Ready) {
        frame_.request.home_retry = 0;
        for (size_t i = 0; i < page_.sections.size(); ++i)
            if (!page_.sections[i].ok) frame_.request.home_retry |= 1u << i;
        if (!frame_.request.home_retry) return;
        retry_page_ = page_;
        load();
    } else if (status_ == Status::Error) load();
}
bool State::complete(uint64_t token, Page page, bool success)
{
    if (!loading_ || token != frame_.request.token) return false;
    if (!success && frame_.request.home_retry && retry_page_.sections.size() == 2) {
        page = {};
        page.sections = {{0, 0, false}, {0, 0, false}};
        success = true; // Keep usable shelf even when retry dispatch/transport fails.
    }
    if (success && frame_.request.home_retry && page.sections.size() == 2 && retry_page_.sections.size() == 2) {
        Page merged;
        for (size_t i = 0; i < 2; ++i) {
            const Page &source = (frame_.request.home_retry & (1u << i)) ? page : retry_page_;
            const auto &section = source.sections[i];
            if (section.begin > source.items.size() || section.count > source.items.size() - section.begin) { success = false; break; }
            merged.sections.push_back({merged.items.size(), section.count, section.ok});
            merged.items.insert(merged.items.end(), source.items.begin() + section.begin, source.items.begin() + section.begin + section.count);
        }
        if (success) page = std::move(merged);
    }
    retry_page_ = {};
    frame_.request.home_retry = 0;
    // Reject broken adapters, rather than silently skipping entries when paging.
    bool sections_valid = page.sections.empty();
    if (frame_.request.view == View::Home && page.sections.size() == 2) {
        size_t end = 0;
        sections_valid = page.next_cursor.empty();
        for (const auto &s : page.sections) {
            sections_valid = sections_valid && s.begin == end && s.count <= 6 && (s.ok || s.count == 0);
            end += s.count;
        }
        sections_valid = sections_valid && end == page.items.size();
    }
    bool top_valid = page.top_songs.size() <= 20 && (page.top_songs_ok || page.top_songs.empty());
    for (const auto &song : page.top_songs) top_valid = top_valid && song.kind == Kind::Track && !song.id.empty();
    bool valid = top_valid && sections_valid && page.items.size() <= page_limit &&
                 (page.next_cursor.empty() || page.next_cursor != frame_.request.cursor);
    for (const auto &item : page.items) {
        const View view = frame_.request.view;
        const bool kind_ok = view == View::Search ? (item.kind == Kind::Track || item.kind == Kind::Artist || item.kind == Kind::Album || item.kind == Kind::Playlist) : view == View::Home ? item.kind != Kind::Track :
            view == View::Artists ? item.kind == Kind::Artist :
            view == View::Albums || view == View::ArtistAlbums ? item.kind == Kind::Album :
            view == View::Playlists ? item.kind == Kind::Playlist : item.kind == Kind::Track;
        valid = valid && kind_ok && !item.id.empty();
    }
    loading_ = false;
    pending_.reset();
    if (direction_) {
        const size_t index = direction_ > 0 ? frame_.last_page + 1 : frame_.first_page - 1;
        if (index < frame_.checkpoints.size()) {
            const auto &checkpoint = frame_.checkpoints[index];
            valid = valid && checkpoint.cursor == frame_.request.cursor &&
                checkpoint.offset == frame_.offset && checkpoint.count == page.items.size();
            if (index + 1 < frame_.checkpoints.size())
                valid = valid && page.next_cursor == frame_.checkpoints[index + 1].cursor;
        }
        if (!success) { continuation_error_ = true; return true; }
        if (!valid) {
            // Server order changed or adapter broke: never splice mismatched windows.
            frame_.request.cursor.clear();
            frame_.offset = frame_.selected = 0;
            frame_.checkpoints.clear();
            frame_.counts.clear();
            frame_.first_page = frame_.last_page = 0;
            page_ = {};
            direction_ = 0;
            desired_.reset();
            status_ = Status::Error;
            return true;
        }
        if (index == frame_.checkpoints.size())
            frame_.checkpoints.push_back({frame_.request.cursor, frame_.offset, page.items.size()});
        const size_t absolute = desired_.value_or(page_.start_index + frame_.selected);
        if (direction_ > 0) {
            frame_.last_page = index;
            frame_.counts.push_back(page.items.size());
            page_.items.insert(page_.items.end(), std::make_move_iterator(page.items.begin()), std::make_move_iterator(page.items.end()));
            page_.next_cursor = std::move(page.next_cursor);
            // Evict whole pages, never the one holding focus; empty sparse pages go first.
            while (frame_.counts.size() > 1 && (!frame_.counts.front() || page_.items.size() > window_items) &&
                   absolute >= page_.start_index + frame_.counts.front()) {
                const size_t count = frame_.counts.front();
                page_.items.erase(page_.items.begin(), page_.items.begin() + count);
                page_.start_index += count;
                frame_.counts.erase(frame_.counts.begin());
                ++frame_.first_page;
            }
        } else {
            frame_.first_page = index;
            frame_.counts.insert(frame_.counts.begin(), page.items.size());
            page_.items.insert(page_.items.begin(), std::make_move_iterator(page.items.begin()), std::make_move_iterator(page.items.end()));
            page_.start_index = frame_.offset;
            while (frame_.counts.size() > 1 && (!frame_.counts.back() || page_.items.size() > window_items) &&
                   absolute < page_.start_index + page_.items.size() - frame_.counts.back()) {
                const size_t count = frame_.counts.back();
                page_.items.resize(page_.items.size() - count);
                frame_.counts.pop_back();
                page_.next_cursor = frame_.checkpoints[frame_.last_page].cursor;
                --frame_.last_page;
            }
        }
        frame_.selected = page_.items.empty() ? 0 : std::min(page_.items.size() - 1,
            absolute >= page_.start_index ? absolute - page_.start_index : size_t(0));
        const bool after = desired_ && *desired_ >= page_.start_index + page_.items.size();
        const bool before = desired_ && *desired_ < page_.start_index;
        direction_ = 0; continuation_error_ = false;
        // Sparse artist batches may return few/zero cards: keep walking toward target.
        if (after && !page_.next_cursor.empty() && !at_ceiling()) extend(1);
        else if (before && frame_.first_page) extend(-1);
        else desired_.reset();
    } else {
        status_ = success && valid ? Status::Ready : Status::Error;
        if (status_ == Status::Ready) {
            // Raw artist offset is NOT a displayed occurrence offset.
            page.start_index = frame_.offset;
            frame_.checkpoints = {{frame_.request.cursor, frame_.offset, page.items.size()}};
            frame_.first_page = frame_.last_page = 0;
            frame_.counts = {page.items.size()};
            page_ = std::move(page);
            if (frame_.selected >= page_.items.size()) frame_.selected = 0;
        } else page_ = {};
    }
    return true;
}
std::optional<Request> State::take_request()
{
    auto result = std::move(pending_);
    pending_.reset();
    return result;
}
std::optional<PlayIntent> State::take_play_intent()
{
    auto result = std::move(play_);
    play_.reset();
    return result;
}
} // namespace ui::music
