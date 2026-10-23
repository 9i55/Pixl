#pragma once
// 8D audio, the way the "8D" songs and browser add-ons do it: the sound becomes one source that circles your head.
// As it comes round to a side, that ear hears it louder, a moment sooner and brighter, and the far ear a little later
// and duller; behind you it loses some of its top. Bass and Clarity shape it first, the hall (Echo) follows it round,
// and a limiter at the end keeps even 200% on everything from clipping.
#include "../equalizer/filter_design.hpp"
#include "../voice/voice_color.hpp"
#include "hall_reverb.hpp"
#include <algorithm>
#include <cmath>

namespace spatial {

struct EightSettings {
    int bass = 100, clarity = 100, echo = 100;  // percent, 0..200: 100 as designed
    bool move = true;                           // circling at all; off, the sound stays in front of you
    int speed = 49;                             // 0..100: one turn in 30 s down to 2 s (49 is about 8 s)
    int width = 85;                             // 0..100: how far to each side it goes
    bool operator==(const EightSettings& o) const {
        return bass == o.bass && clarity == o.clarity && echo == o.echo && move == o.move && speed == o.speed && width == o.width;
    }
};

class EightD {
public:
    static constexpr double ITD_SEC = 0.00028;  // how much later the far ear hears it, right at the side
    static constexpr float GAIN = 1.25f;        // equal-power pan, lifted so the centre stays near the original level
    static constexpr float SIDE_KEEP = 0.25f;   // a little of a stereo song's own width stays
    static constexpr int BLOCK = 32;            // the position moves every 0.7 ms, far finer than anyone hears

    // Seconds for one turn at this Speed.
    static double turnSeconds(int speed) { return 30.0 * std::pow(1.0 / 15.0, std::clamp(speed, 0, 100) / 100.0); }

    void init(double fs) {
        fs_ = fs;
        rearA_ = (float)std::exp(-2.0 * eqb::PI_ * 5000.0 / fs);
        relA_ = (float)std::exp(-1.0 / (0.15 * fs));
        hall_.init(fs);
        apply(set_, true);
        reset();
    }

    void reset() {
        std::fill(buf_, buf_ + SIZE, 0.0f);
        w_ = 0;
        left_ = 0;
        angle_ = 0.0;
        depth_ = 0.0;
        rear_ = shadeL_ = shadeR_ = 0.0f;
        lim_ = 1.0f;
        for (eqb::Biquad* q : {&lo_[0], &lo_[1], &hi_[0], &hi_[1], &punch_[0], &punch_[1], &air_[0], &air_[1]}) q->reset();
        hall_.clear();
    }

    void apply(const EightSettings& s, bool force = false) {
        if (!force && s == set_) return;
        const bool tone = force || s.bass != set_.bass || s.clarity != set_.clarity;
        set_ = s;
        step_ = 2.0 * eqb::PI_ / (turnSeconds(s.speed) * fs_);
        if (tone) {
            // Bass: a shelf from -6 dB to +14 dB, and above 100% a punch at 55 Hz on top.
            // Clarity: a shelf from -6 dB to +11 dB at 3 kHz, and above 100% presence at 5 kHz.
            const double b = s.bass / 100.0, c = s.clarity / 100.0;
            const double bassDb = -6.0 + 10.0 * b, punchDb = 5.0 * std::max(0.0, b - 1.0);
            const double clearDb = -6.0 + 8.5 * c, airDb = 4.0 * std::max(0.0, c - 1.0);
            for (int k = 0; k < 2; ++k) {
                eqb::lowShelf(lo_[k], fs_, 100.0, bassDb);
                eqb::peaking(punch_[k], fs_, 55.0, punchDb, 1.2);
                voice::highShelf(hi_[k], fs_, 3000.0, clearDb);
                eqb::peaking(air_[k], fs_, 5000.0, airDb, 1.5);
            }
            trim_ = (float)std::pow(10.0, -(std::max(0.0, bassDb) * 0.15 + std::max(0.0, clearDb) * 0.1) / 20.0);
        }
        // Echo: off at 0, a hall of about 4 s at 100%, a big, wet hall of about 6 s at 200%.
        const double e = s.echo / 100.0;
        wetWant_ = e <= 0.0 ? 0.0f : (float)std::pow(10.0, (-14.0 + 7.0 * e) / 20.0);
        hall_.set(1.8 + 2.0 * e, 0.45 + 0.27 * e);
    }

    // In place on a stereo pair (a mono voice comes in as the same sound on both).
    void process(float* l, float* r, int n) {
        for (int i = 0; i < n; ++i) {
            if (left_ == 0) place();
            --left_;
            float m = shape(0, 0.5f * (l[i] + r[i]));
            const float side = shape(1, 0.5f * (l[i] - r[i]) * SIDE_KEEP);
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
        // The hall hears the sound where it is, so its reflections move round with it.
        if (wetNow_ > 0.0f || wetWant_ > 0.0f) {
            dryL_.assign(l, l + n);
            dryR_.assign(r, r + n);
            wetNow_ += (wetWant_ - wetNow_) * std::min(1.0f, n / (0.05f * (float)fs_));
            if (wetWant_ == 0.0f && wetNow_ < 1e-4f) wetNow_ = 0.0f;
            hall_.process(dryL_.data(), dryR_.data(), l, r, n, wetNow_);
        }
        // Stereo-linked limiter: instant down, 150 ms back up, never above 0.97.
        for (int i = 0; i < n; ++i) {
            const float peak = std::max(std::fabs(l[i]), std::fabs(r[i]));
            const float want = peak > CEILING ? CEILING / peak : 1.0f;
            lim_ = want < lim_ ? want : want + relA_ * (lim_ - want);
            l[i] *= lim_;
            r[i] *= lim_;
        }
    }

private:
    static constexpr int SIZE = 64, MASK = SIZE - 1;
    static constexpr float CEILING = 0.97f;

    double fs_ = 48000.0, step_ = 0.0, angle_ = 0.0, depth_ = 0.0;
    EightSettings set_;
    eqb::Biquad lo_[2], punch_[2], hi_[2], air_[2];  // [0] the source, [1] the width it keeps
    float trim_ = 1.0f;
    float buf_[SIZE] = {};
    int w_ = 0, left_ = 0;
    double delayL_ = 0.0, delayR_ = 0.0;
    float gainL_ = 0.0f, gainR_ = 0.0f, shadeAL_ = 0.0f, shadeAR_ = 0.0f, shadeL_ = 0.0f, shadeR_ = 0.0f;
    float rearA_ = 0.0f, rear_ = 0.0f, back_ = 0.0f;
    HallReverb hall_;
    float wetWant_ = 0.0f, wetNow_ = 0.0f;
    std::vector<float> dryL_, dryR_;
    float lim_ = 1.0f, relA_ = 0.0f;

    float shape(int k, float x) { return air_[k].run(hi_[k].run(punch_[k].run(lo_[k].run(x)))) * trim_; }

    // Where the sound is now: 0 is in front of you, then it goes round to your right, behind, and your left.
    // With Move off it glides back to the front and stays there.
    void place() {
        left_ = BLOCK;
        const double want = set_.move ? set_.width / 100.0 : 0.0;
        depth_ += (want - depth_) * std::min(1.0, BLOCK / (0.4 * fs_));
        if (set_.move) {
            angle_ += step_ * BLOCK;
            if (angle_ >= 2.0 * eqb::PI_) angle_ -= 2.0 * eqb::PI_;
        }
        const double s = std::sin(angle_) * depth_, c = std::cos(angle_);
        gainL_ = GAIN * (float)std::sqrt(0.5 * (1.0 - s));
        gainR_ = GAIN * (float)std::sqrt(0.5 * (1.0 + s));
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
        back_ = (float)(0.45 * depth_ * std::max(0.0, -c));
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
