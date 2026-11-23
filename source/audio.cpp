#include "audio.h"
#include "i18n.h"
#include "core.h"
#include "resampler.h"

#include <switch.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <malloc.h>
#include <vector>

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"
}

#define DR_MP3_IMPLEMENTATION
#include "third_party/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "third_party/dr_flac.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
}

namespace {

constexpr int kOutChannels = 2;
constexpr int kInFrames = 1024;

// AAC/M4A decoder backed by the prebuilt (PIC) libavformat/libavcodec from
// devkitPro. HLS tracks are downloaded as concatenated fMP4 and decoded here.
struct FfmpegDecoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int stream = -1;
    int rate = 0;
    uint64_t frames = 0;
    bool eof = false;
    std::vector<float> queue;
    size_t queuePos = 0;

    ~FfmpegDecoder() { close(); }

    void close() {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) avformat_close_input(&fmt);
        pkt = nullptr;
        frame = nullptr;
        ctx = nullptr;
        fmt = nullptr;
        queue.clear();
        queuePos = 0;
        eof = false;
    }

    void appendFrame(AVFrame* f) {
        int n = f->nb_samples;
        int ch = f->ch_layout.nb_channels;
        if (n <= 0 || ch <= 0) return;
        queue.reserve(queue.size() + (size_t)n * 2);
        enum AVSampleFormat sf = (enum AVSampleFormat)f->format;
        auto sample = [&](int c, int i) -> float {
            if (c >= ch) c = ch - 1;
            switch (sf) {
                case AV_SAMPLE_FMT_FLTP: return ((const float*)f->extended_data[c])[i];
                case AV_SAMPLE_FMT_FLT:  return ((const float*)f->extended_data[0])[(size_t)i * ch + c];
                case AV_SAMPLE_FMT_S16P: return ((const int16_t*)f->extended_data[c])[i] / 32768.0f;
                case AV_SAMPLE_FMT_S16:  return ((const int16_t*)f->extended_data[0])[(size_t)i * ch + c] / 32768.0f;
                case AV_SAMPLE_FMT_S32P: return ((const int32_t*)f->extended_data[c])[i] / 2147483648.0f;
                case AV_SAMPLE_FMT_S32:  return ((const int32_t*)f->extended_data[0])[(size_t)i * ch + c] / 2147483648.0f;
                default: return 0.0f;
            }
        };
        for (int i = 0; i < n; i++) {
            queue.push_back(sample(0, i));
            queue.push_back(sample(1, i));
        }
    }

    void drain() {
        while (avcodec_receive_frame(ctx, frame) == 0) appendFrame(frame);
    }

    bool open(const std::string& path) {
        close();
        av_log_set_level(AV_LOG_ERROR);
        // FFmpeg parses "scheme:" as a URL protocol. Our paths look like
        // "sdmc:/...", so force the local file protocol explicitly.
        std::string openPath = path;
        if (openPath.rfind("file:", 0) != 0) openPath = "file:" + openPath;
        int ret = avformat_open_input(&fmt, openPath.c_str(), nullptr, nullptr);
        if (ret < 0) {
            char err[128] = {0};
            av_strerror(ret, err, sizeof(err));
            util::log("ffmpeg: open %s failed (%d: %s)", openPath.c_str(), ret, err);
            return false;
        }
        ret = avformat_find_stream_info(fmt, nullptr);
        if (ret < 0) {
            util::log("ffmpeg: stream info failed (%d)", ret);
            return false;
        }
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream < 0) return false;
        AVStream* st = fmt->streams[stream];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) { util::log("ffmpeg: no decoder for codec %d", (int)st->codecpar->codec_id); return false; }
        ctx = avcodec_alloc_context3(dec);
        if (!ctx) return false;
        if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) return false;
        ctx->pkt_timebase = st->time_base;
        ret = avcodec_open2(ctx, dec, nullptr);
        if (ret < 0) { util::log("ffmpeg: avcodec_open2 failed (%d)", ret); return false; }
        rate = ctx->sample_rate;
        if (rate <= 0) return false;
        if (st->duration > 0)
            frames = (uint64_t)av_rescale_q(st->duration, st->time_base, AVRational{1, rate});
        else if (fmt->duration > 0)
            frames = (uint64_t)av_rescale_q(fmt->duration, AV_TIME_BASE_Q, AVRational{1, rate});
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        if (!pkt || !frame) return false;
        util::log("audio: ffmpeg opened %s (%d Hz, %d ch, %.1fs)", path.c_str(), rate,
                  (int)ctx->ch_layout.nb_channels, (double)frames / rate);
        return true;
    }

    size_t read(float* out, size_t want) {
        size_t produced = 0;
        while (produced < want) {
            if (queuePos < queue.size()) {
                size_t avail = (queue.size() - queuePos) / 2;
                size_t take = std::min(avail, want - produced);
                memcpy(out + produced * 2, queue.data() + queuePos, take * 2 * sizeof(float));
                queuePos += take * 2;
                produced += take;
                continue;
            }
            queue.clear();
            queuePos = 0;
            if (eof) break;
            int ret = av_read_frame(fmt, pkt);
            if (ret < 0) {
                avcodec_send_packet(ctx, nullptr);
                drain();
                eof = true;
                continue;
            }
            if (pkt->stream_index == stream) {
                if (avcodec_send_packet(ctx, pkt) >= 0) drain();
            }
            av_packet_unref(pkt);
        }
        return produced;
    }

    void seekToFrame(uint64_t frameNo) {
        if (!fmt || stream < 0) return;
        AVStream* st = fmt->streams[stream];
        int64_t ts = av_rescale_q((int64_t)frameNo, AVRational{1, rate}, st->time_base);
        av_seek_frame(fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
        queue.clear();
        queuePos = 0;
        eof = false;
    }
};

struct Decoder {
    enum Kind { None, Mp3, Flac, Ogg, Ffmpeg } kind = None;
    drmp3 mp3;
    drflac* flac = nullptr;
    stb_vorbis* vorbis = nullptr;
    FfmpegDecoder* ff = nullptr;
    int channels = 0;
    int rate = 0;
    uint64_t frames = 0;

    ~Decoder() { close(); }

    void close() {
        if (kind == Mp3) drmp3_uninit(&mp3);
        if (kind == Flac && flac) drflac_close(flac);
        if (kind == Ogg && vorbis) stb_vorbis_close(vorbis);
        if (kind == Ffmpeg && ff) { delete ff; ff = nullptr; }
        flac = nullptr;
        vorbis = nullptr;
        kind = None;
    }

    bool open(const std::string& path) {
        close();
        std::string low = util::lower(path);
        if (util::endsWith(low, ".mp3")) {
            if (!drmp3_init_file(&mp3, path.c_str(), nullptr)) return false;
            kind = Mp3;
            channels = mp3.channels;
            rate = (int)mp3.sampleRate;
            frames = drmp3_get_pcm_frame_count(&mp3);
        } else if (util::endsWith(low, ".flac")) {
            flac = drflac_open_file(path.c_str(), nullptr);
            if (!flac) return false;
            kind = Flac;
            channels = flac->channels;
            rate = (int)flac->sampleRate;
            frames = flac->totalPCMFrameCount;
        } else if (util::endsWith(low, ".ogg") || util::endsWith(low, ".oga")) {
            int err = 0;
            vorbis = stb_vorbis_open_filename(path.c_str(), &err, nullptr);
            if (!vorbis) return false;
            kind = Ogg;
            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            channels = info.channels;
            rate = (int)info.sample_rate;
            frames = stb_vorbis_stream_length_in_samples(vorbis);
        } else if (util::endsWith(low, ".m4a") || util::endsWith(low, ".mp4") ||
                   util::endsWith(low, ".aac") || util::endsWith(low, ".ts")) {
            ff = new FfmpegDecoder();
            if (!ff->open(path)) { delete ff; ff = nullptr; return false; }
            kind = Ffmpeg;
            channels = 2;
            rate = ff->rate;
            frames = ff->frames;
        } else {
            return false;
        }
        if (channels <= 0 || channels > 2 || rate <= 0) {
            close();
            return false;
        }
        return true;
    }

    size_t read(float* out, size_t wantFrames) {
        if (kind == Mp3) return drmp3_read_pcm_frames_f32(&mp3, wantFrames, out);
        if (kind == Flac) return (size_t)drflac_read_pcm_frames_f32(flac, wantFrames, out);
        if (kind == Ogg) {
            int got = stb_vorbis_get_samples_float_interleaved(vorbis, channels, out,
                                                              (int)(wantFrames * channels));
            return (size_t)got;
        }
        if (kind == Ffmpeg && ff) return ff->read(out, wantFrames);
        return 0;
    }

    void seekToFrame(uint64_t frame) {
        if (kind == Mp3) drmp3_seek_to_pcm_frame(&mp3, frame);
        else if (kind == Flac) drflac_seek_to_pcm_frame(flac, frame);
        else if (kind == Ogg) stb_vorbis_seek_frame64(vorbis, frame);
        else if (kind == Ffmpeg && ff) ff->seekToFrame(frame);
    }
};

} // namespace

void Player::threadEntry(void* arg) {
    static_cast<Player*>(arg)->run();
}

bool Player::init() {
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        util::log("audio: audoutInitialize failed 0x%x", rc);
        return false;
    }
    // audoutInitialize() already opens the default output device. Calling
    // audoutOpenAudioOut() a second time returns 0xd401 (the device is already
    // open), so use the default device that the service guard created.
    u32 rate = audoutGetSampleRate();
    u32 channels = audoutGetChannelCount();
    PcmFormat fmt = audoutGetPcmFormat();
    AudioOutState state = audoutGetDeviceState();
    util::log("audio: default out %u Hz %u ch fmt=%d state=%d", rate, channels, (int)fmt, (int)state);
    if (R_FAILED(audoutStartAudioOut())) util::log("audio: start failed");

    m_running.store(true);
    rc = threadCreate(&m_thread, &Player::threadEntry, this, nullptr, 0x40000, 0x2C, 2);
    if (R_FAILED(rc)) {
        util::log("audio: threadCreate failed 0x%x", rc);
        m_running.store(false);
        audoutExit();
        return false;
    }
    threadStart(&m_thread);
    m_threadStarted = true;
    return true;
}

void Player::shutdown() {
    m_running.store(false);
    if (m_threadStarted) {
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_threadStarted = false;
    }
    audoutStopAudioOut();
    audoutExit();
}

void Player::playFile(const std::string& path) {
    {
        ScopedLock<SpinLock> lk(m_mutex);
        m_pendingPath = path;
        m_currentPath = path;
        m_error.clear();
    }
    m_ended.store(false);
    m_loaded.store(false);
    m_position.store(0);
    m_duration.store(0);
    m_playing.store(false);
    m_command.store(1);
}

void Player::stop() { m_command.store(2); }

void Player::togglePause() {
    if (!m_loaded.load()) return;
    // Debounce: a single A press must not toggle twice (which looked like
    // "pause + immediate resume").
    u64 now = armGetSystemTick();
    u64 last = m_lastToggleTick.load();
    if (last && now - last < 19200000ULL / 4) return;
    m_lastToggleTick.store(now);
    util::log("audio: toggle pause -> %s at %.1fs", m_playing.load() ? "pause" : "play", m_position.load());
    if (m_playing.load()) {
        m_playing.store(false);
    } else {
        m_ended.store(false);
        double dur = m_duration.load();
        if (dur > 0 && m_position.load() >= dur - 0.25) m_seekRequest.store(0.0);
        m_playing.store(true);
    }
}

void Player::setPaused(bool paused) {
    if (paused) m_playing.store(false);
    else if (m_loaded.load()) m_playing.store(true);
}

void Player::seek(double seconds) {
    if (!m_loaded.load()) return;
    double dur = m_duration.load();
    if (seconds < 0) seconds = 0;
    if (dur > 0 && seconds > dur) seconds = dur;
    m_seekRequest.store(seconds);
    m_ended.store(false);
    m_position.store(seconds);
}

void Player::setVolume(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    m_volumeTarget.store(v);
}

std::string Player::errorText() {
    ScopedLock<SpinLock> lk(m_mutex);
    return m_error;
}

bool Player::consumeEnded() { return m_ended.exchange(false); }

std::string Player::currentPath() {
    ScopedLock<SpinLock> lk(m_mutex);
    return m_currentPath;
}

void Player::requestNext() {
    if (m_nextCb) m_nextCb(m_nextUser);
}

void Player::run() {
    Decoder dec;
    audio::Resampler resampler;

    constexpr int kNumBufs = 4;
    constexpr size_t kBufferSize = 16384; // up to 4096 stereo s16 frames
    void* pcm[kNumBufs] = {};
    AudioOutBuffer aobs[kNumBufs] = {};
    int bufFrames[kNumBufs] = {};
    bool inFlight[kNumBufs] = {};

    for (int i = 0; i < kNumBufs; i++) {
        pcm[i] = memalign(0x1000, kBufferSize);
        if (pcm[i]) memset(pcm[i], 0, kBufferSize);
        aobs[i].buffer = pcm[i];
        aobs[i].buffer_size = kBufferSize;
        aobs[i].data_size = 0;
    }
    float* inBuf = (float*)memalign(0x40, kInFrames * 2 * sizeof(float));

    uint64_t playedFrames = 0;
    int flight = 0;
    bool eofSeen = false;
    float volume = 1.0f;

    auto teardownStream = [&] {
        dec.close();
        resampler.reset(44100);
        m_loaded.store(false);
    };

    auto updatePos = [&] {
        double pos = (double)playedFrames / audio::kOutRate;
        double dur = m_duration.load();
        if (dur > 0 && pos > dur) pos = dur;
        m_position.store(pos);
    };

    auto releaseOne = [&](bool block) -> bool {
        AudioOutBuffer* rel = nullptr;
        u32 count = 0;
        Result rc = block ? audoutWaitPlayFinish(&rel, &count, 20 * 1000000ULL)
                          : audoutGetReleasedAudioOutBuffer(&rel, &count);
        if (R_FAILED(rc) || count == 0) return false;
        for (int i = 0; i < kNumBufs; i++) {
            if (rel == &aobs[i] && inFlight[i]) {
                inFlight[i] = false;
                flight--;
                playedFrames += (uint64_t)bufFrames[i];
                updatePos();
                return true;
            }
        }
        return false;
    };

    auto flushAll = [&] {
        bool flushed = false;
        audoutFlushAudioOutBuffers(&flushed);
        for (int i = 0; i < kNumBufs; i++) {
            inFlight[i] = false;
            aobs[i].data_size = 0;
        }
        flight = 0;
    };

    // Never flush/stop the device on a track change: stop/start cycles degrade
    // after a few switches on this firmware. Instead stop submitting and let the
    // (at most four) in-flight buffers finish naturally, then reuse them.
    auto drainAll = [&] {
        m_playing.store(false);
        while (flight > 0 && m_running.load()) releaseOne(true);
    };

    while (m_running.load()) {
        int cmd = m_command.exchange(0);
        if (cmd == 1) {
            std::string path;
            {
                ScopedLock<SpinLock> lk(m_mutex);
                path = m_pendingPath;
            }
            drainAll();
            teardownStream();
            eofSeen = false;
            if (!dec.open(path)) {
                ScopedLock<SpinLock> lk(m_mutex);
                m_error = tr("не удалось открыть файл");
                util::log("audio: cannot open %s", path.c_str());
                continue;
            }
            resampler.reset(dec.rate);
            double dur = dec.rate > 0 ? (double)dec.frames / dec.rate : 0;
            m_duration.store(dur);
            m_position.store(0);
            m_ended.store(false);
            m_loaded.store(true);
            m_playing.store(true);
            playedFrames = 0;
            m_seekRequest.store(-1);
            util::log("audio: playing %s (%d Hz, %d ch, %.1fs)", path.c_str(), dec.rate, dec.channels, dur);
        } else if (cmd == 2) {
            drainAll();
            teardownStream();
            m_position.store(0);
            m_duration.store(0);
        }

        if (!m_loaded.load()) {
            svcSleepThread(10 * 1000000ULL);
            continue;
        }

        double seekTo = m_seekRequest.exchange(-1);
        if (seekTo >= 0) {
            bool wasPlaying = m_playing.load();
            drainAll();
            uint64_t target = (uint64_t)(seekTo * dec.rate);
            if (dec.frames > 0 && target >= dec.frames) target = dec.frames - 1;
            dec.seekToFrame(target);
            resampler.reset(dec.rate);
            playedFrames = (uint64_t)(seekTo * audio::kOutRate);
            m_position.store(seekTo);
            eofSeen = false;
            if (wasPlaying) m_playing.store(true);
        }

        // Reap whatever finished, even while paused, so position stays right.
        while (releaseOne(false)) {}

        if (!m_playing.load()) {
            if (flight > 0) {
                releaseOne(true);
            } else {
                svcSleepThread(5 * 1000000ULL);
            }
            volume = m_volumeTarget.load();
            continue;
        }

        int idx = -1;
        for (int i = 0; i < kNumBufs; i++) {
            if (!inFlight[i]) { idx = i; break; }
        }
        if (idx < 0) {
            releaseOne(true);
            continue;
        }

        size_t got = dec.read(inBuf, kInFrames);
        bool eof = got == 0;
        if (!eof) {
            if (dec.channels == 1) {
                for (size_t i = got; i-- > 0;) {
                    inBuf[i * 2] = inBuf[i];
                    inBuf[i * 2 + 1] = inBuf[i];
                }
            }
            resampler.append(inBuf, (int)got);
        }

        int outFrames = resampler.process((int16_t*)aobs[idx].buffer, 4096, eof);
        if (outFrames <= 0) {
            if (eof) {
                eofSeen = true;
                if (flight == 0) {
                    m_playing.store(false);
                    m_ended.store(true);
                    m_position.store(m_duration.load());
                    requestNext();
                } else {
                    releaseOne(true);
                }
            } else {
                svcSleepThread(2 * 1000000ULL);
            }
            continue;
        }

        float target = m_volumeTarget.load();
        int16_t* samples = (int16_t*)aobs[idx].buffer;
        int totalSamples = outFrames * kOutChannels;
        for (int i = 0; i < totalSamples; i += kOutChannels) {
            volume += (target - volume) * 0.01f;
            for (int c = 0; c < kOutChannels; c++) {
                // 0.85 headroom: sinc-resampling can overshoot 0 dBFS and hard
                // clipping is what makes loud SoundCloud masters sound harsh.
                int v = (int)(samples[i + c] * volume * 0.85f);
                samples[i + c] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
            }
        }
        m_volume.store(volume);

        aobs[idx].data_size = (u64)outFrames * kOutChannels * sizeof(int16_t);
        bufFrames[idx] = outFrames;
        inFlight[idx] = true;
        flight++;
        Result rc = audoutAppendAudioOutBuffer(&aobs[idx]);
        if (R_FAILED(rc)) {
            util::log("audio: append buffer failed 0x%x", rc);
            inFlight[idx] = false;
            flight--;
            svcSleepThread(5 * 1000000ULL);
            continue;
        }

        if (eof) eofSeen = true;
        if (eofSeen && flight == 0 && resampler.drained()) {
            m_playing.store(false);
            m_ended.store(true);
            m_position.store(m_duration.load());
            requestNext();
        }
    }

    flushAll();
    teardownStream();
    for (int i = 0; i < kNumBufs; i++) free(pcm[i]);
    free(inBuf);
}
