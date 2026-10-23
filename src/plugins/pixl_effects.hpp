#pragma once
// Pixl's own effects: the everyday ones (gain, stereo, EQ, filter, dynamics, drive, modulation, delay, reverb...)
// built into Pixl, so they work on every PC without installing anything. Each has a handful of parameters in
// natural units; the window's thread sets them, the audio thread picks them up at the next block.
#include "../spatial/hall_reverb.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace pfx {

static const double FS = 48000.0;
static const double PI = 3.14159265358979323846;

inline float dbGain(double db) { return (float)std::pow(10.0, db / 20.0); }
inline double gainDb(double g) { return 20.0 * std::log10(std::max(g, 1e-9)); }
// A one-pole coefficient that settles in about ms milliseconds.
inline float settle(double ms) { return ms <= 0.0 ? 0.0f : (float)std::exp(-1.0 / (ms * 0.001 * FS)); }
// Dry stays full until halfway, then gives way: what a Mix knob on a delay or reverb should do.
inline void mixGains(float mix, float& dry, float& wet) { dry = mix < 0.5f ? 1.0f : 2.0f * (1.0f - mix); wet = mix < 0.5f ? 2.0f * mix : 1.0f; }

enum Curve { LIN, LOG, CHOICE, TOGGLE };

struct Param {
    const wchar_t* name;
    float lo, hi, def;
    const wchar_t* unit;  // L"dB", L"Hz", L"ms", L"s", L"%", L":1", L"bits", L"" ...
    Curve curve = LIN;
    const wchar_t* const* choices = nullptr;  // CHOICE: lo = 0, hi = the last one's index
};

class Effect {
public:
    static const int MAXP = 8;
    virtual ~Effect() = default;

    const wchar_t* id() const { return id_; }
    const wchar_t* name() const { return name_; }
    int count() const { return n_; }
    const Param& param(int i) const { return params_[i]; }

    float value(int i) const { return v_[i].load(std::memory_order_relaxed); }
    void setValue(int i, float x) {
        if (i < 0 || i >= n_) return;
        const Param& p = params_[i];
        x = std::clamp(x, p.lo, p.hi);
        if (p.curve == CHOICE || p.curve == TOGGLE) x = std::round(x);
        v_[i].store(x, std::memory_order_relaxed);
        version_.fetch_add(1, std::memory_order_release);
    }
    void reset(int i) { if (i >= 0 && i < n_) setValue(i, params_[i].def); }

    // 0..1 along the knob (log parameters spread evenly by octaves).
    float norm(int i) const {
        const Param& p = params_[i];
        const float v = value(i);
        if (p.curve == LOG) return (float)(std::log(v / p.lo) / std::log(p.hi / p.lo));
        return p.hi > p.lo ? (v - p.lo) / (p.hi - p.lo) : 0.0f;
    }
    void setNorm(int i, float t) {
        const Param& p = params_[i];
        t = std::clamp(t, 0.0f, 1.0f);
        setValue(i, p.curve == LOG ? (float)(p.lo * std::pow(p.hi / p.lo, t)) : p.lo + t * (p.hi - p.lo));
    }

    std::wstring text(int i) const {
        const Param& p = params_[i];
        const float v = value(i);
        wchar_t t[64];
        const std::wstring u = p.unit;
        if (p.curve == TOGGLE) return v >= 0.5f ? L"On" : L"Off";
        if (p.curve == CHOICE && p.choices) return p.choices[(int)v];
        if (u == L"dB") swprintf_s(t, 64, L"%+.1f dB", v);
        else if (u == L"Hz") { if (v >= 1000.0f) swprintf_s(t, 64, L"%.2f kHz", v / 1000.0f); else swprintf_s(t, 64, v < 10.0f ? L"%.2f Hz" : L"%.0f Hz", v); }
        else if (u == L"ms") swprintf_s(t, 64, v < 10.0f ? L"%.1f ms" : L"%.0f ms", v);
        else if (u == L"s") swprintf_s(t, 64, L"%.2f s", v);
        else if (u == L"%") swprintf_s(t, 64, L"%.0f%%", v);
        else if (u == L":1") swprintf_s(t, 64, L"%.1f:1", v);
        else swprintf_s(t, 64, L"%.0f %s", v, p.unit);
        return t;
    }

    // Audio thread: new values first, then the sound, in place.
    void process(float* l, float* r, int n) {
        const unsigned now = version_.load(std::memory_order_acquire);
        if (now != seen_) { seen_ = now; update(); }
        run(l, r, n);
    }

protected:
    Effect(const wchar_t* id, const wchar_t* name, std::initializer_list<Param> ps) : id_(id), name_(name) {
        for (const Param& p : ps) {
            if (n_ >= MAXP) break;
            params_[n_] = p;
            v_[n_].store(p.def);
            ++n_;
        }
    }
    float p(int i) const { return value(i); }
    virtual void update() {}
    virtual void run(float* l, float* r, int n) = 0;

private:
    const wchar_t* id_;
    const wchar_t* name_;
    Param params_[MAXP] = {};
    std::atomic<float> v_[MAXP];
    int n_ = 0;
    std::atomic<unsigned> version_{1};
    unsigned seen_ = 0;
};

// ---------------------------------------------------------------- pieces

struct BQ {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void clear() { z1 = z2 = 0; }
};
enum BqType { BQ_LP, BQ_HP, BQ_BP, BQ_NOTCH, BQ_PEAK, BQ_LSHELF, BQ_HSHELF };
// The usual cookbook filters.
inline void design(BQ& q, BqType type, double f, double Q, double db = 0.0) {
    f = std::clamp(f, 10.0, FS * 0.49);
    const double w = 2.0 * PI * f / FS, cs = std::cos(w), sn = std::sin(w), alpha = sn / (2.0 * Q), A = std::pow(10.0, db / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
    case BQ_LP: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case BQ_HP: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case BQ_BP: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case BQ_NOTCH: b0 = 1; b1 = -2 * cs; b2 = 1; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case BQ_PEAK: b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
    case BQ_LSHELF: {
        const double s = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
        b0 = A * ((A + 1) - (A - 1) * cs + s); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - s);
        a0 = (A + 1) + (A - 1) * cs + s; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - s;
        break;
    }
    default: {
        const double s = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
        b0 = A * ((A + 1) + (A - 1) * cs + s); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - s);
        a0 = (A + 1) - (A - 1) * cs + s; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - s;
        break;
    }
    }
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
}

// A delay line read any number of samples back, between samples by a straight line.
struct Line {
    std::vector<float> b;
    size_t w = 0;
    void init(double seconds) { b.assign((size_t)(seconds * FS) + 4, 0.0f); w = 0; }
    void push(float x) { b[w] = x; w = (w + 1) % b.size(); }
    float at(double d) const {  // d samples before the newest
        d = std::clamp(d, 0.0, (double)b.size() - 3.0);
        const double pos = (double)w - 1.0 - d + (double)b.size();
        const size_t i0 = (size_t)pos;
        const float f = (float)(pos - (double)i0);
        return b[i0 % b.size()] * (1.0f - f) + b[(i0 + 1) % b.size()] * f;
    }
};

struct OnePole {  // low-pass; hp = x - lp
    float a = 0.0f, z = 0.0f;
    void set(double hz) { a = (float)std::exp(-2.0 * PI * std::clamp(hz, 1.0, FS * 0.49) / FS); }
    float run(float x) { z = x + a * (z - x); return z; }
};

// ---------------------------------------------------------------- the effects

static const wchar_t* const FILTER_TYPES[] = {L"Low pass", L"High pass", L"Band pass", L"Notch"};
static const wchar_t* const SLOPES[] = {L"12 dB/oct", L"24 dB/oct"};
static const wchar_t* const DRIVE_TYPES[] = {L"Soft", L"Hard", L"Tube", L"Fold", L"Sine"};
static const wchar_t* const STAGES[] = {L"2", L"4", L"6", L"8", L"12"};
static const wchar_t* const SIDES[] = {L"Left", L"Right"};
static const wchar_t* const PAN_MODES[] = {L"Pan", L"Tremolo"};

class Gain : public Effect {
public:
    Gain() : Effect(L"gain", L"Gain", {{L"Gain", -36, 24, 0, L"dB"}, {L"Pan", -100, 100, 0, L"%"}}) {}
protected:
    float gl_ = 1, gr_ = 1, wl_ = 1, wr_ = 1;
    void update() override {
        const float g = dbGain(p(0)), pan = p(1) / 100.0f;
        wl_ = g * std::min(1.0f, 1.0f - pan);
        wr_ = g * std::min(1.0f, 1.0f + pan);
    }
    void run(float* l, float* r, int n) override {
        for (int i = 0; i < n; ++i) {
            gl_ += 0.002f * (wl_ - gl_); gr_ += 0.002f * (wr_ - gr_);
            l[i] *= gl_; r[i] *= gr_;
        }
    }
};

class Stereo : public Effect {
public:
    Stereo() : Effect(L"stereo", L"Stereo", {{L"Width", 0, 300, 100, L"%"}, {L"Mid", -24, 12, 0, L"dB"}, {L"Side", -24, 12, 0, L"dB"},
                                              {L"Pan", -100, 100, 0, L"%"}}) {}
protected:
    float mid_ = 1, side_ = 1, pl_ = 1, pr_ = 1, m_ = 1, s_ = 1, cl_ = 1, cr_ = 1;
    void update() override {
        mid_ = dbGain(p(1));
        side_ = dbGain(p(2)) * p(0) / 100.0f;
        const float pan = p(3) / 100.0f;
        pl_ = std::min(1.0f, 1.0f - pan);
        pr_ = std::min(1.0f, 1.0f + pan);
    }
    void run(float* l, float* r, int n) override {
        for (int i = 0; i < n; ++i) {
            m_ += 0.002f * (mid_ - m_); s_ += 0.002f * (side_ - s_); cl_ += 0.002f * (pl_ - cl_); cr_ += 0.002f * (pr_ - cr_);
            const float m = 0.5f * (l[i] + r[i]) * m_, s = 0.5f * (l[i] - r[i]) * s_;
            l[i] = (m + s) * cl_;
            r[i] = (m - s) * cr_;
        }
    }
};

class ChannelMixer : public Effect {
public:
    ChannelMixer() : Effect(L"mixer", L"Channel Mixer", {{L"Left to left", -200, 200, 100, L"%"}, {L"Right to left", -200, 200, 0, L"%"},
                                                          {L"Left to right", -200, 200, 0, L"%"}, {L"Right to right", -200, 200, 100, L"%"}}) {}
protected:
    float k_[4] = {1, 0, 0, 1}, s_[4] = {1, 0, 0, 1};
    void update() override { for (int i = 0; i < 4; ++i) k_[i] = p(i) / 100.0f; }
    void run(float* l, float* r, int n) override {
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < 4; ++k) s_[k] += 0.002f * (k_[k] - s_[k]);
            const float a = l[i], b = r[i];
            l[i] = s_[0] * a + s_[1] * b;
            r[i] = s_[2] * a + s_[3] * b;
        }
    }
};

class Eq3 : public Effect {
public:
    Eq3() : Effect(L"eq3", L"3-Band EQ", {{L"Low", -24, 12, 0, L"dB"}, {L"Mid", -24, 12, 0, L"dB"}, {L"High", -24, 12, 0, L"dB"},
                                         {L"Low / mid", 50, 1000, 250, L"Hz", LOG}, {L"Mid / high", 1000, 12000, 3000, L"Hz", LOG}}) {}
protected:
    BQ lo_[2], hi_[2];
    float g_ = 1, gw_ = 1;
    void update() override {
        // The mid gain on everything, and the shelves take the low and high ends from there.
        gw_ = dbGain(p(1));
        for (int c = 0; c < 2; ++c) {
            design(lo_[c], BQ_LSHELF, p(3), 0.707, p(0) - p(1));
            design(hi_[c], BQ_HSHELF, p(4), 0.707, p(2) - p(1));
        }
    }
    void run(float* l, float* r, int n) override {
        for (int i = 0; i < n; ++i) {
            g_ += 0.002f * (gw_ - g_);
            l[i] = hi_[0].run(lo_[0].run(l[i])) * g_;
            r[i] = hi_[1].run(lo_[1].run(r[i])) * g_;
        }
    }
};

class Filter : public Effect {
public:
    Filter() : Effect(L"filter", L"Filter", {{L"Type", 0, 3, 0, L"", CHOICE, FILTER_TYPES}, {L"Cutoff", 20, 20000, 1000, L"Hz", LOG},
                                             {L"Resonance", 0, 100, 20, L"%"}, {L"Slope", 0, 1, 0, L"", CHOICE, SLOPES}, {L"Mix", 0, 100, 100, L"%"}}) {}
protected:
    BQ q_[2][2];
    double cut_ = 1000.0;
    int left_ = 0;
    void redesign() {
        const double Q = 0.5 + std::pow(p(2) / 100.0, 2.0) * 11.5;
        for (auto& c : q_)
            for (auto& s : c) design(s, (BqType)(int)p(0), cut_, p(3) >= 0.5f ? std::sqrt(Q) : Q);
    }
    void run(float* l, float* r, int n) override {
        const double want = p(1);
        const bool two = p(3) >= 0.5f;
        const float mix = p(4) / 100.0f;
        for (int i = 0; i < n; ++i) {
            if (left_-- <= 0) {
                left_ = 16;
                cut_ = std::exp(std::log(cut_) + 0.25 * (std::log(want) - std::log(cut_)));
                redesign();
            }
            float a = q_[0][0].run(l[i]), b = q_[1][0].run(r[i]);
            if (two) { a = q_[0][1].run(a); b = q_[1][1].run(b); }
            l[i] += mix * (a - l[i]);
            r[i] += mix * (b - r[i]);
        }
    }
};

class Compressor : public Effect {
public:
    Compressor() : Effect(L"compressor", L"Compressor", {{L"Threshold", -60, 0, -18, L"dB"}, {L"Ratio", 1, 20, 4, L":1", LOG},
                                                         {L"Attack", 0.1f, 100, 10, L"ms", LOG}, {L"Release", 10, 1000, 120, L"ms", LOG},
                                                         {L"Knee", 0, 12, 6, L"dB"}, {L"Makeup", 0, 24, 0, L"dB"}, {L"Mix", 0, 100, 100, L"%"}}) {}
protected:
    float env_ = 0.0f, att_ = 0, rel_ = 0, mk_ = 1;
    void update() override { att_ = settle(p(2)); rel_ = settle(p(3)); mk_ = dbGain(p(5)); }
    void run(float* l, float* r, int n) override {
        const float th = p(0), ratio = p(1), knee = p(4), mix = p(6) / 100.0f;
        for (int i = 0; i < n; ++i) {
            const float lv = (float)gainDb(std::max(std::fabs(l[i]), std::fabs(r[i])));
            float over = lv - th, cut = 0.0f;
            if (knee > 0.0f && std::fabs(over) <= knee / 2) cut = (1.0f - 1.0f / ratio) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            else if (over > 0.0f) cut = (1.0f - 1.0f / ratio) * over;
            env_ = cut > env_ ? cut + att_ * (env_ - cut) : cut + rel_ * (env_ - cut);
            const float g = dbGain(-env_) * mk_;
            l[i] += mix * (l[i] * g - l[i]);
            r[i] += mix * (r[i] * g - r[i]);
        }
    }
};

class Limiter : public Effect {
public:
    Limiter() : Effect(L"limiter", L"Limiter", {{L"Input", 0, 24, 0, L"dB"}, {L"Ceiling", -12, 0, -0.3f, L"dB"}, {L"Release", 10, 1000, 100, L"ms", LOG}}) {
        dl_.init(0.01); dr_.init(0.01);
    }
protected:
    static const int LOOK = 72;  // 1.5 ms ahead: the gain is already down when the peak arrives
    Line dl_, dr_;
    float peaks_[LOOK] = {};
    int pp_ = 0;
    float g_ = 1, in_ = 1, ceil_ = 1, rel_ = 0;
    void update() override { in_ = dbGain(p(0)); ceil_ = dbGain(p(1)); rel_ = settle(p(2)); }
    void run(float* l, float* r, int n) override {
        for (int i = 0; i < n; ++i) {
            const float a = l[i] * in_, b = r[i] * in_;
            dl_.push(a); dr_.push(b);
            peaks_[pp_] = std::max(std::fabs(a), std::fabs(b));
            pp_ = (pp_ + 1) % LOOK;
            float most = 0.0f;
            for (float v : peaks_) most = std::max(most, v);
            const float want = most > ceil_ ? ceil_ / most : 1.0f;
            g_ = want < g_ ? g_ + (want - g_) * (4.0f / LOOK) : want + rel_ * (g_ - want);
            l[i] = std::clamp(dl_.at(LOOK - 1) * g_, -ceil_, ceil_);
            r[i] = std::clamp(dr_.at(LOOK - 1) * g_, -ceil_, ceil_);
        }
    }
};

class Gate : public Effect {
public:
    Gate() : Effect(L"gate", L"Gate", {{L"Threshold", -80, 0, -40, L"dB"}, {L"Attack", 0.1f, 50, 1, L"ms", LOG}, {L"Hold", 0, 500, 50, L"ms"},
                                       {L"Release", 5, 1000, 150, L"ms", LOG}, {L"Range", -80, 0, -80, L"dB"}}) {}
protected:
    float env_ = 0, g_ = 0, att_ = 0, rel_ = 0, floor_ = 0, envRel_ = settle(20.0);
    int hold_ = 0;
    bool open_ = false;
    void update() override { att_ = settle(p(1)); rel_ = settle(p(3)); floor_ = dbGain(p(4)); }
    void run(float* l, float* r, int n) override {
        const float th = dbGain(p(0)), shut = th * 0.7f;  // 3 dB lower to close: no chatter
        const int holdN = (int)(p(2) * 0.001 * FS);
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(l[i]), std::fabs(r[i]));
            env_ = x > env_ ? x : x + envRel_ * (env_ - x);
            if (env_ > th) { open_ = true; hold_ = holdN; }
            else if (env_ < shut && hold_-- <= 0) open_ = false;
            const float want = open_ ? 1.0f : floor_;
            g_ = want > g_ ? want + att_ * (g_ - want) : want + rel_ * (g_ - want);
            l[i] *= g_; r[i] *= g_;
        }
    }
};

class Distortion : public Effect {
public:
    Distortion() : Effect(L"distortion", L"Distortion", {{L"Drive", 0, 48, 12, L"dB"}, {L"Type", 0, 4, 0, L"", CHOICE, DRIVE_TYPES},
                                                         {L"Tone", 500, 20000, 8000, L"Hz", LOG}, {L"Output", -24, 6, -6, L"dB"}, {L"Mix", 0, 100, 100, L"%"}}) {}
protected:
    OnePole tone_[2], dc_[2];
    float drive_ = 1, out_ = 1;
    void update() override {
        drive_ = dbGain(p(0)); out_ = dbGain(p(3));
        for (int c = 0; c < 2; ++c) { tone_[c].set(p(2)); dc_[c].set(10.0); }
    }
    float shape(float x) const {
        switch ((int)p(1)) {
        case 1: return std::clamp(x, -1.0f, 1.0f);
        case 2: return x > 0.0f ? std::tanh(x) : std::tanh(0.6f * x) / 0.6f * 0.8f;  // uneven, like a tube
        case 3: { const float t = 0.25f * x + 0.25f; return 4.0f * (std::fabs(t - std::floor(t + 0.5f))) - 1.0f; }  // folds back
        case 4: return std::sin(x);
        default: return std::tanh(x);
        }
    }
    void run(float* l, float* r, int n) override {
        const float mix = p(4) / 100.0f;
        float* ch[2] = {l, r};
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                float y = shape(ch[c][i] * drive_);
                y = tone_[c].run(y);
                y -= dc_[c].run(y);
                ch[c][i] += mix * (y * out_ - ch[c][i]);
            }
    }
};

class Bitcrush : public Effect {
public:
    Bitcrush() : Effect(L"bitcrush", L"Bitcrush", {{L"Bits", 1, 16, 8, L"bits"}, {L"Rate", 200, 48000, 12000, L"Hz", LOG}, {L"Mix", 0, 100, 100, L"%"}}) {}
protected:
    double phase_ = 1.0;
    float hl_ = 0, hr_ = 0;
    void run(float* l, float* r, int n) override {
        const float q = std::pow(2.0f, std::round(p(0)) - 1.0f), mix = p(2) / 100.0f;
        const double step = p(1) / FS;
        for (int i = 0; i < n; ++i) {
            phase_ += step;
            if (phase_ >= 1.0) { phase_ -= std::floor(phase_); hl_ = std::round(l[i] * q) / q; hr_ = std::round(r[i] * q) / q; }
            l[i] += mix * (hl_ - l[i]);
            r[i] += mix * (hr_ - r[i]);
        }
    }
};

// Chorus and flanger: the sound against a copy of itself whose delay sways.
class Sway : public Effect {
protected:
    Sway(const wchar_t* id, const wchar_t* name, std::initializer_list<Param> ps) : Effect(id, name, ps) { dl_.init(0.06); dr_.init(0.06); }
    Line dl_, dr_;
    double ph_ = 0.0;
    float fl_ = 0, fr_ = 0;
    void sway(float* l, float* r, int n, double rate, double depthMs, double baseMs, float fb, double spread, float mix) {
        for (int i = 0; i < n; ++i) {
            ph_ += 2.0 * PI * rate / FS;
            if (ph_ > 2.0 * PI) ph_ -= 2.0 * PI;
            const double a = baseMs + depthMs * 0.5 * (1.0 + std::sin(ph_)), b = baseMs + depthMs * 0.5 * (1.0 + std::sin(ph_ + spread));
            dl_.push(l[i] + fb * fl_);
            dr_.push(r[i] + fb * fr_);
            fl_ = dl_.at(a * 0.001 * FS);
            fr_ = dr_.at(b * 0.001 * FS);
            l[i] += mix * (fl_ - l[i]);
            r[i] += mix * (fr_ - r[i]);
        }
    }
};

class Chorus : public Sway {
public:
    Chorus() : Sway(L"chorus", L"Chorus", {{L"Rate", 0.05f, 5, 0.8f, L"Hz", LOG}, {L"Depth", 0, 10, 3, L"ms"}, {L"Delay", 1, 30, 12, L"ms"},
                                           {L"Feedback", 0, 90, 0, L"%"}, {L"Spread", 0, 180, 90, L"°"}, {L"Mix", 0, 100, 50, L"%"}}) {}
protected:
    void run(float* l, float* r, int n) override { sway(l, r, n, p(0), p(1), p(2), p(3) / 100.0f, p(4) * PI / 180.0, p(5) / 100.0f); }
};

class Flanger : public Sway {
public:
    Flanger() : Sway(L"flanger", L"Flanger", {{L"Rate", 0.02f, 5, 0.25f, L"Hz", LOG}, {L"Depth", 0, 5, 2, L"ms"}, {L"Delay", 0.1f, 10, 1, L"ms", LOG},
                                              {L"Feedback", -95, 95, 60, L"%"}, {L"Mix", 0, 100, 50, L"%"}}) {}
protected:
    void run(float* l, float* r, int n) override { sway(l, r, n, p(0), p(1), p(2), p(3) / 100.0f, PI / 2, p(4) / 100.0f); }
};

class Phaser : public Effect {
public:
    Phaser() : Effect(L"phaser", L"Phaser", {{L"Rate", 0.02f, 5, 0.4f, L"Hz", LOG}, {L"Depth", 0, 100, 80, L"%"}, {L"Centre", 100, 8000, 800, L"Hz", LOG},
                                             {L"Stages", 0, 4, 1, L"", CHOICE, STAGES}, {L"Feedback", -95, 95, 50, L"%"}, {L"Mix", 0, 100, 50, L"%"}}) {}
protected:
    float x1_[2][12] = {}, y1_[2][12] = {}, last_[2] = {};
    double ph_ = 0.0;
    float coef_[2] = {};
    int left_ = 0;
    void run(float* l, float* r, int n) override {
        static const int STAGE_N[] = {2, 4, 6, 8, 12};
        const int stages = STAGE_N[(int)p(3)];
        const float fb = p(4) / 100.0f, mix = p(5) / 100.0f;
        float* ch[2] = {l, r};
        for (int i = 0; i < n; ++i) {
            ph_ += 2.0 * PI * p(0) / FS;
            if (ph_ > 2.0 * PI) ph_ -= 2.0 * PI;
            if (left_-- <= 0) {
                left_ = 16;
                for (int c = 0; c < 2; ++c) {
                    const double f = p(2) * std::pow(2.0, 2.0 * p(1) / 100.0 * std::sin(ph_ + c * PI / 2));
                    const double t = std::tan(PI * std::clamp(f, 20.0, FS * 0.45) / FS);
                    coef_[c] = (float)((t - 1.0) / (t + 1.0));
                }
            }
            for (int c = 0; c < 2; ++c) {
                float x = ch[c][i] + fb * last_[c];
                for (int s = 0; s < stages; ++s) {
                    const float y = coef_[c] * x + x1_[c][s] - coef_[c] * y1_[c][s];
                    x1_[c][s] = x; y1_[c][s] = y; x = y;
                }
                last_[c] = x;
                // Half the sound against half the shifted copy: the notches are deepest at 100%.
                ch[c][i] = (1.0f - 0.5f * mix) * ch[c][i] + 0.5f * mix * x;
            }
        }
    }
};

class Delay : public Effect {
public:
    Delay() : Effect(L"delay", L"Delay", {{L"Time", 1, 2000, 350, L"ms", LOG}, {L"Feedback", 0, 95, 40, L"%"}, {L"Ping-pong", 0, 1, 0, L"", TOGGLE},
                                          {L"Low cut", 20, 2000, 150, L"Hz", LOG}, {L"High cut", 1000, 20000, 6000, L"Hz", LOG}, {L"Mix", 0, 100, 30, L"%"}}) {
        dl_.init(2.2); dr_.init(2.2);
    }
protected:
    Line dl_, dr_;
    OnePole lo_[2], hi_[2];
    double time_ = -1.0;
    void update() override { for (int c = 0; c < 2; ++c) { lo_[c].set(p(3)); hi_[c].set(p(4)); } }
    float tone(int c, float x) { const float y = hi_[c].run(x); return y - lo_[c].run(y); }
    void run(float* l, float* r, int n) override {
        const double want = p(0) * 0.001 * FS;
        if (time_ < 0.0) time_ = want;
        const float fb = p(1) / 100.0f;
        const bool ping = p(2) >= 0.5f;
        float dry, wet;
        mixGains(p(5) / 100.0f, dry, wet);
        for (int i = 0; i < n; ++i) {
            time_ += (want - time_) * 0.0005;  // a change of time glides, like tape
            const float a = dl_.at(time_), b = dr_.at(time_);
            if (ping) {
                dl_.push(tone(0, 0.5f * (l[i] + r[i]) + fb * b));
                dr_.push(tone(1, fb * a));
            } else {
                dl_.push(tone(0, l[i] + fb * a));
                dr_.push(tone(1, r[i] + fb * b));
            }
            l[i] = l[i] * dry + a * wet;
            r[i] = r[i] * dry + b * wet;
        }
    }
};

class Reverb : public Effect {
public:
    Reverb() : Effect(L"reverb", L"Reverb", {{L"Size", 0, 100, 60, L"%"}, {L"Decay", 0.2f, 12, 2.5f, L"s", LOG}, {L"Mix", 0, 100, 30, L"%"}}) {
        hall_.init(FS);
    }
protected:
    spatial::HallReverb hall_;
    std::vector<float> wl_, wr_;
    void update() override { hall_.set(p(1), p(0) / 100.0); }
    void run(float* l, float* r, int n) override {
        wl_.assign(n, 0.0f); wr_.assign(n, 0.0f);
        hall_.process(l, r, wl_.data(), wr_.data(), n, 1.0f);
        float dry, wet;
        mixGains(p(2) / 100.0f, dry, wet);
        for (int i = 0; i < n; ++i) { l[i] = l[i] * dry + wl_[i] * wet; r[i] = r[i] * dry + wr_[i] * wet; }
    }
};

class Haas : public Effect {
public:
    Haas() : Effect(L"haas", L"Haas", {{L"Delay", 0, 40, 15, L"ms"}, {L"Side", 0, 1, 1, L"", CHOICE, SIDES}}) { dl_.init(0.05); dr_.init(0.05); }
protected:
    Line dl_, dr_;
    void run(float* l, float* r, int n) override {
        const double d = p(0) * 0.001 * FS;
        const bool right = p(1) >= 0.5f;
        for (int i = 0; i < n; ++i) {
            dl_.push(l[i]); dr_.push(r[i]);
            if (right) r[i] = dr_.at(d); else l[i] = dl_.at(d);
        }
    }
};

class TranceGate : public Effect {
public:
    TranceGate() : Effect(L"trancegate", L"Trance Gate", {{L"Rate", 0.5f, 32, 8, L"Hz", LOG}, {L"Length", 5, 95, 50, L"%"}, {L"Depth", 0, 100, 100, L"%"},
                                                          {L"Smooth", 0, 20, 3, L"ms"}}) {}
protected:
    double ph_ = 0.0;
    float g_ = 1.0f;
    void run(float* l, float* r, int n) override {
        const float a = settle(std::max(0.05f, p(3))), low = 1.0f - p(2) / 100.0f, len = p(1) / 100.0f;
        for (int i = 0; i < n; ++i) {
            ph_ += p(0) / FS;
            if (ph_ >= 1.0) ph_ -= 1.0;
            const float want = ph_ < len ? 1.0f : low;
            g_ = want + a * (g_ - want);
            l[i] *= g_; r[i] *= g_;
        }
    }
};

class TapeStop : public Effect {
public:
    TapeStop() : Effect(L"tapestop", L"Tape Stop", {{L"Stop", 0, 1, 0, L"", TOGGLE}, {L"Time", 50, 4000, 800, L"ms", LOG}}) { dl_.init(5.0); dr_.init(5.0); }
protected:
    Line dl_, dr_;
    double lag_ = 0.0, speed_ = 1.0;
    float back_ = 1.0f;
    void run(float* l, float* r, int n) override {
        const bool stop = p(0) >= 0.5f;
        const double fall = 1.0 / (p(1) * 0.001 * FS);
        for (int i = 0; i < n; ++i) {
            dl_.push(l[i]); dr_.push(r[i]);
            if (stop) {
                // Slowing down: the tape falls behind, the pitch falls with it, and it stops.
                speed_ = std::max(0.0, speed_ - fall);
                lag_ = std::min(lag_ + (1.0 - speed_), 4.9 * FS);
                back_ = 0.0f;
            } else if (lag_ > 0.0) {
                // Let go: back to the live sound with a short fade.
                back_ = std::min(1.0f, back_ + 1.0f / (0.03f * (float)FS));
                if (back_ >= 1.0f) { lag_ = 0.0; speed_ = 1.0; }
            }
            if (lag_ > 0.0) {
                const float fade = speed_ > 0.02 ? 1.0f : (float)(speed_ / 0.02);
                const float a = dl_.at(lag_) * fade, b = dr_.at(lag_) * fade;
                l[i] = a + back_ * (l[i] - a);
                r[i] = b + back_ * (r[i] - b);
            }
        }
    }
};

class RingMod : public Effect {
public:
    RingMod() : Effect(L"ringmod", L"Ring Mod", {{L"Frequency", 1, 5000, 200, L"Hz", LOG}, {L"Mix", 0, 100, 50, L"%"}}) {}
protected:
    double ph_ = 0.0;
    void run(float* l, float* r, int n) override {
        const float mix = p(1) / 100.0f;
        for (int i = 0; i < n; ++i) {
            ph_ += 2.0 * PI * p(0) / FS;
            if (ph_ > 2.0 * PI) ph_ -= 2.0 * PI;
            const float s = (float)std::sin(ph_);
            l[i] += mix * (l[i] * s - l[i]);
            r[i] += mix * (r[i] * s - r[i]);
        }
    }
};

class AutoPan : public Effect {
public:
    AutoPan() : Effect(L"autopan", L"Auto Pan", {{L"Rate", 0.05f, 20, 2, L"Hz", LOG}, {L"Depth", 0, 100, 50, L"%"}, {L"Mode", 0, 1, 0, L"", CHOICE, PAN_MODES}}) {}
protected:
    double ph_ = 0.0;
    void run(float* l, float* r, int n) override {
        const float d = p(1) / 100.0f;
        const bool trem = p(2) >= 0.5f;
        for (int i = 0; i < n; ++i) {
            ph_ += 2.0 * PI * p(0) / FS;
            if (ph_ > 2.0 * PI) ph_ -= 2.0 * PI;
            const float s = (float)std::sin(ph_);
            if (trem) {
                const float g = 1.0f - d * 0.5f * (1.0f + s);
                l[i] *= g; r[i] *= g;
            } else {
                const float a = (float)(PI / 4) * (1.0f + d * s);
                l[i] *= std::cos(a) * 1.4142f;
                r[i] *= std::sin(a) * 1.4142f;
            }
        }
    }
};

// ---------------------------------------------------------------- the list

struct Entry { const wchar_t* id; const wchar_t* name; std::unique_ptr<Effect> (*make)(); };
template <class T> std::unique_ptr<Effect> makeOne() { return std::make_unique<T>(); }

inline const std::vector<Entry>& catalog() {
    static const std::vector<Entry> list = {
        {L"gain", L"Gain", makeOne<Gain>}, {L"stereo", L"Stereo", makeOne<Stereo>}, {L"mixer", L"Channel Mixer", makeOne<ChannelMixer>},
        {L"eq3", L"3-Band EQ", makeOne<Eq3>}, {L"filter", L"Filter", makeOne<Filter>}, {L"compressor", L"Compressor", makeOne<Compressor>},
        {L"limiter", L"Limiter", makeOne<Limiter>}, {L"gate", L"Gate", makeOne<Gate>}, {L"distortion", L"Distortion", makeOne<Distortion>},
        {L"bitcrush", L"Bitcrush", makeOne<Bitcrush>}, {L"chorus", L"Chorus", makeOne<Chorus>}, {L"flanger", L"Flanger", makeOne<Flanger>},
        {L"phaser", L"Phaser", makeOne<Phaser>}, {L"delay", L"Delay", makeOne<Delay>}, {L"reverb", L"Reverb", makeOne<Reverb>},
        {L"haas", L"Haas", makeOne<Haas>}, {L"trancegate", L"Trance Gate", makeOne<TranceGate>}, {L"tapestop", L"Tape Stop", makeOne<TapeStop>},
        {L"ringmod", L"Ring Mod", makeOne<RingMod>}, {L"autopan", L"Auto Pan", makeOne<AutoPan>},
    };
    return list;
}

// "pixl:reverb" -> a new Reverb; nullptr for anything else.
static const wchar_t* const PREFIX = L"pixl:";
inline bool isPixlPath(const std::wstring& path) { return path.compare(0, 5, PREFIX) == 0; }
inline std::unique_ptr<Effect> make(const std::wstring& path) {
    if (!isPixlPath(path)) return nullptr;
    const std::wstring id = path.substr(5);
    for (const Entry& e : catalog())
        if (id == e.id) return e.make();
    return nullptr;
}

}  // namespace pfx
