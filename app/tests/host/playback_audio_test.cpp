/* Navi5 — real worker/queue/HTTP/decoder/native-sink orchestration check.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/smoke_session.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
namespace {
std::mutex sink_mutex;
int active_handle = 0, opens = 0, closes = 0;
bool fail_successor_output = false;
struct Output { int writes = 0, crossings = 0; double peak = 0; };
std::vector<Output> outputs;
std::vector<std::string> events;

template<class F> void until(const char *label, F f) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!f()) {
        if (std::chrono::steady_clock::now() >= end) {
            std::cerr << "Timed out: " << label << '\n';
            std::lock_guard<std::mutex> lock(sink_mutex);
            for (const auto &event : events) std::cerr << event << '\n';
            assert(false);
        }
        std::this_thread::sleep_for(2ms);
    }
}
}

// Exercise AudioPlayer's actual NativeSink, not an injected AudioSink.
extern "C" {
// PS5 returns SCE_AUDIO_OUT_ERROR_ALREADY_INIT on every init after the first.
int sceAudioOutInit() { static int calls = 0; return calls++ ? int(0x8026000E) : 0; }
int sceAudioOutOpen(int user, int type, int index, unsigned grain, unsigned rate, unsigned format) {
    assert(user == 0xFF && type == 0 && index == 0);
    assert(grain == 2048 && rate == 48000 && format == 4);
    std::lock_guard<std::mutex> lock(sink_mutex);
    assert(active_handle == 0); // Handles must never overlap.
    active_handle = ++opens;
    outputs.emplace_back();
    events.push_back("open " + std::to_string(active_handle));
    return active_handle;
}
int sceAudioOutOutput(int handle, const void *pcm) {
    assert(pcm);
    {
        std::lock_guard<std::mutex> lock(sink_mutex);
        assert(handle == active_handle && handle > 0);
        auto &output = outputs.at(handle - 1);
        const auto *samples = static_cast<const float *>(pcm);
        for (int i = 0; i < 4096; ++i) {
            assert(std::isfinite(samples[i]));
            output.peak = std::max(output.peak, std::abs(double(samples[i])));
            if (i >= 2 && i % 2 == 0 && samples[i - 2] < 0 && samples[i] >= 0)
                ++output.crossings;
        }
        if (++output.writes == 1) events.push_back("write " + std::to_string(handle));
        // A has 28800 frames. Write 15 first attempts mixed A/B PCM.
        if (fail_successor_output && output.writes == 15) return -1;
    }
    // Native blocking output pacing makes both playing states observable by the
    // real worker's 100ms polling, without injecting EOF/status or a second Play.
    std::this_thread::sleep_for(42ms);
    return 0;
}
int sceAudioOutClose(int handle) {
    std::lock_guard<std::mutex> lock(sink_mutex);
    assert(handle > 0 && handle == active_handle);
    events.push_back("close " + std::to_string(handle));
    active_handle = 0;
    ++closes;
    return 0;
}
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const std::string base = argv[1], second = argv[2];
    assert(base.rfind("http://127.0.0.1:", 0) == 0);
    assert(second == "B" || second == "B-bad" || second == "B-denied" || second == "B-output");
    fail_successor_output = second == "B-output";
    const bool success = second == "B";
    using namespace app::smoke;
    auto box = std::make_shared<Mailbox>();
    // Only metadata transport is fake. Real Subsonic adapter builds stream URLs,
    // Library builds the two-song context, and PlayQueue consumes natural EOF.
    auto metadata = [second](const std::string &url) {
        std::string detail;
        if (url.find("/rest/getAlbum?") != std::string::npos) {
            detail = ",\"album\":{\"id\":\"fixture\",\"song\":["
                     "{\"id\":\"A\",\"title\":\"Track A\"},"
                     "{\"id\":\"" + second + "\",\"title\":\"Track B\"}]}";
        } else assert(url.find("/rest/ping?") != std::string::npos);
        return subsonic::HttpResponse{200, "{\"subsonic-response\":{\"status\":\"ok\"" + detail + "}}", {}};
    };
    assert(start_worker(box, metadata));
    auto send = [&](Command command) {
        command.generation = box->generation;
        assert(box->send(std::move(command)));
    };
    Command login; login.kind = Command::Login; login.server = base;
    login.username = "fixture"; login.password = "fixture";
    send(login);
    until("login", [&] {
        std::lock_guard<std::mutex> lock(box->mutex);
        if (box->results.empty()) return false;
        assert(box->results.front().kind == Command::Login && box->results.front().ok);
        return true;
    });
    auto wait_status = [&](app::AudioState state, const std::string &title) {
        until(title.c_str(), [&] {
            std::lock_guard<std::mutex> lock(box->mutex);
            if (box->audio.state == app::AudioState::error && state != app::AudioState::error)
                std::cerr << "Unexpected error: " << box->audio.error << '\n';
            return box->audio.state == state && box->title == title;
        });
    };
    Command play; play.kind = Command::Play;
    play.play = {"A", "fixture", {}, ui::music::View::AlbumTracks, 0, 0, {}};
    send(play); // Only Play in this process. Everything after A is automatic.
    wait_status(app::AudioState::playing, "Track A");
    if (success) wait_status(app::AudioState::playing, "Track B");
    const auto terminal = success ? app::AudioState::ended : app::AudioState::error;
    wait_status(terminal, "Track B");
    {
        std::lock_guard<std::mutex> lock(box->mutex);
        assert(box->audio.error_code == (success ? app::AudioError::none :
               fail_successor_output ? app::AudioError::output :
               second == "B-bad" ? app::AudioError::demux : app::AudioError::auth));
        if (fail_successor_output) assert(box->audio.error_track == 1 && box->audio.track == 0);
        if (success) assert(box->audio.position > .55 && box->audio.position < .7);
        else assert(!box->audio.error.empty());
    }
    // Several worker ticks must leave final EOF/error intact, with no retry,
    // wraparound, or skip back to A. Server independently checks request counts.
    std::this_thread::sleep_for(450ms);
    {
        std::lock_guard<std::mutex> lock(box->mutex);
        assert(box->audio.state == terminal && box->title == "Track B");
        box->quit = true;
        box->wake.notify_one();
    }
    std::weak_ptr<Mailbox> worker_done = box;
    box.reset();
    until("worker destruction", [&] { return worker_done.expired(); });
    {
        std::lock_guard<std::mutex> lock(sink_mutex);
        assert(active_handle == 0 && opens == closes && opens == 1);
        assert(outputs[0].writes >= 10 && outputs[0].peak > .01);
        assert(success ? (outputs[0].crossings > 730 && outputs[0].crossings < 850) :
                         (outputs[0].crossings > 240 && outputs[0].crossings < (fail_successor_output ? 340 : 290)));
        assert((events == std::vector<std::string>{"open 1", "write 1", "close 1"}));
    }
    std::cout << "PASS: " << second << ": natural EOF advances A to B; "
              << (success ? "distinct PCM, exclusive balanced native handles, final ended" :
                            "typed terminal error, no B output") << '\n';
}
