/*
 * Navi5 — continuous-output audio session
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstdint>
#include <atomic>
#include <memory>
#include <pthread.h>
#include <string>

namespace app {

// Worker-only calls. write consumes exactly 2048 stereo float frames at 48 kHz;
// must return in bounded time. Sink outlives Player. nullptr selects sceAudioOut.
struct AudioSink {
    virtual ~AudioSink() = default;
    virtual bool open() = 0;
    virtual bool write(const float *stereo) = 0;
    virtual bool close() = 0;
};

enum class AudioState { stopped, opening, playing, paused, ended, error };
enum class AudioError { none = 0, transport = 1, auth = 2, demux = 3, decode = 4, output = 5 };
enum class AudioMultiOp { none = 0, add = 1, perform = 2, poll = 3 };
struct AudioStatus {
    AudioState state = AudioState::stopped;
    double position = 0; // submitted media frames, excluding final zero padding
    std::string error; // fixed local descriptions only, never URLs/server text
    AudioError error_code = AudioError::none;
    // Numeric transport failure details only; zero means unknown/not applicable.
    int curl_result = 0;
    int http_status = 0;
    int multi_result = 0;
    AudioMultiOp multi_op = AudioMultiOp::none;
    double duration = 0; // Finite decoder duration; zero means unknown/unavailable.
    bool seeking = false;
    // Last asynchronous seek outcome; failures leave current playback intact.
    bool seek_failed = false;
    uint64_t track = 0; // Session occurrence, advanced after successor PCM submission.
    uint64_t next_track = 0; // Occurrence whose successor may now be declared.
    bool buffering = false; // Waiting for PCM/next declaration; no inserted silence.
    // Decoder failure: failing occurrence, even without PCM. Output write failure:
    // last PCM occurrence in attempted block (including mixed-boundary blocks).
    uint64_t error_track = 0;
};

class AudioPlayer {
public:
    explicit AudioPlayer(AudioSink *sink = nullptr);
    virtual ~AudioPlayer();
    AudioPlayer(const AudioPlayer &) = delete;
    AudioPlayer &operator=(const AudioPlayer &) = delete;

    // Control calls must be serialized by caller. start replaces current track.
    // URL comes from Client::stream_url; never logged or passed to FFmpeg.
    // HTTPS verified; HTTP is explicitly PLAINTEXT, including credentials.
    // start/stop/destruction cancel and join session plus both bounded producers
    // (normally <= two 100ms polls plus one sink write). Keep off render thread.
    // Initial next_url is prefetched immediately. Empty means final track.
    virtual bool start(std::string url, std::string next_url = {});
    // Declare successor after observing new status.next_track. Empty URL ends session.
    // Occurrence guard rejects stale declarations; one bounded successor only.
    virtual bool queue_next(std::string url, uint64_t track);
    // Atomically replace speculative successor, never current PCM. Refuses handoff,
    // seek, terminal or stale occurrence. Cancellation/join happens on audio worker.
    virtual bool replace_next(std::string url, uint64_t track);
    virtual void stop();
    virtual void pause();
    virtual void resume();
    virtual AudioStatus status() const;
    // Queue absolute media seconds on worker. false: invalid, busy, terminal,
    // or natural handoff pending its first PCM submission.
    // expected_track atomically rejects stale UI occurrence; UINT64_MAX is unguarded.
    // Accepted != completed: poll status().seeking/seek_failed. Pause is retained.
    // Decode from origin preserves codec trim/resampler history. Target rounds to
    // nearest 48 kHz frame; CPU/network work scales with target, not constant-time.
    // Outcome reports decoder replacement, before forward discard completes.
    virtual bool seek(double seconds, uint64_t expected_track = UINT64_MAX);
    virtual std::shared_ptr<std::atomic<bool>> cancel_flag();
private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    pthread_t worker_{};
    bool joinable_ = false;
};
} // namespace app
