/*
 * Navi5 — offline play queue checks
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/play_queue.h"
#include <algorithm>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

using app::PlayQueue;
using app::Repeat;

static subsonic::Song song(std::string id, std::string title = "") {
    subsonic::Song result;
    result.id = std::move(id);
    result.title = std::move(title);
    return result;
}

int main() {
    std::mt19937 rng(12);
    PlayQueue empty;
    for (auto repeat : {Repeat::off, Repeat::all, Repeat::one}) {
        empty.set_repeat(repeat);
        assert(!empty.current() && !empty.current_index());
        assert(!empty.next() && !empty.previous() && !empty.on_track_end());
        empty.set_shuffle(true, rng);
        assert(empty.shuffled() && empty.tracks().empty());
        empty.set_shuffle(false, rng);
    }
    empty.insert_next(song("first"));
    assert(empty.current()->id == "first" && empty.current_index() == 0);

    PlayQueue single;
    single.set_shuffle(true, rng);
    single.append(song("only"));
    single.set_shuffle(false, rng);
    assert(single.current()->id == "only");
    for (auto repeat : {Repeat::off, Repeat::one, Repeat::all}) {
        single.set_repeat(repeat);
        assert(single.next() == (repeat == Repeat::all));
        assert(single.previous() == (repeat == Repeat::all));
        assert(single.on_track_end() == (repeat != Repeat::off));
        assert(single.current_index() == 0);
    }

    PlayQueue queue({song("a"), song("b"), song("c")});
    assert(!queue.previous());
    assert(queue.next() && queue.current()->id == "b");
    queue.set_repeat(Repeat::one);
    assert(queue.on_track_end() && queue.current()->id == "b");
    assert(queue.previous() && queue.current()->id == "a");
    assert(!queue.previous());
    assert(queue.next() && queue.next() && queue.current()->id == "c");
    assert(!queue.next()); // Manual next never loops repeat-one.
    assert(queue.on_track_end() && queue.current()->id == "c");
    queue.set_repeat(Repeat::off);
    assert(!queue.on_track_end() && queue.current()->id == "c");
    queue.set_repeat(Repeat::all);
    assert(queue.on_track_end() && queue.current()->id == "a");
    assert(queue.previous() && queue.current()->id == "c");
    assert(queue.next() && queue.current()->id == "a");

    PlayQueue album({song("disc1:1", "First"), song("disc1:2", "Second"), song("disc2:1", "Third")});
    assert(album.up_next()->title == "Second");
    assert(album.on_track_end() && album.current()->title == "Second");
    assert(album.up_next()->title == "Third");
    album.set_repeat(Repeat::one);
    assert(!album.up_next());
    assert(album.on_track_end() && album.current()->title == "Second");
    album.set_repeat(Repeat::off);
    assert(album.on_track_end() && album.current()->title == "Third");
    assert(!album.up_next() && !album.on_track_end());

    // Opaque IDs, including empty strings and duplicate IDs, are never parsed.
    const std::string opaque = "001:/?% mixed\xE2\x98\x83";
    PlayQueue duplicates({song(opaque, "first"), song(opaque, "second"), song("")});
    assert(duplicates.next());
    duplicates.set_shuffle(true, rng);
    assert(duplicates.current()->title == "second");
    assert(duplicates.current_index() == 1);
    duplicates.set_shuffle(false, rng);
    assert(duplicates.current()->title == "second");
    assert(duplicates.current_index() == 1);
    assert(duplicates.tracks()[0].id == opaque && duplicates.tracks()[1].id == opaque);
    assert(duplicates.next() && duplicates.current()->id.empty());

    std::vector<subsonic::Song> tracks;
    for (int i = 0; i < 20; ++i) tracks.push_back(song(std::to_string(i)));
    PlayQueue shuffled(tracks), same(tracks);
    for (int i = 0; i < 7; ++i) assert(shuffled.next() && same.next());
    std::mt19937 seed_a(42), seed_b(42);
    shuffled.set_shuffle(true, seed_a);
    same.set_shuffle(true, seed_b);
    assert(shuffled.current()->id == "7");
    auto rng_before = seed_a;
    shuffled.set_shuffle(true, seed_a);
    assert(seed_a == rng_before); // Idempotent; does not consume RNG.
    std::vector<std::size_t> seen;
    do {
        assert(shuffled.current_index() == same.current_index());
        seen.push_back(*shuffled.current_index());
        bool advanced = shuffled.next();
        assert(same.next() == advanced);
        if (!advanced) break;
    } while (true);
    auto sorted = seen;
    std::sort(sorted.begin(), sorted.end());
    assert(sorted.size() == tracks.size());
    for (std::size_t i = 0; i < sorted.size(); ++i) assert(sorted[i] == i);
    assert(seen != sorted);
    const auto last = shuffled.current_index();
    shuffled.set_shuffle(false, seed_a);
    assert(shuffled.current_index() == last);
    for (std::size_t i = 0; i < tracks.size(); ++i)
        assert(shuffled.tracks()[i].id == tracks[i].id);

    // Insert-next and append maintain both canonical and shuffled orders.
    for (bool shuffle : {false, true}) {
        PlayQueue edits({song("a"), song("b"), song("c")});
        assert(edits.next());
        edits.set_shuffle(shuffle, rng);
        edits.insert_next(song("x"));
        edits.insert_next(song("y")); // Most recent insertion is next.
        edits.append(song("tail"));
        assert(edits.current()->id == "b");
        assert(edits.next() && edits.current()->id == "y");
        assert(edits.next() && edits.current()->id == "x");
        assert(edits.previous() && edits.current()->id == "y");
        edits.set_shuffle(false, rng);
        assert(edits.current()->id == "y" && edits.current_index() == 2);
        const std::vector<std::string> expected = {"a", "b", "y", "x", "c", "tail"};
        for (std::size_t i = 0; i < expected.size(); ++i)
            assert(edits.tracks()[i].id == expected[i]);
        while (edits.next()) {}
        assert(edits.current()->id == "tail");
        edits.insert_next(song("new last"));
        assert(edits.next() && edits.current()->id == "new last");
        assert(!edits.next());
    }

    // Shuffled edits visit every occurrence once, including appended tail.
    PlayQueue edits(tracks);
    edits.set_shuffle(true, rng);
    edits.insert_next(song("next"));
    edits.append(song("tail"));
    std::vector<std::size_t> edited_seen{*edits.current_index()};
    assert(edits.next() && edits.current()->id == "next");
    do {
        edited_seen.push_back(*edits.current_index());
    } while (edits.next());
    assert(edits.current()->id == "tail");
    std::sort(edited_seen.begin(), edited_seen.end());
    assert(edited_seen.size() == edits.tracks().size());
    for (std::size_t i = 0; i < edited_seen.size(); ++i) assert(edited_seen[i] == i);

    // Live toggles: visited history stays fixed, current stays, every occurrence
    // (including duplicate IDs) plays exactly once per cycle; OFF restores canonical.
    for (unsigned seed = 0; seed < 50; ++seed) {
        std::mt19937 live(seed);
        PlayQueue q({song("a"), song("dup"), song("dup"), song("b"), song("c"), song("d")});
        std::vector<std::size_t> seen{*q.current_index()};
        q.next(); seen.push_back(*q.current_index());
        q.set_live_shuffle(true, live);
        assert(q.shuffled() && *q.current_index() == 1);
        q.set_live_shuffle(true, live); // idempotent
        q.next(); seen.push_back(*q.current_index());
        const auto at = *q.current_index();
        q.set_live_shuffle(false, live);
        assert(!q.shuffled() && *q.current_index() == at);
        // Remaining canonical from current's successor, wrapping to earlier unplayed.
        std::vector<std::size_t> rest;
        while (q.on_track_end()) rest.push_back(*q.current_index());
        for (std::size_t i = 1; i < rest.size(); ++i)
            assert((rest[i - 1] < at ? rest[i - 1] + 6 : rest[i - 1]) < (rest[i] < at ? rest[i] + 6 : rest[i]));
        seen.insert(seen.end(), rest.begin(), rest.end());
        std::sort(seen.begin(), seen.end());
        assert((seen == std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
    }
    PlayQueue tail({song("x"), song("y")});
    tail.next();
    tail.set_live_shuffle(true, rng);
    assert(*tail.current_index() == 1 && !tail.successor_index());
    tail.set_live_shuffle(false, rng);
    assert(*tail.current_index() == 1 && !tail.successor_index());

    // Play Next keeps insertion order behind earlier played-next entries, with
    // and without shuffle; the cursor is consumed as playback advances.
    const auto ids = [](const PlayQueue &q) {
        std::vector<std::string> out;
        for (std::size_t i = q.position(); i < q.size(); ++i) out.push_back(q.at(i).id);
        return out;
    };
    using Ids = std::vector<std::string>;
    for (bool shuffle : {false, true}) {
        PlayQueue q({song("a"), song("b"), song("c"), song("d")});
        assert(q.next());
        if (shuffle) q.set_live_shuffle(true, rng);
        const auto before = q.revision();
        assert(!q.play_next({}) && q.revision() == before);
        assert(q.play_next({song("x")}) && q.revision() != before);
        assert(q.play_next({song("y1"), song("y2")}));
        assert(q.play_next({song("z")}));
        assert(q.pinned() == 4);
        auto upcoming = ids(q);
        upcoming.resize(5);
        assert((upcoming == Ids{"b", "x", "y1", "y2", "z"}));
        assert(q.successor_uid() == q.uid_at(q.position() + 1));
        // Toggling shuffle never scatters what was asked to play next.
        q.set_live_shuffle(!shuffle, rng);
        upcoming = ids(q); upcoming.resize(5);
        assert((upcoming == Ids{"b", "x", "y1", "y2", "z"}));
        assert(q.next() && q.next() && q.current()->id == "y1" && q.pinned() == 2);
        assert(q.play_next({song("w")}));
        upcoming = ids(q); upcoming.resize(4);
        assert((upcoming == Ids{"y1", "y2", "z", "w"}));
        // Advancing past the block resets the cursor: next insert is immediate.
        for (int i = 0; i < 3; ++i) assert(q.next());
        assert(q.current()->id == "w" && q.pinned() == 0);
        assert(q.play_next({song("v")}) && q.at(q.position() + 1).id == "v");
        // Going back forgets the cursor.
        assert(q.previous() && q.pinned() == 0);
        assert(q.play_next({song("u")}) && q.at(q.position() + 1).id == "u");
        std::size_t seen = 1;
        while (q.next()) ++seen;
        assert(seen + q.position() + 1 - seen == q.size() && q.size() == 4 + 7);
    }
    {
        // Unshuffled Play Next keeps canonical order equal to play order.
        PlayQueue q({song("a"), song("b"), song("c")});
        assert(q.play_next({song("x"), song("y")}) && q.play_next({song("z")}));
        const Ids expected{"a", "x", "y", "z", "b", "c"};
        for (std::size_t i = 0; i < expected.size(); ++i) assert(q.tracks()[i].id == expected[i]);
        assert(ids(q) == expected);
        PlayQueue fresh;
        assert(fresh.play_next({song("p"), song("q")}));
        assert(fresh.current()->id == "p" && fresh.pinned() == 1 && fresh.up_next()->id == "q");
        assert(fresh.play_next({song("r")}) && (ids(fresh) == Ids{"p", "q", "r"}));
    }
    {
        // Cap: failures add nothing.
        std::vector<subsonic::Song> many(PlayQueue::max_tracks - 1, song("m"));
        PlayQueue q(std::move(many));
        const auto revision = q.revision();
        assert(!q.play_next({song("one"), song("two")}) && q.size() == PlayQueue::max_tracks - 1);
        assert(q.revision() == revision && q.pinned() == 0);
        assert(q.play_next({song("one")}) && q.size() == PlayQueue::max_tracks);
        assert(!q.play_next({song("two")}));
        // Reorder and remove stay cheap at the cap.
        const auto last = q.uid_at(q.size() - 1);
        assert(q.move_after(last, q.uid_at(0)) && q.uid_at(1) == last);
        assert(q.remove(last) && q.size() == PlayQueue::max_tracks - 1);
    }
    for (bool shuffle : {false, true}) {
        // Move/remove touch upcoming entries only; duplicate IDs are told apart by uid.
        PlayQueue q({song("a"), song("dup"), song("dup"), song("b"), song("c")});
        assert(q.next());
        if (shuffle) q.set_live_shuffle(true, rng);
        const auto current = q.uid_at(q.position()), played = q.uid_at(0);
        const auto first = q.uid_at(2), second = q.uid_at(3), third = q.uid_at(4);
        assert(!q.move_after(current, third) && !q.remove(current));     // Current is fixed.
        assert(!q.move_after(played, third) && !q.remove(played));       // So is history.
        assert(!q.move_after(first, played) && !q.move_after(first, first));
        assert(!q.move_after(first, 0) && !q.remove(0));
        assert(q.move_after(third, current));
        assert(q.uid_at(2) == third && q.uid_at(3) == first && q.uid_at(4) == second);
        assert(q.successor_uid() == third);
        assert(q.move_after(third, second));                              // Down past two.
        assert(q.uid_at(2) == first && q.uid_at(3) == second && q.uid_at(4) == third);
        assert(q.move_after(second, first));                              // No-op move succeeds.
        assert(q.uid_at(3) == second);
        assert(q.remove(first) && q.size() == 4 && q.uid_at(2) == second);
        assert(q.uid_at(q.position()) == current && q.current()->id == "dup");
        if (!shuffle) for (std::size_t i = 0; i < q.size(); ++i) assert(q.uid_at(i) == q.uid_at(i) && &q.at(i) == &q.tracks()[i]);
        std::size_t rest = 0;
        while (q.next()) ++rest;
        assert(rest == 2);
    }
    {
        // Pinned block follows manual edits: removing a pinned entry shrinks it,
        // moving one out releases it, dropping one inside joins it.
        PlayQueue q({song("a"), song("b"), song("c")});
        assert(q.play_next({song("x"), song("y")}));
        const auto x = q.uid_at(1), y = q.uid_at(2), b = q.uid_at(3), c = q.uid_at(4);
        assert(q.remove(x) && q.pinned() == 1);
        assert(q.move_after(y, c) && q.pinned() == 0);
        assert(q.play_next({song("n1"), song("n2")}) && q.pinned() == 2);
        assert(q.move_after(b, q.uid_at(1)) && q.pinned() == 3);
        assert((ids(q) == Ids{"a", "n1", "b", "n2", "c", "y"}));
        assert(q.play_next({song("n3")}) && (ids(q) == Ids{"a", "n1", "b", "n2", "n3", "c", "y"}));
        // Unshuffled manual order is canonical: a shuffle round trip keeps it.
        q.set_live_shuffle(true, rng); q.set_live_shuffle(false, rng);
        assert((ids(q) == Ids{"a", "n1", "b", "n2", "n3", "c", "y"}));
    }
    {
        // A copy is the same queue (same revision and uids); any edit diverges.
        PlayQueue q({song("a"), song("b")});
        auto copy = q;
        assert(copy.revision() == q.revision() && copy.uid_at(1) == q.uid_at(1));
        assert(copy.play_next({song("x")}) && copy.revision() != q.revision());
        PlayQueue other({song("a"), song("b")});
        assert(other.uid_at(0) != q.uid_at(0) && other.revision() != q.revision());
    }
}
