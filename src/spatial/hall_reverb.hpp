#pragma once
// The hall behind 8D Audio's Echo: a stereo reverb built the way studio halls are. The sound is cleaned of rumble
// and fizz, waits a moment (pre-delay), comes back first as a handful of early reflections from the walls, then is
// smeared by four allpass diffusers into eight feedback delay lines that mix through a Householder matrix. Each line
// loses its top faster than its body (air and walls), and two pairs drift slowly in length so the tail never rings.
#include <algorithm>
#include <cmath>
#include <vector>

namespace spatial {

class HallReverb {
public:
    void init(double fs) {
        fs_ = fs;
        const double k = fs / 48000.0;
        pre_.assign((size_t)(0.25 * fs) + 8, 0.0f);
        static const double DIFF[4] = {142, 107, 379, 277};
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4; ++i) diff_[c][i].init((int)std::lround(DIFF[i] * k * (c ? 1.07 : 1.0)), i < 2 ? 0.72f : 0.62f);
        for (int i = 0; i < N; ++i) line_[i].assign((size_t)(LINE_MS[i] * 1.6 * 0.001 * fs) + MOD_MAX + 4, 0.0f);
        hpA_ = (float)std::exp(-2.0 * PI * 160.0 / fs);
        lpA_ = (float)std::exp(-2.0 * PI * 9000.0 / fs);
        set(2.5, 0.7);
        clear();
    }

    void clear() {
        std::fill(pre_.begin(), pre_.end(), 0.0f);
        for (auto& c : diff_) for (auto& d : c) d.clear();
        for (auto& l : line_) std::fill(l.begin(), l.end(), 0.0f);
        std::fill(std::begin(damp_), std::end(damp_), 0.0f);
        hpL_ = hpR_ = lpL_ = lpR_ = inL_ = inR_ = 0.0f;
        prePos_ = 0;
        linePos_ = 0;
    }

    // seconds for the tail to fall 60 dB, and the room's size 0..1 (longer lines, later reflections).
    void set(double t60, double size) {
        if (std::fabs(t60 - t60_) < 1e-6 && std::fabs(size - size_) < 1e-6) return;
        t60_ = t60;
        size_ = std::clamp(size, 0.0, 1.0);
        const double scale = 1.0 + 0.6 * size_;
        for (int i = 0; i < N; ++i) {
            len_[i] = LINE_MS[i] * scale * 0.001 * fs_;
            const double sec = len_[i] / fs_;
            gain_[i] = (float)std::pow(10.0, -3.0 * sec / t60);
            // The top end dies in about half the time: a one-pole that takes that much more off each pass.
            const double hiGain = std::pow(10.0, -3.0 * sec / (0.45 * t60));
            const double ratio = std::clamp(hiGain / gain_[i], 0.05, 1.0);
            dampA_[i] = (float)std::clamp(1.0 - ratio, 0.0, 0.92);
        }
        preDelay_ = (int)((0.012 + 0.018 * size_) * fs_);
        for (int t = 0; t < TAPS; ++t) erAt_[t] = (int)(ER_MS[t] * (0.7 + 0.6 * size_) * 0.001 * fs_);
    }

    // Adds the hall of (l, r) to out, at level wet.
    void process(const float* l, const float* r, float* outL, float* outR, int n, float wet) {
        const int preSize = (int)pre_.size();
        for (int i = 0; i < n; ++i) {
            // Clean input: no rumble into the tail, and no fizz.
            hpL_ = hpA_ * (hpL_ + l[i] - inL_); inL_ = l[i];
            hpR_ = hpA_ * (hpR_ + r[i] - inR_); inR_ = r[i];
            lpL_ += (1.0f - lpA_) * (hpL_ - lpL_);
            lpR_ += (1.0f - lpA_) * (hpR_ - lpR_);
            const float mid = 0.5f * (lpL_ + lpR_), side = 0.5f * (lpL_ - lpR_);
            pre_[prePos_] = mid;
            // Early reflections: walls left and right, quieter the later they come.
            float erL = side * 0.3f, erR = -side * 0.3f;
            for (int t = 0; t < TAPS; ++t) {
                const float v = pre_[(prePos_ - erAt_[t] + preSize) % preSize] * ER_GAIN[t];
                if (t & 1) erR += v; else erL += v;
            }
            const float late = pre_[(prePos_ - preDelay_ + preSize) % preSize];
            prePos_ = (prePos_ + 1) % preSize;
            float dl = late + side, dr = late - side;
            for (int k = 0; k < 4; ++k) { dl = diff_[0][k].run(dl); dr = diff_[1][k].run(dr); }

            // Eight lines, read a little early or late as they drift, damped, mixed and fed back.
            float o[N];
            for (int k = 0; k < N; ++k) {
                double d = len_[k];
                if (k < 4) d += MOD_DEPTH * std::sin(modPhase_[k]);
                o[k] = read(k, d);
                damp_[k] += (1.0f - dampA_[k]) * (o[k] - damp_[k]);
                o[k] = damp_[k] * gain_[k];
            }
            float sum = 0.0f;
            for (float v : o) sum += v;
            const float h = sum * (2.0f / N);
            for (int k = 0; k < N; ++k) {
                const float feed = o[k] - h + ((k & 1) ? dr : dl) * 0.6f;
                line_[k][linePos_ % line_[k].size()] = feed;
            }
            linePos_++;
            for (int k = 0; k < 4; ++k) {
                modPhase_[k] += MOD_HZ[k] * 2.0 * PI / fs_;
                if (modPhase_[k] > 2.0 * PI) modPhase_[k] -= 2.0 * PI;
            }
            const float tailL = o[0] - o[2] + o[4] - o[6] + 0.5f * (o[1] - o[5]);
            const float tailR = o[1] - o[3] + o[5] - o[7] + 0.5f * (o[2] - o[6]);
            outL[i] += wet * (0.7f * erL + 1.0f * tailL);
            outR[i] += wet * (0.7f * erR + 1.0f * tailR);
        }
    }

private:
    static constexpr double PI = 3.14159265358979323846;
    static constexpr int N = 8, TAPS = 8, MOD_MAX = 24;
    static constexpr double MOD_DEPTH = 9.0;
    static constexpr double LINE_MS[N] = {29.7, 37.1, 41.1, 43.7, 47.9, 53.3, 59.9, 67.1};
    static constexpr double MOD_HZ[4] = {0.31, 0.47, 0.59, 0.83};
    static constexpr double ER_MS[TAPS] = {7.1, 11.3, 16.9, 21.7, 29.3, 36.1, 44.9, 57.3};
    static constexpr float ER_GAIN[TAPS] = {0.62f, 0.55f, 0.47f, 0.41f, 0.33f, 0.27f, 0.21f, 0.15f};

    struct Allpass {
        std::vector<float> buf;
        int pos = 0;
        float g = 0.7f;
        void init(int len, float gain) { buf.assign((size_t)std::max(1, len), 0.0f); g = gain; pos = 0; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
        float run(float x) {
            const float z = buf[pos];
            const float w = x + g * z;
            buf[pos] = w;
            pos = (pos + 1) % (int)buf.size();
            return z - g * w;
        }
    };

    float read(int k, double d) const {
        const std::vector<float>& b = line_[k];
        const size_t size = b.size();
        const double at = (double)linePos_ - d;
        const double fl = std::floor(at);
        const float frac = (float)(at - fl);
        const long long i0 = (long long)fl;
        const float a = b[(size_t)(((i0 % (long long)size) + size) % size)];
        const float c = b[(size_t)((((i0 + 1) % (long long)size) + size) % size)];
        return a + (c - a) * frac;
    }

    double fs_ = 48000.0, t60_ = -1.0, size_ = -1.0;
    std::vector<float> pre_;
    int prePos_ = 0, preDelay_ = 0;
    int erAt_[TAPS] = {};
    Allpass diff_[2][4];
    std::vector<float> line_[N];
    unsigned long long linePos_ = 0;
    double len_[N] = {};
    float gain_[N] = {}, dampA_[N] = {}, damp_[N] = {};
    double modPhase_[4] = {0.0, 1.3, 2.6, 3.9};
    float hpA_ = 0.0f, lpA_ = 0.0f, hpL_ = 0.0f, hpR_ = 0.0f, lpL_ = 0.0f, lpR_ = 0.0f, inL_ = 0.0f, inR_ = 0.0f;
};

}  // namespace spatial
