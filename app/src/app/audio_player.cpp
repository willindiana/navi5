/*
 * Navi5 — bounded curl AVIO / FFmpeg / sceAudioOut playback
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio_player.h"
#include "../../engine/media/include/evo_thread.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <vector>
#include <curl/curl.h>
#include "../curl_sockopt.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/mem.h>
int sceAudioOutInit(void);
int sceAudioOutOpen(int, int, int, unsigned, unsigned, unsigned);
int sceAudioOutOutput(int, const void *);
int sceAudioOutClose(int);
}
#ifndef NAVI5_CA_BUNDLE
#define NAVI5_CA_BUNDLE "/data/navi5/ca-bundle.pem"
#endif
namespace app {
namespace {
// Same port layout/grain as retained evo_audio_open_port; independent ownership
// avoids legacy global playback flags and volatile PCM ring.
constexpr int grain = 2048;
struct NativeSink : AudioSink {
    int handle = -1;
    bool floating = true;
    bool open() override {
        // Later inits return 0x8026000E ALREADY_INIT; library stays usable. Strict check
        // failed every second track on hardware (DEV audio=5); SDL PS5 inits once.
        int init = sceAudioOutInit();
        if (init < 0 && init != static_cast<int>(0x8026000E)) return false;
        handle = sceAudioOutOpen(0xFF, 0, 0, grain, 48000, 4);
        floating = handle >= 1;
        if (!floating) handle = sceAudioOutOpen(0xFF, 0, 0, grain, 48000, 1);
        return handle >= 1;
    }
    bool write(const float *p) override {
        if (floating) return sceAudioOutOutput(handle, p) >= 0;
        std::array<int16_t, grain * 2> pcm;
        for (size_t i = 0; i < pcm.size(); ++i) {
            float v = std::isfinite(p[i]) ? std::clamp(p[i], -1.f, 1.f) * 32767.f : 0.f;
            pcm[i] = static_cast<int16_t>(v);
        }
        return sceAudioOutOutput(handle, pcm.data()) >= 0;
    }
    bool close() override {
        bool ok = handle < 1 || sceAudioOutClose(handle) >= 0;
        handle = -1;
        return ok;
    }
};
struct Failure { const char *message; };
void require(bool ok, const char *message) {
    if (!ok) throw Failure{message};
}

// Only this object sees credential URL. FFmpeg gets bytes, no URL,
// no protocol access, and no ability to open nested files/playlists.
struct Stream {
    CURL *easy = nullptr;
    CURLM *multi = nullptr;
    std::atomic<bool> &cancel;
    std::array<unsigned char, 256 * 1024> bytes{};
    size_t used = 0;
    bool paused = false, done = false, failed = false;
    AudioError error = AudioError::none;
    int curl_result = 0, http_status = 0, multi_result = 0;
    AudioMultiOp multi_op = AudioMultiOp::none;
    const char *message = "HTTP transfer failed";
    std::string url;
    int64_t offset = 0, requested_offset = 0, length = -1, range_start = -1;
    int64_t range_end = -1, content_length = -1, received = 0, known_length = -1;
    bool ranged = false, saw_range = false, saw_length = false, invalid_headers = false;
    Stream &active() { return replacement ? replacement->active() : *this; }
    static bool number(const std::string &s, size_t &i, int64_t &value) {
        size_t begin = i;
        value = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            int digit = s[i++] - '0';
            if (value > (INT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return i > begin;
    }
    bool valid_extent() const {
        return !invalid_headers && (!ranged || (saw_range && range_start == requested_offset &&
            range_end == length - 1 && (known_length < 0 || length == known_length) &&
            (!saw_length || content_length == length - range_start)));
    }
    static size_t header(char *p, size_t size, size_t count, void *opaque) {
        auto &s = *static_cast<Stream *>(opaque);
        if (size && count > SIZE_MAX / size) return 0;
        size_t n = size * count;
        std::string line(p, n);
        if (line.rfind("HTTP/", 0) == 0) {
            s.range_start = s.range_end = s.length = s.content_length = -1;
            s.saw_range = s.saw_length = s.invalid_headers = false;
        }
        auto value = [&](size_t prefix) {
            std::string v = line.substr(prefix);
            size_t end = v.find_last_not_of(" \t\r\n");
            v = end == std::string::npos ? "" : v.substr(0, end + 1);
            size_t begin = v.find_first_not_of(" \t");
            return begin == std::string::npos ? std::string{} : v.substr(begin);
        };
        if (line.size() >= 15 && strncasecmp(line.c_str(), "Content-Length:", 15) == 0) {
            std::string v = value(15);
            size_t i = 0;
            if (s.saw_length || !number(v, i, s.content_length) || i != v.size()) s.invalid_headers = true;
            s.saw_length = true;
            if (!s.ranged) s.length = s.content_length;
        }
        if (line.size() >= 14 && strncasecmp(line.c_str(), "Content-Range:", 14) == 0) {
            std::string v = value(14);
            size_t i = 6;
            bool ok = v.rfind("bytes ", 0) == 0 && number(v, i, s.range_start) &&
                i < v.size() && v[i++] == '-' && number(v, i, s.range_end) &&
                i < v.size() && v[i++] == '/' && number(v, i, s.length) && i == v.size() &&
                s.length > 0 && s.range_start <= s.range_end && s.range_end < s.length;
            if (s.saw_range || !ok) s.invalid_headers = true;
            s.saw_range = true;
        }
        return n;
    }
    void check_easy(CURLcode result, const char *description) {
        if (result == CURLE_OK) return;
        curl_result = static_cast<int>(result);
        transport_failure();
        message = description;
        throw Failure{message};
    }
    bool check_multi(CURLMcode result, AudioMultiOp op) {
        if (result == CURLM_OK) return true;
        multi_result = static_cast<int>(result);
        multi_op = op;
        transport_failure();
        return false;
    }
    explicit Stream(std::atomic<bool> &c) : cancel(c) {}
    ~Stream() {
        if (multi && easy) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
    }
    static size_t body(char *p, size_t size, size_t count, void *opaque) {
        auto &s = *static_cast<Stream *>(opaque);
        long status = 0;
        curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
        if (s.cancel || !s.valid_extent() || (s.ranged ? status != 206 : status != 200)) return 0;
        if (size && count > (s.bytes.size() - s.used) / size) {
            s.paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        size_t n = size * count;
        if (n > static_cast<uint64_t>(INT64_MAX - s.received)) return 0;
        s.received += static_cast<int64_t>(n);
        if (s.ranged && s.received > s.length - s.range_start) return 0;
        memcpy(s.bytes.data() + s.used, p, n);
        s.used += n;
        return n;
    }
    static int progress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<Stream *>(p)->cancel ? 1 : 0;
    }
    void open(const std::string &address, int64_t start = -1) {
        url = address;
        ranged = start >= 0;
        requested_offset = offset = ranged ? start : 0;
        const auto &url = this->url;
        require((url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) &&
                url.find_first_of("\r\n") == std::string::npos &&
                url.find('\0') == std::string::npos, "Invalid stream URL");
        // PacBrew 0.39 curl 8.18.0 reports CURL_VERSION_THREADSAFE (curl-config
        // --features): global init is thread-safe, including jf_http's lazy init.
        // Recheck this feature before replacing the native curl build.
        static const CURLcode init = curl_global_init(CURL_GLOBAL_DEFAULT);
        check_easy(init, "HTTP initialization failed");
        easy = curl_easy_init();
        multi = curl_multi_init();
        require(easy && multi, "HTTP initialization failed");
        auto opt = [&](CURLoption key, auto value) {
            check_easy(curl_easy_setopt(easy, key, value), "HTTP configuration failed");
        };
        opt(CURLOPT_URL, url.c_str());
        opt(CURLOPT_PROTOCOLS_STR, "http,https");
        opt(CURLOPT_REDIR_PROTOCOLS_STR, "https");
        opt(CURLOPT_FOLLOWLOCATION, 0L);
        opt(CURLOPT_SSL_VERIFYPEER, 1L);
        opt(CURLOPT_SSL_VERIFYHOST, 2L);
        opt(CURLOPT_CAINFO, NAVI5_CA_BUNDLE);
        opt(CURLOPT_CAPATH, static_cast<const char *>(nullptr));
        opt(CURLOPT_PROXY, "");
        opt(CURLOPT_VERBOSE, 0L);
        opt(CURLOPT_NOSIGNAL, 1L);
#ifdef EVO_TARGET_PS5
        opt(CURLOPT_SOCKOPTFUNCTION, jf::curl_sockopt);
#endif
        opt(CURLOPT_CONNECTTIMEOUT, 15L);
        opt(CURLOPT_LOW_SPEED_LIMIT, 1L);
        opt(CURLOPT_LOW_SPEED_TIME, 30L);
        opt(CURLOPT_WRITEFUNCTION, body);
        opt(CURLOPT_WRITEDATA, this);
        opt(CURLOPT_HEADERFUNCTION, header);
        opt(CURLOPT_HEADERDATA, this);
        if (ranged) {
            std::string range = std::to_string(start) + "-";
            opt(CURLOPT_RANGE, range.c_str());
        }
        opt(CURLOPT_XFERINFOFUNCTION, progress);
        opt(CURLOPT_XFERINFODATA, this);
        opt(CURLOPT_NOPROGRESS, 0L);
        require(check_multi(curl_multi_add_handle(multi, easy), AudioMultiOp::add),
                "HTTP initialization failed");
    }
    int transport_failure() {
        long status = 0;
        if (easy && curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK)
            http_status = status >= 100 && status <= 599 ? static_cast<int>(status) : 0;
        failed = true;
        error = AudioError::transport;
        return AVERROR(EIO);
    }
    std::unique_ptr<Stream> replacement;
    static int64_t seek(void *opaque, int64_t pos, int whence) {
        auto &root = *static_cast<Stream *>(opaque);
        auto &s = root.replacement ? *root.replacement : root;
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) return s.length >= 0 ? s.length : AVERROR(ENOSYS);
        if (whence == SEEK_CUR) {
            if (pos > 0 && s.offset > INT64_MAX - pos) return AVERROR(EINVAL);
            pos += s.offset;
        } else if (whence != SEEK_SET) return AVERROR(ENOSYS);
        if (pos < 0 || (s.length >= 0 && pos >= s.length)) return AVERROR(EINVAL);
        if (pos == s.offset) return pos;
        try {
            auto candidate = std::make_unique<Stream>(root.cancel);
            candidate->known_length = s.length;
            candidate->open(root.url, pos);
            uint8_t first;
            if (read(candidate.get(), &first, 1) != 1) return AVERROR(EIO);
            // Put validation byte back; commit only after actual matching 206.
            memmove(candidate->bytes.data() + 1, candidate->bytes.data(), candidate->used);
            candidate->bytes[0] = first;
            ++candidate->used;
            --candidate->offset;
            root.replacement = std::move(candidate);
            return pos;
        } catch (...) { return AVERROR(EIO); }
    }
    static int read(void *opaque, uint8_t *out, int capacity) {
        auto &root = *static_cast<Stream *>(opaque);
        auto &s = root.replacement ? *root.replacement : root;
        while (!s.cancel) {
            if (s.used) {
                int n = static_cast<int>(std::min(s.used, static_cast<size_t>(capacity)));
                if (n > INT64_MAX - s.offset) return s.transport_failure();
                memcpy(out, s.bytes.data(), n);
                s.used -= n;
                memmove(s.bytes.data(), s.bytes.data() + n, s.used);
                s.offset += n;
                return n;
            }
            if (s.done) return s.failed ? AVERROR(EIO) : AVERROR_EOF;
            if (s.paused) {
                s.paused = false;
                CURLcode result = curl_easy_pause(s.easy, CURLPAUSE_CONT);
                if (result != CURLE_OK) {
                    s.curl_result = static_cast<int>(result);
                    return s.transport_failure();
                }
            }
            int running = 0;
            if (!s.check_multi(curl_multi_perform(s.multi, &running), AudioMultiOp::perform))
                return AVERROR(EIO);
            int pending = 0;
            while (CURLMsg *msg = curl_multi_info_read(s.multi, &pending)) {
                if (msg->msg == CURLMSG_DONE) {
                    long status = 0;
                    curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
                    s.done = true;
                    s.failed = msg->data.result != CURLE_OK || !s.valid_extent() ||
                        (s.ranged ? status != 206 || s.received != s.length - s.range_start : status != 200) ||
                        (s.saw_length && s.received != s.content_length);
                    if (!s.ranged && !s.failed) {
                        curl_off_t length = -1;
                        if (curl_easy_getinfo(s.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK)
                            s.length = length;
                    }
                    if (s.failed) {
                        s.curl_result = static_cast<int>(msg->data.result);
                        s.http_status = status >= 100 && status <= 599 ? static_cast<int>(status) : 0;
                        s.error = status == 401 || status == 403 ? AudioError::auth : AudioError::transport;
                        s.message = s.error == AudioError::auth ? "HTTP authorization failed" :
                            msg->data.result == CURLE_PARTIAL_FILE ? "HTTP transfer incomplete" :
                            msg->data.result == CURLE_SSL_CACERT_BADFILE ? "HTTPS CA bundle unavailable or invalid" :
                            "HTTP transfer failed";
                    }
                }
            }
            if (!s.used && !s.done &&
                !s.check_multi(curl_multi_poll(s.multi, nullptr, 0, 100, nullptr), AudioMultiOp::poll))
                return AVERROR(EIO);
        }
        return AVERROR_EXIT;
    }
};
struct Decode {
    AVFormatContext *format = nullptr;
    AVIOContext *io = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    SwrContext *swr = nullptr;
    AVChannelLayout layout{};
    int rate = 0, sample_format = -1;
    void swap(Decode &other) {
        using std::swap;
        swap(format, other.format); swap(io, other.io); swap(codec, other.codec);
        swap(frame, other.frame); swap(packet, other.packet); swap(swr, other.swr);
        swap(layout, other.layout); swap(rate, other.rate); swap(sample_format, other.sample_format);
    }
    ~Decode() {
        av_channel_layout_uninit(&layout);
        swr_free(&swr);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }
};
int refuse_open(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **) {
    return AVERROR(EACCES);
}
} // namespace

// Decoder and transport belong to one bounded PCM producer. Native output belongs
// only to the session worker; no output handle or padded block crosses this pipe.
struct PcmProducer {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::condition_variable wake;
    std::array<float, 48000 * 2> ring{};
    size_t head = 0, used = 0;
    bool ready = false, done = false;
    AudioStatus result;
    std::string url;
    double target;
    double duration = 0; // Published with ready; immutable afterwards.
    int64_t known_length;
    pthread_t thread{};
    bool joinable = false;
    explicit PcmProducer(std::string address, double seconds = -1, int64_t length = -1)
        : url(std::move(address)), target(seconds), known_length(length) {}
    ~PcmProducer() { stop(); }
    void stop() {
        cancel = true;
        wake.notify_all();
        if (joinable) { pthread_join(thread, nullptr); joinable = false; }
    }
    bool start() {
        joinable = evo_thread_create(&thread, [](void *p) -> void * {
            static_cast<PcmProducer *>(p)->run(); return nullptr;
        }, this) == 0;
        if (!joinable) {
            std::lock_guard<std::mutex> lock(mutex);
            ready = done = true;
            result = {AudioState::error, 0, "Cannot start audio worker", AudioError::output};
        }
        return joinable;
    }
    void push(const float *p, int frames) {
        while (frames) {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return cancel || used < ring.size() / 2; });
            require(!cancel, "Cancelled");
            size_t tail = (head + used) % (ring.size() / 2);
            size_t n = std::min({size_t(frames), ring.size() / 2 - used, ring.size() / 2 - tail});
            memcpy(ring.data() + tail * 2, p, n * 2 * sizeof(float));
            used += n; p += n * 2; frames -= n;
            lock.unlock(); wake.notify_all();
        }
    }
    void run() {
        // Heap allocation keeps transport's 256 KiB buffer off native worker stack.
        std::unique_ptr<Stream> stream;
        AudioError code = AudioError::transport;
        AudioStatus outcome;
        try {
            stream = std::make_unique<Stream>(cancel);
            stream->known_length = known_length;
            stream->open(url, target >= 0 ? 0 : -1);
            code = AudioError::demux;
            Decode d;
            require(d.frame && d.packet, "Audio allocation failed");
            auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
            require(buffer != nullptr, "Audio allocation failed");
            d.io = avio_alloc_context(buffer, 32768, 0, stream.get(), Stream::read, nullptr, Stream::seek);
            if (!d.io) av_free(buffer);
            require(d.io != nullptr, "Audio allocation failed");
            d.io->seekable = AVIO_SEEKABLE_NORMAL;
            d.format = avformat_alloc_context();
            require(d.format != nullptr, "Audio allocation failed");
            d.format->pb = d.io;
            d.format->flags |= AVFMT_FLAG_CUSTOM_IO;
            d.format->io_open = refuse_open;
            d.format->probesize = 256 * 1024;
            d.format->max_analyze_duration = 3 * AV_TIME_BASE;
            AVDictionary *options = nullptr;
            av_dict_set(&options, "format_whitelist", "mp3,flac,aac,ogg,mov", 0);
            av_dict_set(&options, "protocol_whitelist", "", 0);
            int rc = avformat_open_input(&d.format, nullptr, nullptr, &options);
            av_dict_free(&options);
            require(rc >= 0 && avformat_find_stream_info(d.format, nullptr) >= 0, "Cannot read audio stream");
            int index = av_find_best_stream(d.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            require(index >= 0, "No audio stream");
            code = AudioError::decode;
            const auto *track = d.format->streams[index];
            const auto *par = track->codecpar;
            require(par->codec_id == AV_CODEC_ID_MP3 || par->codec_id == AV_CODEC_ID_FLAC ||
                    par->codec_id == AV_CODEC_ID_AAC || par->codec_id == AV_CODEC_ID_OPUS, "Unsupported audio codec");
            const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
            require(decoder != nullptr, "Audio decoder unavailable");
            d.codec = avcodec_alloc_context3(decoder);
            require(d.codec && avcodec_parameters_to_context(d.codec, par) >= 0 &&
                    avcodec_open2(d.codec, decoder, nullptr) >= 0, "Cannot open audio decoder");
            double decoded_duration = track->duration != AV_NOPTS_VALUE ? track->duration * av_q2d(track->time_base) :
                d.format->duration / static_cast<double>(AV_TIME_BASE);
            if (!std::isfinite(decoded_duration) || decoded_duration <= 0) decoded_duration = 0;
            if (target >= 0) {
                require(decoded_duration > 0 && target < decoded_duration && target * 48000.0 < static_cast<double>(INT64_MAX),
                        "Seek outside audio duration");
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                known_length = stream->active().length;
                duration = decoded_duration;
                ready = true;
            }
            wake.notify_all();
            // FFmpeg applies codec skip/discard metadata. Do not trim it twice.
            // ponytail: seek decodes from origin; indexed seek needs proven preroll.
            int64_t discard = target >= 0 ? static_cast<int64_t>(std::llround(target * 48000.0)) : 0;
            constexpr int capacity = 400000;
            std::vector<float> pcm(capacity * 2);
            auto convert = [&](bool flush) {
                require(swr_get_out_samples(d.swr, flush ? 0 : d.frame->nb_samples) <= capacity,
                        "Audio resampler capacity exceeded");
                uint8_t *out[] = {reinterpret_cast<uint8_t *>(pcm.data())};
                int n = swr_convert(d.swr, out, capacity,
                    flush ? nullptr : const_cast<const uint8_t **>(d.frame->extended_data), flush ? 0 : d.frame->nb_samples);
                require(n >= 0, "Audio resampling failed");
                int skip = static_cast<int>(std::min<int64_t>(discard, n));
                discard -= skip;
                push(pcm.data() + skip * 2, n - skip);
                return n;
            };
            auto receive = [&] {
                int result;
                while ((result = avcodec_receive_frame(d.codec, d.frame)) >= 0) {
                    require(!cancel, "Cancelled");
                    require(d.frame->sample_rate >= 8000 && d.frame->sample_rate <= 384000 &&
                            d.frame->ch_layout.nb_channels > 0 && d.frame->ch_layout.nb_channels <= 8 &&
                            d.frame->nb_samples >= 0 && d.frame->nb_samples <= 65536, "Invalid audio frame");
                    if (!d.swr) {
                        d.rate = d.frame->sample_rate; d.sample_format = d.frame->format;
                        require(av_channel_layout_copy(&d.layout, &d.frame->ch_layout) >= 0, "Audio allocation failed");
                        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                        require(swr_alloc_set_opts2(&d.swr, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                            &d.frame->ch_layout, static_cast<AVSampleFormat>(d.frame->format), d.frame->sample_rate, 0, nullptr) >= 0 &&
                            swr_init(d.swr) >= 0, "Cannot initialize audio resampler");
                    }
                    require(d.frame->sample_rate == d.rate && d.frame->format == d.sample_format &&
                            av_channel_layout_compare(&d.frame->ch_layout, &d.layout) == 0, "Audio format changed");
                    convert(false); av_frame_unref(d.frame);
                }
                require(result == AVERROR(EAGAIN) || result == AVERROR_EOF, "Audio decode failed");
            };
            while (!cancel) {
                code = AudioError::demux;
                rc = av_read_frame(d.format, d.packet);
                if (rc < 0) break;
                code = AudioError::decode;
                if (d.packet->stream_index == index) {
                    require(avcodec_send_packet(d.codec, d.packet) >= 0, "Audio decode failed"); receive();
                }
                av_packet_unref(d.packet);
            }
            require(!cancel, "Cancelled");
            require(rc == AVERROR_EOF, "Audio stream interrupted");
            std::array<uint8_t, 32768> tail;
            int result;
            do { result = Stream::read(&stream->active(), tail.data(), tail.size()); } while (result > 0);
            require(result == AVERROR_EOF && !stream->active().failed, "Audio stream interrupted");
            code = AudioError::decode;
            require(avcodec_send_packet(d.codec, nullptr) >= 0, "Audio decode failed"); receive();
            if (d.swr) while (convert(true) > 0) {}
        } catch (const Failure &e) {
            outcome.error = stream && stream->active().failed ? stream->active().message : e.message;
            outcome.error_code = stream && stream->active().failed ? stream->active().error : code;
        } catch (...) { outcome.error = "Audio worker failed"; outcome.error_code = code; }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stream) {
                auto &s = stream->active();
                known_length = s.length;
                if (s.failed) {
                    outcome.error = s.message; outcome.error_code = s.error;
                    outcome.curl_result = s.curl_result; outcome.http_status = s.http_status;
                    outcome.multi_result = s.multi_result; outcome.multi_op = s.multi_op;
                }
            }
            result = std::move(outcome);
            ready = done = true;
        }
        wake.notify_all();
    }
};

struct AudioPlayer::Shared {
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> cancel{false};
    bool active = false, paused = false, output_ready = false, seek_pending = false;
    double seek_target = -1;
    AudioStatus status;
    std::string url, next_url;
    bool next_known = true;
    uint64_t next_revision = 0;
    NativeSink native;
    AudioSink *sink;
    explicit Shared(AudioSink *s) : sink(s ? s : &native) {}
    void run() {
        bool opened = false;
        uint64_t failure_occurrence = 0;
        std::unique_ptr<PcmProducer> current, next;
        uint64_t loaded_revision = 0;
        auto make = [](std::string url, double target = -1, int64_t length = -1) {
            auto p = std::make_unique<PcmProducer>(std::move(url), target, length);
            p->start(); return p;
        };
        auto wait_ready = [&](PcmProducer &p) {
            for (;;) {
                require(!cancel, "Cancelled");
                std::unique_lock<std::mutex> lock(p.mutex);
                if (p.ready) return p.result.error.empty();
                p.wake.wait_for(lock, std::chrono::milliseconds(10));
            }
        };
        try {
            current = make(url);
            // Submit the initial successor before waiting for current-track probing.
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!next_url.empty()) next = make(next_url);
            }
            if (!wait_ready(*current)) { std::lock_guard<std::mutex> lock(current->mutex); status_error(current->result); throw Failure{"Decoder failed"}; }
            {
                std::unique_lock<std::mutex> lock(mutex);
                while (!cancel && paused) wake.wait_for(lock, std::chrono::milliseconds(100));
            }
            require(!cancel, "Cancelled");
            opened = sink->open(); require(opened, "Cannot open audio output");
            {
                std::lock_guard<std::mutex> lock(mutex);
                { std::lock_guard<std::mutex> producer_lock(current->mutex); status.duration = current->duration; }
                output_ready = true; status.state = paused ? AudioState::paused : AudioState::playing;
            }
            std::array<float, grain * 2> block{};
            int filled = 0, block_current = 0;
            uint64_t occurrence = 0, block_track = 0;
            // Only the last occurrence in a block determines public position.
            // Keep its attribution until submission, even after decoder promotion.
            auto submit_block = [&] {
                // Mixed-block failure belongs to the last PCM occurrence attempted.
                failure_occurrence = block_track;
                require(sink->write(block.data()), "Audio output failed");
                std::lock_guard<std::mutex> lock(mutex);
                { std::lock_guard<std::mutex> producer_lock(current->mutex); status.duration = current->duration; }
                if (status.track != block_track) { status.track = block_track; status.position = 0; status.seek_failed = false; }
                status.position += block_current / 48000.0;
                filled = block_current = 0;
            };
            for (;;) {
                double target = -1;
                std::string successor;
                uint64_t revision;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    while (!cancel && !seek_pending && paused && loaded_revision == next_revision)
                        wake.wait_for(lock, std::chrono::milliseconds(100));
                    require(!cancel, "Cancelled");
                    revision = next_revision;
                    if (seek_pending) { target = seek_target; seek_pending = false; }
                    if ((!next || loaded_revision != revision) && !next_url.empty()) successor = next_url;
                }
                if (loaded_revision != revision) {
                    next.reset(); // Cancel/join outside admission lock; current PCM untouched.
                    loaded_revision = revision;
                }
                if (target >= 0) {
                    failure_occurrence = occurrence;
                    // Cancel and join speculative decode before allocating replacement.
                    next.reset();
                    int64_t length;
                    { std::lock_guard<std::mutex> lock(current->mutex); length = current->known_length; }
                    auto candidate = make(current->url, target, length);
                    bool success = wait_ready(*candidate);
                    if (success) {
                        current = std::move(candidate); // Cancels/joins old producer.
                        filled = block_current = 0;
                        bool closed = sink->close(); opened = false;
                        require(closed, "Cannot close audio output");
                        opened = sink->open(); require(opened, "Cannot open audio output");
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        status.seeking = false; status.seek_failed = !success;
                        status.buffering = false;
                        if (success) { status.position = target; status.duration = current->duration; }
                    }
                    // Rebuild next from URL after either outcome; no pre-seek PCM survives.
                    continue;
                }
                if (!successor.empty()) next = make(std::move(successor));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (paused || loaded_revision != next_revision) continue;
                }
                int count = 0;
                bool done;
                AudioStatus result;
                {
                    std::unique_lock<std::mutex> lock(current->mutex);
                    size_t n = std::min({size_t(grain - filled), current->used, current->ring.size() / 2 - current->head});
                    count = static_cast<int>(n);
                    if (n) {
                        memcpy(block.data() + filled * 2, current->ring.data() + current->head * 2, n * 2 * sizeof(float));
                        current->head = (current->head + n) % (current->ring.size() / 2); current->used -= n;
                    }
                    done = current->done && current->used == 0;
                    if (done) result = current->result;
                }
                current->wake.notify_all();
                if (count) {
                    if (block_track != occurrence) { block_track = occurrence; block_current = 0; }
                    filled += count; block_current += count;
                    std::lock_guard<std::mutex> lock(mutex);
                    status.buffering = false;
                }
                if (filled == grain) submit_block();
                if (done) {
                    if (!result.error.empty()) {
                        // Error terminates this session. Preserve already-consumed PCM;
                        // padding belongs only to this terminal block, never A/B handoff.
                        if (filled) {
                            std::fill(block.begin() + filled * 2, block.end(), 0.f);
                            submit_block();
                        }
                        { std::lock_guard<std::mutex> lock(mutex); status.error_track = occurrence; }
                        status_error(result); throw Failure{"Decoder failed"};
                    }
                    bool known, terminal;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        known = next_known; terminal = known && next_url.empty();
                    }
                    if (known && next) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (seek_pending || loaded_revision != next_revision) continue;
                            // Serialize promotion against seek admission. Until B is
                            // submitted, seek refuses this ambiguous boundary window.
                            next_url.clear(); next_known = false; status.next_track = occurrence + 1;
                        }
                        current = std::move(next); ++occurrence;
                        continue;
                    }
                    if (terminal) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (seek_pending || loaded_revision != next_revision) continue;
                            output_ready = false;
                        }
                        if (filled) {
                            std::fill(block.begin() + filled * 2, block.end(), 0.f);
                            submit_block();
                        }
                        break;
                    }
                }
                if (!count) {
                    std::unique_lock<std::mutex> lock(mutex);
                    status.buffering = true;
                    wake.wait_for(lock, std::chrono::milliseconds(5));
                }
            }
        } catch (const Failure &e) {
            std::lock_guard<std::mutex> lock(mutex);
            if (status.error.empty() && !cancel) {
                status.error = e.message; status.error_code = AudioError::output; status.error_track = failure_occurrence;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            status.error = "Audio worker failed"; status.error_code = AudioError::output; status.error_track = failure_occurrence;
        }
        // No producer can outlive the session or retain credentials after stop.
        next.reset(); current.reset();
        auto close_error = [&] {
            { std::lock_guard<std::mutex> lock(mutex); status.error_track = failure_occurrence; }
            status_error({AudioState::error, 0, "Cannot close audio output", AudioError::output});
        };
        try {
            if (opened && !sink->close()) close_error();
        } catch (...) { close_error(); }
        {
            std::lock_guard<std::mutex> lock(mutex);
            status.state = cancel ? AudioState::stopped : status.error.empty() ? AudioState::ended : AudioState::error;
            if (cancel) status = {};
            status.seeking = status.buffering = false;
            seek_pending = output_ready = active = false;
        }
        wake.notify_all();
    }
    void status_error(const AudioStatus &error) {
        std::lock_guard<std::mutex> lock(mutex);
        status.error = error.error; status.error_code = error.error_code;
        status.curl_result = error.curl_result; status.http_status = error.http_status;
        status.multi_result = error.multi_result; status.multi_op = error.multi_op;
        // Preserve occurrence and position; errors never fabricate a PCM transition.
    }
};
AudioPlayer::AudioPlayer(AudioSink *sink) : shared_(std::make_shared<Shared>(sink)) {}
AudioPlayer::~AudioPlayer() { stop(); }
bool AudioPlayer::start(std::string url, std::string next_url) {
    stop();
    auto s = shared_;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->cancel = false; s->paused = false; s->output_ready = false; s->seek_pending = false;
        s->active = true; s->next_known = true; s->next_revision = 0;
        s->status = {AudioState::opening, 0, {}};
        s->url = std::move(url); s->next_url = std::move(next_url);
    }
    if (evo_thread_create(&worker_, [](void *p) -> void * {
            static_cast<Shared *>(p)->run(); return nullptr;
        }, s.get()) == 0) { joinable_ = true; return true; }
    std::lock_guard<std::mutex> lock(s->mutex);
    s->active = false;
    s->status = {AudioState::error, 0, "Cannot start audio worker", AudioError::output};
    return false;
}
bool AudioPlayer::queue_next(std::string url, uint64_t track) {
    auto s = shared_;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->active || s->cancel || s->status.next_track != track || s->next_known) return false;
    s->next_url = std::move(url); s->next_known = true; s->wake.notify_all(); return true;
}
bool AudioPlayer::replace_next(std::string url, uint64_t track) {
    auto s = shared_;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->active || s->cancel || !s->output_ready || s->status.seeking ||
        s->status.track != track || s->status.next_track != track) return false;
    s->next_url = std::move(url); s->next_known = true; ++s->next_revision;
    s->wake.notify_all(); return true;
}
void AudioPlayer::stop() {
    auto s = shared_;
    std::unique_lock<std::mutex> lock(s->mutex);
    s->cancel = true; s->wake.notify_all();
    s->wake.wait(lock, [&] { return !s->active; });
    s->status = {}; s->url.clear(); s->next_url.clear();
    lock.unlock();
    if (joinable_) { pthread_join(worker_, nullptr); joinable_ = false; }
}
void AudioPlayer::pause() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (!shared_->active) return;
    shared_->paused = true; shared_->status.state = AudioState::paused;
}
void AudioPlayer::resume() {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->paused = false;
    if (shared_->status.state == AudioState::paused)
        shared_->status.state = shared_->output_ready ? AudioState::playing : AudioState::opening;
    shared_->wake.notify_all();
}
bool AudioPlayer::seek(double seconds, uint64_t expected_track) {
    auto s = shared_;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!std::isfinite(seconds) || seconds < 0 || seconds > static_cast<double>(INT64_MAX / AV_TIME_BASE) ||
        !s->active || !s->output_ready || s->cancel || s->status.seeking ||
        (expected_track != UINT64_MAX && s->status.track != expected_track) ||
        s->status.track != s->status.next_track) return false;
    s->seek_target = seconds; s->seek_pending = true;
    s->status.seeking = true; s->status.seek_failed = false; s->wake.notify_all(); return true;
}
std::shared_ptr<std::atomic<bool>> AudioPlayer::cancel_flag() {
    return {shared_, &shared_->cancel};
}
AudioStatus AudioPlayer::status() const {
    std::lock_guard<std::mutex> lock(shared_->mutex); return shared_->status;
}
} // namespace app
