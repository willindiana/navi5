/* Navi5 — real seek content checks. SPDX-License-Identifier: GPL-3.0-or-later */
#include "app/audio_player.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
extern "C" {
int sceAudioOutInit() { assert(false); return -1; }
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned) { assert(false); return -1; }
int sceAudioOutOutput(int, const void *) { assert(false); return -1; }
int sceAudioOutClose(int) { assert(false); return -1; }
}
using namespace std::chrono_literals;
template<class F> void until(F f) {
    auto end = std::chrono::steady_clock::now() + 15s;
    while (!f()) { assert(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(1ms); }
}
struct Sink : app::AudioSink {
    std::atomic<int> blocks{0}, crossings{0}, opens{0}, closes{0};
    bool open() override { ++opens; return true; }
    bool close() override { ++closes; return true; }
    bool write(const float *p) override {
        int count = 0;
        for (int i = 2; i < 4096; i += 2) {
            assert(std::isfinite(p[i]));
            if ((p[i] >= 0) != (p[i-2] >= 0)) ++count;
        }
        crossings = count;
        ++blocks;
        std::this_thread::sleep_for(3ms);
        return true;
    }
};
int main(int argc, char **argv) {
    assert(argc == 2);
    std::string base = argv[1];
    Sink sink;
    app::AudioPlayer p(&sink);
    assert(!p.seek(1));
    for (auto file : {"/track.flac", "/track.m4a"}) {
        sink.crossings = 0;
        assert(p.start(base + file));
        until([&] { return p.status().state == app::AudioState::playing; });
        until([&] { return sink.crossings > 10; });
        assert(sink.crossings < 30);
        assert(p.status().duration > 0 && std::isfinite(p.status().duration));
        assert(!p.seek(1, p.status().track + 1)); // Atomic occurrence guard.
        assert(!p.seek(-1) && !p.seek(std::numeric_limits<double>::quiet_NaN()));
        assert(!p.seek(std::numeric_limits<double>::infinity()));
        p.pause();
        std::this_thread::sleep_for(20ms);
        for (double target : {18.0, 2.0, 20.0, 1.0}) {
            assert(p.seek(target));
            until([&] { return !p.status().seeking; });
            auto s = p.status();
            assert(!s.seek_failed && s.state == app::AudioState::paused);
            assert(std::abs(s.position - target) < .001);
            int before = sink.blocks;
            std::this_thread::sleep_for(20ms);
            assert(sink.blocks == before);
            p.resume();
            until([&] { return sink.blocks >= before + 3; });
            p.pause();
            std::this_thread::sleep_for(20ms);
            // Check actual decoded PCM frequency, not reported position.
            assert(target > 10 ? sink.crossings > 130 : sink.crossings < 30);
        }
        assert(p.seek(10000));
        until([&] { return !p.status().seeking; });
        assert(p.status().seek_failed && p.status().state == app::AudioState::paused);
        assert(p.seek(file == std::string("/track.flac") ? 29.9 : 239.9));
        until([&] { return !p.status().seeking; });
        assert(!p.status().seek_failed);
        p.resume();
        until([&] { return p.status().state == app::AudioState::ended || p.status().state == app::AudioState::error; });
        assert(p.status().state == app::AudioState::ended);
        assert(!p.seek(1));
        p.stop();
    }
    for (auto path : {"/ignore.flac", "/wrong.flac", "/stall.flac"}) {
        assert(p.start(base + path));
        until([&] { return p.status().state == app::AudioState::playing; });
        p.pause();
        std::this_thread::sleep_for(20ms);
        auto before = p.status().position;
        assert(p.seek(18));
        if (std::string(path) == "/stall.flac") {
            std::this_thread::sleep_for(50ms);
            auto start = std::chrono::steady_clock::now();
            p.stop();
            assert(std::chrono::steady_clock::now() - start < 1s);
            assert(!p.status().seeking && p.status().state == app::AudioState::stopped);
        } else {
            until([&] { return !p.status().seeking; });
            assert(p.status().seek_failed && p.status().position == before);
            int count = sink.blocks;
            p.resume();
            until([&] { return sink.blocks > count + 2; });
            assert(p.status().state == app::AudioState::playing && sink.crossings < 30);
            p.stop();
        }
    }
    assert(sink.opens == sink.closes);
    std::cout << "PASS: forward/back PCM content, paused seek, EOF, rollback, invalid input, cancellation\n";
}
