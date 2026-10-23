#pragma once
// 8D audio, the way the "8D" songs and browser add-ons do it: the sound becomes one source that circles your head,
// one turn every 8 seconds. As it comes round to a side, that ear hears it louder, a moment sooner and brighter, and
// the far ear a little later and duller; behind you it loses some of its top. Bass and Clarity shape it first. The
// echo is not made here: it is the Environment's own reverb, fed by Pixl's send like every other room.
#include "../equalizer/filter_design.hpp"
#include "../voice/voice_color.hpp"
#include <algorithm>
#include <cmath>

namespace spatial {

class EightD {
public:
    static constexpr double TURN_SEC = 8.0;    // one full circle
    static constexpr double ITD_SEC = 0.00028; // how much later the far ear hears it, at the side
    static constexpr float GAIN = 1.25f;       // equal-power pan, lifted so the centre stays near the original level
    static constexpr double PAN = 0.85;        // how far to the side: the far ear keeps about -12 dB, never silence
    static constexpr float SIDE_KEEP = 0.25f;  // a little of a stereo song's own width stays
    static constexpr int BLOCK = 32;           // the position moves every 0.7 ms, far finer than anyone hears

    void init(double fs) {
        fs_ = fs;
        step_ = 2.0 * eqb::PI_ / (TURN_SEC * fs);
        rearA_ = (float)std::exp(-2.0 * eqb::PI_ * 5000.0 / fs);
        tone(bass_, clarity_, true);
        reset();
    }

    void reset() {
        std::fill(buf_, buf_ + SIZE, 0.0f);
        w_ = 0;
        left_ = 0;
        angle_ = 0.0;
        rear_ = shadeL_ = shadeR_ = 0.0f;
        for (eqb::Biquad* q : {&lo_[0], &lo_[1], &hi_[0], &hi_[1]}) q->reset();
    }

    // Bass and Clarity in percent, 0..200; 100 is the sound as designed.
    void tone(int bass, int clarity, bool force = false) {
        if (!force && bass == bass_ && clarity == clarity_) return;
        bass_ = bass;
        clarity_ = clarity;
        const double bassDb = -6.0 + 8.0 * bass / 100.0, clearDb = -5.0 + 6.0 * clarity / 100.0;
        for (int k = 0; k < 2; ++k) {
            eqb::lowShelf(lo_[k], fs_, 110.0, bassDb);
            voice::highShelf(hi_[k], fs_, 3200.0, clearDb);
        }
        // Boosts take back part of what they add, so the loudest notes keep their headroom.
        trim_ = (float)std::pow(10.0, -(std::max(0.0, bassDb) * 0.2 + std::max(0.0, clearDb) * 0.15) / 20.0);
    }

    // In place on a stereo pair (a mono voice comes in as the same sound on both).
    void process(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {
            if (left_ == 0) place();
            --left_;
            float m = hi_[0].run(lo_[0].run(0.5f * (l[i] + r[i]))) * trim_;
            const float side = hi_[1].run(lo_[1].run(0.5f * (l[i] - r[i]) * SIDE_KEEP)) * trim_;
            rear_ += (1.0f - rearA_) * (m - rear_);
            m += back_ * (rear_ - m);
            buf_[w_] = m;
            const float xl = tap(delayL_), xr = tap(delayR_);
            w_ = (w_ + 1) & MASK;
            shadeL_ += (1.0f - shadeAL_) * (xl - shadeL_);
            shadeR_ += (1.0f - shadeAR_) * (xr - shadeR_);
            l[i] = gainL_ * shadeL_ + side;
            r[i] = gainR_ * shadeR_ - side;
        }
    }

private:
    static constexpr int SIZE = 64, MASK = SIZE - 1;

    double fs_ = 48000.0, step_ = 0.0, angle_ = 0.0;
    int bass_ = 100, clarity_ = 100;
    eqb::Biquad lo_[2], hi_[2];  // [0] the source, [1] the width it keeps
    float trim_ = 1.0f;
    float buf_[SIZE] = {};
    int w_ = 0, left_ = 0;
    double delayL_ = 0.0, delayR_ = 0.0;
    float gainL_ = 0.0f, gainR_ = 0.0f, shadeAL_ = 0.0f, shadeAR_ = 0.0f, shadeL_ = 0.0f, shadeR_ = 0.0f;
    float rearA_ = 0.0f, rear_ = 0.0f, back_ = 0.0f;

    // Where the sound is now: 0 is in front of you, then it goes round to your right, behind, and your left.
    void place() {
        left_ = BLOCK;
        angle_ += step_ * BLOCK;
        if (angle_ >= 2.0 * eqb::PI_) angle_ -= 2.0 * eqb::PI_;
        const double s = std::sin(angle_), c = std::cos(angle_);
        gainL_ = GAIN * (float)std::sqrt(0.5 * (1.0 - PAN * s));
        gainR_ = GAIN * (float)std::sqrt(0.5 * (1.0 + PAN * s));
        const double late = ITD_SEC * fs_ * std::fabs(s);
        delayL_ = s > 0.0 ? late : 0.0;
        delayR_ = s < 0.0 ? late : 0.0;
        // The head shades the far ear: open when the sound faces it, down to about 2.6 kHz when it is right across.
        auto shade = [&](double away) {
            if (away <= 0.0) return 0.0f;
            const double hz = 16000.0 * std::pow(2600.0 / 16000.0, away);
            return (float)std::exp(-2.0 * eqb::PI_ * hz / fs_);
        };
        shadeAL_ = shade(s);
        shadeAR_ = shade(-s);
        back_ = (float)(0.45 * std::max(0.0, -c));
    }

    // The source d samples before the one just written, between samples by a straight line.
    float tap(double d) const {
        const double at = (double)w_ - d;
        const int i0 = (int)std::floor(at);
        const float frac = (float)(at - i0);
        return buf_[i0 & MASK] * (1.0f - frac) + buf_[(i0 + 1) & MASK] * frac;
    }
};

}  // namespace spatial
