/* Navi5 — real decoder/native output gapless continuity.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/audio_player.h"
#include "app/play_queue.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace std::chrono_literals;
namespace {
std::mutex mutex;
std::vector<float> output;
int opens = 0, closes = 0, handle = 0;
std::atomic<int> writes{0};
std::atomic<int> fail_write{0};
std::atomic<bool> hold{false}, entered{false};
std::atomic<int> delay_ms{1};
template<class F> void until(F f) {
    auto end = std::chrono::steady_clock::now() + 15s;
    while (!f()) { assert(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
}
void reset() {
    std::lock_guard<std::mutex> lock(mutex);
    assert(handle == 0); output.clear(); opens = closes = 0; writes = 0; fail_write = 0; entered = false;
}
app::AudioStatus finish(app::AudioPlayer &p) {
    until([&] { auto s = p.status().state; return s == app::AudioState::ended || s == app::AudioState::error; });
    return p.status();
}
std::vector<float> reference(const std::string &url) {
    reset(); app::AudioPlayer p; assert(p.start(url));
    auto s = finish(p); assert(s.state == app::AudioState::ended);
    std::lock_guard<std::mutex> lock(mutex);
    auto pcm = output; pcm.resize(std::llround(s.position * 48000) * 2); return pcm;
}
void compare(const std::vector<float> &expected) {
    std::lock_guard<std::mutex> lock(mutex);
    assert(opens == 1 && closes == 1 && handle == 0);
    assert(output.size() == ((expected.size() / 2 + 2047) / 2048) * 4096);
    assert(std::equal(expected.begin(), expected.end(), output.begin()));
    for (size_t i = expected.size(); i < output.size(); ++i) assert(output[i] == 0);
}
}
extern "C" {
int sceAudioOutInit() { static bool initialized = false; if (initialized) return int(0x8026000E); initialized = true; return 0; }
int sceAudioOutOpen(int, int, int, unsigned grain, unsigned rate, unsigned format) {
    std::lock_guard<std::mutex> lock(mutex);
    assert(!handle && grain == 2048 && rate == 48000 && format == 4);
    handle = ++opens; return handle;
}
int sceAudioOutClose(int h) { std::lock_guard<std::mutex> lock(mutex); assert(h == handle); handle = 0; ++closes; return 0; }
int sceAudioOutOutput(int h, const void *p) {
    {
        std::lock_guard<std::mutex> lock(mutex); assert(h == handle);
        auto pcm = static_cast<const float *>(p); output.insert(output.end(), pcm, pcm + 4096);
    }
    ++writes; entered = true;
    until([] { return !hold; });
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
    return fail_write == writes ? -1 : 0;
}
}
int main(int argc, char **argv) {
    assert(argc == 2); std::string base = argv[1];
    auto a = reference(base + "/A.flac");
    for (auto ext : {"flac", "mp3", "opus", "m4a"}) {
        auto b = reference(base + "/B." + ext);
        auto expected = a; expected.insert(expected.end(), b.begin(), b.end());
        reset(); hold = true;
        app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/B." + ext));
        until([] { return entered.load(); });
        assert(p.status().track == 0); p.pause(); hold = false;
        std::this_thread::sleep_for(20ms); int before = writes;
        std::this_thread::sleep_for(20ms); assert(writes == before && p.status().track == 0);
        p.resume();
        until([&] { return p.status().track == 1; });
        assert(!p.queue_next("", 0)); assert(p.queue_next("", 1));
        assert(finish(p).state == app::AudioState::ended); compare(expected);
    }
    // Several sub-grain occurrences cannot deadlock waiting for a visible transition.
    {
        auto short_pcm = reference(base + "/short.flac");
        reset(); app::AudioPlayer p; assert(p.start(base + "/short.flac", base + "/short.flac"));
        until([&] { return p.status().next_track == 1; });
        assert(p.status().track == 0); assert(p.queue_next(base + "/short.flac", 1));
        until([&] { return p.status().next_track == 2; }); assert(p.queue_next("", 2));
        assert(finish(p).state == app::AudioState::ended);
        auto expected = short_pcm; expected.insert(expected.end(), short_pcm.begin(), short_pcm.end());
        expected.insert(expected.end(), short_pcm.begin(), short_pcm.end()); compare(expected);
    }
    // Multiple occurrences, same opaque ID, no extra output handles.
    {
        reset(); app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/A.flac"));
        until([&] { return p.status().next_track == 1; }); assert(p.queue_next(base + "/A.flac", 1));
        until([&] { return p.status().next_track == 2; }); assert(p.queue_next("", 2));
        assert(finish(p).state == app::AudioState::ended);
        auto expected = a; expected.insert(expected.end(), a.begin(), a.end()); expected.insert(expected.end(), a.begin(), a.end()); compare(expected);
    }
    // Failed seek rolls current playback back and rebuilds, rather than reuses, prefetch.
    {
        auto b = reference(base + "/B.flac");
        reset(); hold = true; app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/B.flac"));
        until([] { return entered.load(); }); p.pause(); hold = false;
        std::this_thread::sleep_for(20ms);
        assert(p.seek(10000)); until([&] { return !p.status().seeking; }); assert(p.status().seek_failed);
        p.resume(); until([&] { return p.status().next_track == 1; }); assert(p.queue_next("", 1));
        assert(finish(p).state == app::AudioState::ended);
        auto expected = a; expected.insert(expected.end(), b.begin(), b.end()); compare(expected);
    }
    // Failure is terminal, attributed to B without fabricated PCM transition or retries.
    for (auto path : {"/denied", "/bad", "/partial"}) {
        reset(); app::AudioPlayer p; assert(p.start(base + "/A.flac", base + path));
        auto s = finish(p); assert(s.state == app::AudioState::error && s.error_track == 1);
        assert(s.error_code == (std::string(path) == "/denied" ? app::AudioError::auth :
            std::string(path) == "/bad" ? app::AudioError::demux : app::AudioError::transport));
        if (std::string(path) != "/partial") {
            assert(s.track == 0);
            assert(std::llround(s.position * 48000) == static_cast<int64_t>(a.size() / 2));
            compare(a);
        }
        assert(opens == 1 && closes == 1);
    }
    // Pending PCM retains last contributing occurrence across no-PCM failures.
    {
        auto short_pcm = reference(base + "/short.flac");
        reset(); app::AudioPlayer p; assert(p.start(base + "/short.flac", base + "/short.flac"));
        until([&] { return p.status().next_track == 1; }); assert(p.queue_next(base + "/short.flac", 1));
        until([&] { return p.status().next_track == 2; }); assert(p.queue_next(base + "/denied", 2));
        auto s = finish(p);
        assert(s.state == app::AudioState::error && s.error_track == 3 && s.track == 2);
        assert(std::llround(s.position * 48000) == static_cast<int64_t>(short_pcm.size() / 2));
        auto expected = short_pcm; expected.insert(expected.end(), short_pcm.begin(), short_pcm.end());
        expected.insert(expected.end(), short_pcm.begin(), short_pcm.end()); compare(expected);
    }
    // Failed mixed/full/final blocks belong to last PCM occurrence attempted.
    for (int destination : {1, 2}) {
        auto b = reference(base + "/B.flac");
        for (bool final_block : {false, true}) {
            reset();
            const size_t prefix = destination == 1 ? a.size() / 2 : a.size() / 2 + b.size() / 2;
            const size_t total = prefix + b.size() / 2;
            fail_write = final_block ? (total + 2047) / 2048 : prefix / 2048 + 1;
            app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/B.flac"));
            until([&] { return p.status().next_track == 1; });
            assert(p.queue_next(destination == 2 ? base + "/B.flac" : "", 1));
            if (destination == 2) {
                until([&] { return p.status().next_track == 2; }); assert(p.queue_next("", 2));
            }
            auto s = finish(p);
            assert(s.state == app::AudioState::error && s.error_code == app::AudioError::output);
            assert(s.error_track == static_cast<uint64_t>(destination));
            assert(s.track == static_cast<uint64_t>(final_block ? destination : destination - 1));
            assert(opens == 1 && closes == 1);
        }
    }
    // Stalled successor exposes buffering, never invents zeros; stop joins promptly.
    {
        reset(); app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/stall"));
        until([&] { return p.status().buffering && writes > 5; });
        int before = writes; std::this_thread::sleep_for(30ms); assert(writes == before);
        auto begin = std::chrono::steady_clock::now(); p.stop();
        assert(std::chrono::steady_clock::now() - begin < 1s && closes == 1);
    }
    // Seek cancels prepared PCM and rebuilds successor; output after seek is exact.
    {
        auto b = reference(base + "/B.flac");
        reset(); hold = true; app::AudioPlayer p; assert(p.start(base + "/A.flac", base + "/B.flac"));
        until([] { return entered.load(); }); p.pause(); hold = false;
        std::this_thread::sleep_for(20ms);
        assert(p.seek(.125)); until([&] { return !p.status().seeking; }); assert(!p.status().seek_failed);
        { std::lock_guard<std::mutex> lock(mutex); output.clear(); }
        p.resume(); until([&] { return p.status().next_track == 1; }); assert(p.queue_next("", 1));
        assert(finish(p).state == app::AudioState::ended);
        std::vector<float> expected(a.begin() + 6000 * 2, a.end()); expected.insert(expected.end(), b.begin(), b.end());
        { std::lock_guard<std::mutex> lock(mutex); assert(opens == 2 && closes == 2); opens = closes = 1; }
        compare(expected);
    }
    // Full speculative ring must not deadlock paused stop or session replacement.
    {
        reset(); app::AudioPlayer p; assert(p.start(base + "/long.flac", base + "/long.flac"));
        until([] { return writes > 2; }); p.pause(); std::this_thread::sleep_for(30ms);
        auto begin = std::chrono::steady_clock::now(); p.stop(); assert(std::chrono::steady_clock::now() - begin < 1s);
        reset(); assert(p.start(base + "/A.flac", base + "/stall")); until([] { return writes > 1; });
        assert(p.start(base + "/B.flac")); assert(finish(p).state == app::AudioState::ended);
        assert(opens == closes);
    }
    // Successor lookup preserves repeat/shuffle and canonical duplicate occurrence.
    {
        std::vector<subsonic::Song> songs(3); for (auto &s : songs) s.id = "duplicate";
        app::PlayQueue q(songs); assert(q.successor_index() == 1); assert(q.current_index() == 0);
        q.set_repeat(app::Repeat::one); assert(q.successor_index() == 0);
        q.set_repeat(app::Repeat::all); q.next(); q.next(); assert(q.successor_index() == 0);
        std::mt19937 rng(7); q.set_shuffle(true, rng);
        auto index = q.successor_index(); assert(q.on_track_end() && q.current_index() == index);
        q.set_shuffle(false, rng); assert(q.current_index() == index);
    }
    std::cout << "PASS: complete PCM concatenation, mixed rates/codecs/trim, one native handle, final-only padding, pause, seek, errors, buffering, cancellation, occurrences\n";
}
