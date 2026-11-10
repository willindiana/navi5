/* Navi5 — actual decoder checks. SPDX-License-Identifier: GPL-3.0-or-later */
#include "app/audio_player.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
#include <curl/curl.h>

extern "C" {
int sceAudioOutInit() { assert(false); return -1; }
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned) { assert(false); return -1; }
int sceAudioOutOutput(int, const void *) { assert(false); return -1; }
int sceAudioOutClose(int) { assert(false); return -1; }
}
using namespace std::chrono_literals;
struct Sink : app::AudioSink {
    std::atomic<int> blocks{0}, opens{0}, closes{0};
    std::atomic<bool> fail{false}, slow{false}, hold_open{false}, opening{false};
    std::atomic<double> peak{0};
    bool open() override {
        opening = true;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (hold_open) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        opening = false;
        ++opens;
        return true;
    }
    bool write(const float *p) override {
        for (int i = 0; i < 4096; ++i) {
            assert(std::isfinite(p[i]));
            peak = std::max(peak.load(), std::abs(static_cast<double>(p[i])));
        }
        ++blocks;
        if (slow) std::this_thread::sleep_for(2ms);
        return !fail;
    }
    bool close() override { ++closes; return true; }
};
template<class F> void until(F f) {
    auto end = std::chrono::steady_clock::now() + 20s;
    while (!f()) {
        assert(std::chrono::steady_clock::now() < end);
        std::this_thread::sleep_for(1ms);
    }
}
app::AudioStatus finish(app::AudioPlayer &p) {
    until([&] { auto s = p.status().state; return s == app::AudioState::ended || s == app::AudioState::error; });
    return p.status();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    std::string base = argv[1];
    Sink sink;
    app::AudioPlayer p(&sink);
    for (auto file : {"tone.mp3", "tone.flac", "tone.aac", "tone.opus", "tone.m4a"}) {
        sink.blocks = 0; sink.peak = 0;
        assert(p.start(base + "/" + file));
        auto s = finish(p);
        if (s.state != app::AudioState::ended) std::cerr << file << ": " << s.error << '\n';
        assert(s.state == app::AudioState::ended);
        assert(s.error_code == app::AudioError::none && s.curl_result == 0 && s.http_status == 0);
        assert(s.multi_result == 0 && s.multi_op == app::AudioMultiOp::none);
        assert(s.position > .20 && s.position < .40);
        assert(sink.blocks >= 5 && sink.peak > .01);
        assert(sink.opens == sink.closes);
    }
    for (auto path : {"/bad", "/redirect", "/denied", "/unauthorized", "/partial", "/missing"}) {
        assert(p.start(base + path));
        auto s = finish(p);
        assert(s.state == app::AudioState::error && !s.error.empty());
        assert(s.error.find(base) == std::string::npos);
        auto expected = std::string(path) == "/bad" ? app::AudioError::demux :
            (std::string(path) == "/denied" || std::string(path) == "/unauthorized") ?
                app::AudioError::auth : app::AudioError::transport;
        assert(s.error_code == expected);
        const int expected_http = std::string(path) == "/bad" ? 0 :
            std::string(path) == "/redirect" ? 302 : std::string(path) == "/denied" ? 403 :
            std::string(path) == "/unauthorized" ? 401 : std::string(path) == "/partial" ? 200 : 404;
        assert(s.http_status == expected_http);
        assert(s.multi_result == 0 && s.multi_op == app::AudioMultiOp::none);
        if (std::string(path) == "/partial") {
            assert(s.error == "HTTP transfer incomplete");
            assert(s.curl_result == CURLE_PARTIAL_FILE);
        } else if (std::string(path) == "/bad" || std::string(path) == "/redirect") {
            assert(s.curl_result == CURLE_OK);
        } else {
            assert(s.curl_result == CURLE_WRITE_ERROR);
        }
        assert(sink.opens == sink.closes);
    }
    for (auto url : {"file:///etc/passwd", "ftp://localhost/a", "not a URL"}) {
        assert(p.start(url));
        auto invalid = finish(p);
        assert(invalid.state == app::AudioState::error && invalid.error_code == app::AudioError::transport);
        assert(invalid.curl_result == 0 && invalid.http_status == 0);
        assert(invalid.multi_result == 0 && invalid.multi_op == app::AudioMultiOp::none);
    }
    assert(p.start(base + "/unsupported"));
    auto unsupported = finish(p);
    assert(unsupported.state == app::AudioState::error && unsupported.error_code == app::AudioError::decode);
    assert(unsupported.curl_result == 0 && unsupported.http_status == 0);
    assert(unsupported.multi_result == 0 && unsupported.multi_op == app::AudioMultiOp::none);
    // Hold actual output opening: pause must be visible to UI toggles, and
    // resume must not claim playing until worker finishes opening output.
    sink.hold_open = true;
    const auto before_open_blocks = sink.blocks.load();
    assert(p.start(base + "/tone.flac"));
    until([&] { return sink.opening.load(); });
    assert(p.status().state == app::AudioState::opening);
    for (int i = 0; i < 2; ++i) {
        p.pause();
        assert(p.status().state == app::AudioState::paused);
        p.resume();
        assert(p.status().state == app::AudioState::opening);
    }
    p.pause();
    sink.hold_open = false;
    until([&] { return !sink.opening.load(); });
    std::this_thread::sleep_for(20ms);
    assert(p.status().state == app::AudioState::paused && sink.blocks == before_open_blocks);
    p.resume();
    assert(finish(p).state == app::AudioState::ended);
    assert(sink.blocks > before_open_blocks && sink.opens == sink.closes);
    // Replace already-prefetched bad decoder while paused. No current restart,
    // no old successor promotion; same-URL replacement remains occurrence-safe.
    sink.slow = true;
    assert(p.start(base + "/large.flac", base + "/bad"));
    until([&] { return p.status().position > 0; });
    p.pause();
    std::this_thread::sleep_for(20ms);
    auto anchor = p.status();
    assert(anchor.track == 0 && anchor.next_track == 0);
    assert(p.replace_next(base + "/tone.flac", 0));
    assert(!p.replace_next(base + "/bad", 1));
    std::this_thread::sleep_for(30ms);
    assert(p.status().state == app::AudioState::paused && p.status().position == anchor.position);
    assert(p.seek(119.5, 0));
    assert(!p.replace_next(base + "/bad", 0)); // Seek owns producer rebuild.
    until([&] { return !p.status().seeking; });
    assert(p.replace_next(base + "/tone.flac", 0));
    p.resume();
    until([&] { return p.status().next_track == 1; });
    assert(!p.replace_next(base + "/bad", 0)); // Handoff committed.
    assert(p.queue_next({}, 1));
    assert(finish(p).state == app::AudioState::ended);
    p.stop();
    assert(!p.replace_next(base + "/tone.flac", 0));
    sink.slow = false;
    sink.fail = true;
    assert(p.start(base + "/tone.flac"));
    auto failure = finish(p);
    assert(failure.state == app::AudioState::error && failure.error_code == app::AudioError::output);
    assert(failure.curl_result == 0 && failure.http_status == 0);
    assert(failure.multi_result == 0 && failure.multi_op == app::AudioMultiOp::none);
    sink.fail = false;
    sink.slow = true;
    sink.blocks = 0;
    assert(p.start(base + "/large.flac"));
    until([&] { return sink.blocks >= 3; });
    p.pause();
    std::this_thread::sleep_for(20ms); // one in-flight write allowed
    auto blocks = sink.blocks.load();
    std::this_thread::sleep_for(30ms);
    assert(p.status().state == app::AudioState::paused && sink.blocks == blocks);
    p.resume();
    until([&] { return sink.blocks > blocks; });
    p.pause();
    auto begin = std::chrono::steady_clock::now();
    *p.cancel_flag() = true; // Account reset cancels without condition notification.
    until([&] { return p.status().state == app::AudioState::stopped; });
    p.stop();
    assert(std::chrono::steady_clock::now() - begin < 2s);
    assert(p.status().state == app::AudioState::stopped && sink.opens == sink.closes);
    sink.slow = false;
    assert(p.start(base + "/large.flac"));
    auto s = finish(p);
    assert(s.state == app::AudioState::ended && s.position > 119.9 && s.position < 120.1);
    assert(sink.opens == sink.closes);
    assert(p.start(base + "/stall"));
    std::this_thread::sleep_for(100ms);
    begin = std::chrono::steady_clock::now();
    p.stop();
    assert(std::chrono::steady_clock::now() - begin < 2s);
    assert(p.start(base + "/large.flac"));
    assert(p.start(base + "/tone.flac")); // cancel/replace
    assert(finish(p).state == app::AudioState::ended);
    {
        app::AudioPlayer doomed(&sink);
        assert(doomed.start(base + "/stall"));
    }
    assert(sink.opens == sink.closes);
    std::cout << "PASS: real MP3/FLAC/AAC/Opus decode, streaming >16MiB, pause/resume, stop, replacement, destruction, HTTP/decode/output errors\n";
}
