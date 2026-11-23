#pragma once
#include <cmath>
#include <vector>
#include <cstdint>

namespace audio {
constexpr int kOutRate = 48000;
constexpr int kTaps = 8;

// Small windowed-sinc resampler for stereo float -> s16 at 48 kHz.
class Resampler {
public:
    void reset(int inRate) {
        m_inRate = inRate;
        m_step = (double)inRate / (double)kOutRate;
        m_pos = 0;
        m_padded = false;
        m_hist.clear();
    }

    void append(const float* stereo, int frames) {
        m_hist.insert(m_hist.end(), stereo, stereo + (size_t)frames * 2);
    }

    // Produces up to outCap frames. Returns frames written.
    int process(int16_t* out, int outCap, bool eof) {
        int produced = 0;
        size_t total = m_hist.size() / 2;
        while (produced < outCap) {
            int base = (int)floor(m_pos);
            if (base + kTaps >= (int)total) {
                if (!eof || m_padded) break;
                // pad with silence so the tail can be rendered
                m_hist.insert(m_hist.end(), (size_t)(kTaps + 2) * 2, 0.0f);
                m_padded = true;
                total = m_hist.size() / 2;
            }
            if (base - kTaps + 1 < 0) { m_pos += m_step; continue; }
            float left = 0, right = 0, wsum = 0;
            for (int k = -kTaps + 1; k <= kTaps; k++) {
                int idx = base + k;
                float dist = (float)(m_pos - idx);
                float w = weight(dist);
                left += m_hist[(size_t)idx * 2] * w;
                right += m_hist[(size_t)idx * 2 + 1] * w;
                wsum += w;
            }
            if (wsum > 0.0001f) { left /= wsum; right /= wsum; }
            out[produced * 2] = toS16(left);
            out[produced * 2 + 1] = toS16(right);
            produced++;
            m_pos += m_step;
        }
        // drop samples that will never be needed again
        int drop = (int)floor(m_pos) - kTaps;
        if (drop > 0 && drop * 2 <= (int)m_hist.size()) {
            m_hist.erase(m_hist.begin(), m_hist.begin() + (size_t)drop * 2);
            m_pos -= drop;
        }
        return produced;
    }

    bool drained() const {
        size_t total = m_hist.size() / 2;
        return m_padded && m_pos + kTaps >= (double)total;
    }

private:
    static int16_t toS16(float v) {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int i = (int)lrintf(v * 32767.0f);
        if (i > 32767) i = 32767;
        if (i < -32768) i = -32768;
        return (int16_t)i;
    }

    static float sinc(float x) {
        if (fabsf(x) < 1e-6f) return 1.0f;
        float px = 3.14159265f * x;
        return sinf(px) / px;
    }

    float weight(float dist) const {
        if (fabsf(dist) > kTaps) return 0.0f;
        float t = dist / kTaps;
        float window = 0.42f + 0.5f * cosf(3.14159265f * t) + 0.08f * cosf(6.2831853f * t);
        float cutoff = 1.0f;
        if (m_inRate > kOutRate) cutoff = (float)kOutRate / (float)m_inRate;
        return sinc(dist * cutoff) * cutoff * window;
    }

    int m_inRate = 44100;
    double m_step = 1.0;
    double m_pos = 0;
    bool m_padded = false;
    std::vector<float> m_hist;
};

} // namespace audio
