/* Navi5 — range integrity and sample-aligned seek regression checks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../../src/app/audio_player.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
extern "C" {
int sceAudioOutInit() { assert(false); return -1; }
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned) { assert(false); return -1; }
int sceAudioOutOutput(int, const void *) { assert(false); return -1; }
int sceAudioOutClose(int) { assert(false); return -1; }
}
using namespace std::chrono_literals;
template<class F> void until(F f) {
    auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!f()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}
struct Capture : app::AudioSink {
    std::mutex mutex;
    std::vector<float> samples;
    bool open() override { std::lock_guard<std::mutex> lock(mutex); samples.clear(); return true; }
    bool close() override { return true; }
    bool write(const float *p) override {
        { std::lock_guard<std::mutex> lock(mutex); samples.insert(samples.end(), p, p + 4096); }
        std::this_thread::sleep_for(1ms);
        return true;
    }
};
int main(int argc, char **argv) {
    assert(argc == 2);
    std::string base = argv[1];
    std::atomic<bool> cancel{false};
    auto header = [](app::Stream &s, std::string h) { app::Stream::header(h.data(), 1, h.size(), &s); };
    for (auto value : {"bytes 0-99/100garbage", "bytes +0-99/100", "bytes 0-99/9223372036854775808",
                       "bytes 0--1/100", "bytes 0-100/100", "bytes 0-99/+100", "broken"}) {
        app::Stream s(cancel); s.ranged = true;
        header(s, std::string("Content-Range: ") + value + "\r\n");
        assert(!s.valid_extent());
    }
    {
        app::Stream s(cancel); s.ranged = true;
        header(s, "Content-Range: bytes 0-99/100\r\n");
        assert(s.valid_extent());
        header(s, "Content-Range: broken\r\n"); assert(!s.valid_extent());
        header(s, "HTTP/1.1 206 Partial Content\r\n");
        assert(s.length == -1 && !s.valid_extent());
        header(s, "Content-Range: bytes 0-99/100\r\n");
        header(s, "Content-Length: 1\r\n"); assert(!s.valid_extent());
    }
    for (auto path : {"/short", "/truncated", "/wrong-total", "/duplicate", "/overflow-length"}) {
        app::Stream s(cancel); s.known_length = 100; s.open(base + path, 0);
        uint8_t bytes[200]; int n;
        do { n = app::Stream::read(&s, bytes, sizeof(bytes)); } while (n > 0);
        assert(n == AVERROR(EIO) && s.failed && s.error == app::AudioError::transport);
    }
    for (auto file : {"/track.flac", "/track.mp3", "/track.opus", "/track.m4a"}) {
        Capture sink; app::AudioPlayer p(&sink);
        auto finish = [&] { until([&] { auto s = p.status().state; return s == app::AudioState::ended || s == app::AudioState::error; }); assert(p.status().state == app::AudioState::ended); };
        assert(p.start(base + file)); finish();
        auto reference = sink.samples;
        const int64_t frames = std::llround(p.status().position * 48000);
        for (int64_t target : {int64_t(96000), frames - 8}) {
            assert(p.start(base + file));
            until([&] { return p.status().state == app::AudioState::playing; });
            p.pause(); std::this_thread::sleep_for(10ms);
            assert(p.seek(target / 48000.0));
            until([&] { return !p.status().seeking; }); assert(!p.status().seek_failed);
            p.resume(); finish();
            assert(sink.samples.size() == static_cast<size_t>(((frames - target + 2047) / 2048) * 4096));
            for (int64_t i = 0; i < (frames - target) * 2; ++i)
                assert(sink.samples[i] == reference[target * 2 + i]);
            for (size_t i = (frames - target) * 2; i < sink.samples.size(); ++i) assert(sink.samples[i] == 0);
        }
    }
    // Actual replacement transport failure must survive demux EOF and retain diagnostics.
    {
        Capture sink; app::AudioPlayer p(&sink);
        assert(p.start(base + "/late.flac"));
        until([&] { return p.status().state == app::AudioState::playing; });
        p.pause(); std::this_thread::sleep_for(10ms);
        assert(p.seek(1)); until([&] { return !p.status().seeking; });
        assert(!p.status().seek_failed); p.resume();
        until([&] { return p.status().state == app::AudioState::error; });
        auto s = p.status();
        assert(s.error_code == app::AudioError::transport && s.http_status == 206 && s.curl_result == CURLE_PARTIAL_FILE);
    }
    {
        app::Stream root(cancel); root.open(base + "/deny.flac");
        root.replacement = std::make_unique<app::Stream>(cancel);
        root.replacement->open(base + "/deny.flac", 0);
        uint8_t byte;
        assert(app::Stream::read(&root, &byte, 1) == AVERROR(EIO));
        assert(root.active().error == app::AudioError::auth && root.active().http_status == 403);
    }
    {
        app::Stream root(cancel); root.open(base + "/track.flac");
        assert(app::Stream::seek(&root, 1, SEEK_SET) == 1);
        assert(root.replacement);
        uint8_t bytes[32768]; int n;
        do { n = app::Stream::read(&root, bytes, sizeof(bytes)); } while (n > 0);
        assert(n == AVERROR_EOF && root.active().done && !root.active().failed);
    }
    std::cout << "PASS: strict range grammar/extents/completion; exact sequential PCM suffix including resampler tail (FLAC/MP3/Opus/AAC)\n";
}
