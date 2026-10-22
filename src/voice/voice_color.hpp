#pragma once
// Voice colour, after Voicemeeter's Intellipan colour panel: one dot on a square.
// Left/right tilts the voice from Lo (warmer) to Hi (brighter). Up adds brightness and,
// in the top half, a tiny room echo; down takes brightness away. The centre changes nothing.

#include "../equalizer/filter_design.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace voice {

inline void highShelf(eqb::Biquad& q, double fs, double f0, double dB) {
    if (dB == 0.0) { q.identity(); return; }
    const double A = std::pow(10.0, dB / 40.0);
    const double w0 = 2.0 * eqb::PI_ * f0 / fs;
    const double sn = std::sin(w0), cs = std::cos(w0);
    const double alpha = sn / 2.0 * std::sqrt(2.0);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1.0) - (A - 1.0) * cs + sq;
    q.b0 = (A * ((A + 1.0) + (A - 1.0) * cs + sq)) / a0;
    q.b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cs)) / a0;
    q.b2 = (A * ((A + 1.0) + (A - 1.0) * cs - sq)) / a0;
    q.a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cs)) / a0;
    q.a2 = ((A + 1.0) - (A - 1.0) * cs - sq) / a0;
}

struct VoiceColor {
    static constexpr double LO_HZ = 220.0, HI_HZ = 3200.0, AIR_HZ = 7500.0;
    static constexpr double TILT_DB = 6.0, AIR_DB = 6.0;
    static constexpr float ECHO_MOST = 0.30f;  // wet level at the very top

    void prepare(int rate) {
        fs = rate;
        // Two short damped combs and an allpass: a small room, not a long reverb.
        combA.assign((size_t)(rate * 0.037), 0.0f);
        combB.assign((size_t)(rate * 0.053), 0.0f);
        diff.assign((size_t)(rate * 0.0071), 0.0f);
        set(0, 0);
    }

    // x and y run from -100 to 100; 0, 0 is the centre.
    void set(int x, int y) {
        const double tx = x / 100.0, ty = y / 100.0;
        eqb::lowShelf(lo, fs, LO_HZ, -tx * TILT_DB);
        highShelf(hi, fs, HI_HZ, tx * TILT_DB);
        highShelf(air, fs, AIR_HZ, ty * AIR_DB);
        echoWant = ty > 0.0 ? (float)(ty * ECHO_MOST) : 0.0f;
        // Boosting brightness makes everything louder; take half of each boost back so the level holds.
        const double boostDb = std::max(0.0, tx) * TILT_DB + std::max(0.0, ty) * AIR_DB;  // Lo already cuts the highs it gives up
        trim = (float)std::pow(10.0, -0.5 * boostDb / 20.0);
        flat = x == 0 && y == 0;
    }

    bool idle() const { return flat && echoNow < 1e-4f; }

    void process(float* s, int n) {
        if (idle()) return;
        for (int i = 0; i < n; ++i) {
            float v = trim * air.run(hi.run(lo.run(s[i])));
            echoNow += 0.0015f * (echoWant - echoNow);
            if (echoNow > 1e-4f) {
                const float a = comb(combA, atA, dampA, v, 0.42f);
                const float b = comb(combB, atB, dampB, v, 0.38f);
                v += echoNow * allpass(0.5f * (a + b));
            }
            s[i] = v;
        }
    }

private:
    int fs = 48000;
    eqb::Biquad lo, hi, air;
    bool flat = true;
    float echoWant = 0.0f, echoNow = 0.0f, trim = 1.0f;
    std::vector<float> combA, combB, diff;
    size_t atA = 0, atB = 0, atD = 0;
    float dampA = 0.0f, dampB = 0.0f;

    static float comb(std::vector<float>& buf, size_t& at, float& damp, float in, float feedback) {
        if (buf.empty()) return 0.0f;
        const float out = buf[at];
        damp += 0.35f * (out - damp);  // a darker tail, like a real room
        buf[at] = in + damp * feedback;
        if (++at >= buf.size()) at = 0;
        return out;
    }

    float allpass(float in) {
        if (diff.empty()) return in;
        const float held = diff[atD];
        const float out = held - 0.5f * in;
        diff[atD] = in + 0.5f * held;
        if (++atD >= diff.size()) atD = 0;
        return out;
    }
};

}
