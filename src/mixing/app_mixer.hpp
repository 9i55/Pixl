#pragma once
// App mixer: groups programs into strips like Voicemeeter. Each strip sets the
// Windows volume of its programs, mutes or solos them, adds an echo
// and can send them to the mic (B1) through a process loopback tap.

#include "../playback/program_tap.hpp"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <map>
#include <functional>
#include <memory>

namespace mixer {

constexpr int STRIPS = 3;
constexpr int OUTS = 3;
constexpr int MAX_APPS = 6;
constexpr int MIN_DB10 = -600;

inline float volumeOf(int db10) { return db10 <= MIN_DB10 ? 0.0f : powf(10.0f, db10 / 200.0f); }

struct Strip {
    std::wstring title;
    std::vector<std::wstring> apps;
    int db10 = 0;  // fader in tenths of a dB, MIN_DB10 (bottom) is silence
    bool mute = false, solo = false, b1 = false, echo = false;
    bool audio = true;  // off: you hear nothing; with B1 the program still reaches the mic
    int out = -1;
};

inline bool sameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

struct Mixer {
    std::mutex mx;
    Strip strips[STRIPS];
    std::wstring outs[OUTS];
    std::atomic<float> level[STRIPS];
    std::atomic<bool> b1Live{false};
    std::atomic<bool> echoLive{false};
    std::atomic<bool> echoAny{false};  // some program has Echo on (heard or mic only)
    std::wstring skipName;
    std::wstring duckName;  // the program Pixl turned down to play it itself: its volume is Pixl's
    std::function<std::wstring()> cableFinder;  // id of the VB-CABLE playback device (the mic path)

    Mixer() {
        for (int i = 0; i < STRIPS; ++i) { strips[i].title = L"Audio " + std::to_wstring(i + 1); level[i] = 0.0f; }
    }
    ~Mixer() { stop(); }

    void start(int fs) {
        if (th.joinable()) return;
        rate = fs;
        quit = false;
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        th = std::thread([this] { run(); });
    }

    void stop() {
        if (!th.joinable()) return;
        quit = true;
        if (wake) SetEvent(wake);
        th.join();
        if (wake) { CloseHandle(wake); wake = nullptr; }
    }

    void poke() { if (wake) SetEvent(wake); }

    // Called by the audio thread. dry: Mic programs for the mic. Echo buses: mic: heard and on the
    // mic. mine: only heard. cab: only on the mic (Audio off).
    void pull(float* dryL, float* dryR, float* micL, float* micR, float* mineL, float* mineR, float* cabL, float* cabR, int n) {
        for (float* b : {dryL, dryR, micL, micR, mineL, mineR, cabL, cabR}) std::fill(b, b + n, 0.0f);
        std::unique_lock<std::mutex> lk(tapMx, std::try_to_lock);
        if (!lk.owns_lock()) return;
        tl.resize((size_t)n); tr.resize((size_t)n);
        for (auto& t : taps) {
            const Tap& tp = t.second;
            if (!tp.tap->active()) continue;
            tp.tap->pull(tl.data(), tr.data(), n);  // always drained, so it stays in step
            if (!tp.open) continue;
            if (tp.dry) for (int i = 0; i < n; ++i) { dryL[i] += tl[i]; dryR[i] += tr[i]; }
            if (tp.echo) {
                float* l = !tp.hear ? cabL : tp.b1 ? micL : mineL;
                float* r = !tp.hear ? cabR : tp.b1 ? micR : mineR;
                for (int i = 0; i < n; ++i) { l[i] += tl[i]; r[i] += tr[i]; }
            }
        }
    }

    // Which strip holds a program, or -1.
    int stripOf(const std::wstring& name) {
        std::lock_guard<std::mutex> lk(mx);
        return findStrip(name);
    }

    void place(const std::wstring& name, int strip) {
        {
            std::lock_guard<std::mutex> lk(mx);
            for (Strip& s : strips)
                s.apps.erase(std::remove_if(s.apps.begin(), s.apps.end(), [&](const std::wstring& a) { return sameName(a, name); }), s.apps.end());
            if (strip >= 0 && strip < STRIPS && (int)strips[strip].apps.size() < MAX_APPS) strips[strip].apps.push_back(name);
        }
        poke();
    }

    void load(const std::wstring& key) {
        std::lock_guard<std::mutex> lk(mx);
        auto num = [&](const std::wstring& n, int def) {
            DWORD v = 0, cb = sizeof v;
            return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), n.c_str(), RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? (int)v : def;
        };
        auto text = [&](const std::wstring& n) {
            wchar_t buf[2048];
            DWORD cb = sizeof buf - sizeof(wchar_t);
            return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), n.c_str(), RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS ? std::wstring(buf) : std::wstring();
        };
        for (int o = 0; o < OUTS; ++o) outs[o] = text(L"Out" + std::to_wstring(o));
        for (int i = 0; i < STRIPS; ++i) {
            Strip& s = strips[i];
            const std::wstring p = L"S" + std::to_wstring(i);
            const std::wstring t = text(p + L"Name");
            if (!t.empty()) s.title = t;
            if (s.title.rfind(L"Strip ", 0) == 0) s.title = L"Audio " + s.title.substr(6);  // old default name
            {
                // Older saves kept a percentage; the fader now works in dB.
                const int pct = std::max(0, std::min(100, num(p + L"Gain", 100)));
                const int fromPct = pct <= 0 ? MIN_DB10 : (int)lroundf(200.0f * log10f(pct / 100.0f));
                s.db10 = std::max(MIN_DB10, std::min(0, num(p + L"GainDb10", fromPct)));
            }
            s.mute = num(p + L"Mute", 0) != 0;
            s.solo = num(p + L"Solo", 0) != 0;
            s.b1 = num(p + L"B1", 0) != 0;
            s.echo = num(p + L"Echo", 0) != 0;
            s.audio = num(p + L"Audio", 1) != 0;
            s.out = -1;  // no A1-A3 buttons: programs play on their usual device
            s.apps.clear();
            const std::wstring all = text(p + L"Apps");
            size_t from = 0;
            while (from < all.size() && (int)s.apps.size() < MAX_APPS) {
                const size_t bar = all.find(L'|', from);
                const std::wstring a = all.substr(from, bar == std::wstring::npos ? std::wstring::npos : bar - from);
                if (!a.empty()) s.apps.push_back(a);
                if (bar == std::wstring::npos) break;
                from = bar + 1;
            }
        }
    }

    void save(const std::wstring& key) {
        std::lock_guard<std::mutex> lk(mx);
        HKEY hk;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS) return;
        auto putNum = [&](const std::wstring& n, int v) { DWORD d = (DWORD)v; RegSetValueExW(hk, n.c_str(), 0, REG_DWORD, (const BYTE*)&d, sizeof d); };
        auto putText = [&](const std::wstring& n, const std::wstring& v) {
            RegSetValueExW(hk, n.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        };
        for (int o = 0; o < OUTS; ++o) putText(L"Out" + std::to_wstring(o), outs[o]);
        for (int i = 0; i < STRIPS; ++i) {
            const Strip& s = strips[i];
            const std::wstring p = L"S" + std::to_wstring(i);
            std::wstring all;
            for (const std::wstring& a : s.apps) { if (!all.empty()) all += L"|"; all += a; }
            putText(p + L"Name", s.title);
            putText(p + L"Apps", all);
            putNum(p + L"GainDb10", s.db10);
            putNum(p + L"Mute", s.mute);
            putNum(p + L"Solo", s.solo);
            putNum(p + L"B1", s.b1);
            putNum(p + L"Echo", s.echo);
            putNum(p + L"Audio", s.audio);
            putNum(p + L"Out", s.out);
        }
        RegCloseKey(hk);
    }

private:
    struct Tap {
        std::unique_ptr<clip::ProgramTap> tap;
        bool open = true, b1 = false, echo = false, dry = false, hear = true;
        int strip = -1;
    };
    struct Touched {
        float firstVolume = 1.0f;
        std::wstring routedTo;
        bool routed = false;
        std::vector<DWORD> pids;
    };

    std::thread th;
    std::atomic<bool> quit{false};
    HANDLE wake = nullptr;
    int rate = 48000;
    std::mutex tapMx;
    std::map<std::wstring, Tap> taps;
    std::map<std::wstring, Touched> touched;
    std::vector<float> tl, tr;

    int findStrip(const std::wstring& name) const {
        for (int i = 0; i < STRIPS; ++i)
            for (const std::wstring& a : strips[i].apps)
                if (sameName(a, name)) return i;
        return -1;
    }

    static std::wstring lower(std::wstring s) {
        for (auto& c : s) c = towlower(c);
        return s;
    }

    struct Session {
        DWORD pid;
        std::wstring name;
        ISimpleAudioVolume* vol;
        IAudioMeterInformation* meter;
    };

    static std::vector<Session> sessions(const clip::ProcessTable& table) {
        std::vector<Session> out;
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) return out;
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d)) || !d) continue;
                IAudioSessionManager2* mgr = nullptr;
                if (SUCCEEDED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
                    IAudioSessionEnumerator* se = nullptr;
                    if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                        int cnt = 0;
                        se->GetCount(&cnt);
                        for (int k = 0; k < cnt; ++k) {
                            IAudioSessionControl* sc = nullptr;
                            if (FAILED(se->GetSession(k, &sc)) || !sc) continue;
                            AudioSessionState st = AudioSessionStateExpired;
                            sc->GetState(&st);
                            IAudioSessionControl2* sc2 = nullptr;
                            if (st != AudioSessionStateExpired && SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                                DWORD pid = 0;
                                sc2->GetProcessId(&pid);
                                const std::wstring name = pid ? table.name(pid) : L"";
                                if (!name.empty()) {
                                    Session s{pid, name, nullptr, nullptr};
                                    sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&s.vol);
                                    sc->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&s.meter);
                                    out.push_back(s);
                                }
                                sc2->Release();
                            }
                            sc->Release();
                        }
                        se->Release();
                    }
                    mgr->Release();
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
        return out;
    }

    static void route(const std::vector<DWORD>& pids, const std::wstring& deviceId) {
        clip::IAudioPolicyConfig* fac = clip::openPolicy();
        if (!fac) return;
        const HMODULE combase = GetModuleHandleW(L"combase.dll");
        auto mkStr = combase ? (HRESULT (WINAPI *)(const wchar_t*, UINT32, HSTRING*))GetProcAddress(combase, "WindowsCreateString") : nullptr;
        auto delStr = combase ? (HRESULT (WINAPI *)(HSTRING))GetProcAddress(combase, "WindowsDeleteString") : nullptr;
        HSTRING want = clip::makeHs(clip::packRenderId(deviceId), mkStr);
        HSTRING raw = clip::makeHs(deviceId, mkStr);
        static const ERole roles[3] = {eConsole, eMultimedia, eCommunications};
        for (DWORD pid : pids)
            for (ERole r : roles) {
                fac->SetPersistedDefaultAudioEndpoint(pid, eRender, r, want);
                if (raw) fac->SetPersistedDefaultAudioEndpoint(pid, eRender, r, raw);
            }
        if (want && delStr) delStr(want);
        if (raw && delStr) delStr(raw);
        fac->Release();
    }

    void step() {
        Strip snap[STRIPS];
        std::wstring outIds[OUTS], skip, duck;
        {
            std::lock_guard<std::mutex> lk(mx);
            for (int i = 0; i < STRIPS; ++i) snap[i] = strips[i];
            for (int o = 0; o < OUTS; ++o) outIds[o] = outs[o];
            skip = skipName;
            duck = duckName;
        }
        bool anySolo = false;
        for (const Strip& s : snap) anySolo = anySolo || s.solo;
        auto stripFor = [&](const std::wstring& name) {
            for (int i = 0; i < STRIPS; ++i)
                for (const std::wstring& a : snap[i].apps)
                    if (sameName(a, name)) return i;
            return -1;
        };
        auto silent = [&](int i) { return snap[i].mute || (anySolo && !snap[i].solo); };
        // Audio off: you hear nothing. With Mic on the program plays straight into the mic
        // cable instead of your headphones, so the others still hear it.
        auto toCable = [&](int i) { return !snap[i].audio && snap[i].b1 && !silent(i); };
        auto quiet = [&](int i) { return silent(i) || (!snap[i].audio && !snap[i].b1); };
        auto needTap = [&](int i) { return i >= 0; };  // every strip program is captured: its meter comes from it
        std::wstring cable;
        for (int i = 0; i < STRIPS; ++i)
            if (toCable(i) && !snap[i].apps.empty() && cableFinder) { cable = cableFinder(); break; }

        const clip::ProcessTable table;
        std::vector<Session> list = sessions(table);
        std::map<std::wstring, std::vector<DWORD>> pidsOf;
        std::map<std::wstring, DWORD> rootOf;
        for (Session& s : list) {
            const int i = stripFor(s.name);
            const std::wstring key = lower(s.name);
            const bool ducked = !duck.empty() && sameName(s.name, duck);
            if (i >= 0) {
                if (s.vol && !ducked) {
                    auto seen = touched.find(key);
                    if (seen == touched.end()) {
                        float v = 1.0f;
                        s.vol->GetMasterVolume(&v);
                        seen = touched.emplace(key, Touched{}).first;
                        seen->second.firstVolume = v;
                    }
                    const float want = quiet(i) ? 0.0f : volumeOf(snap[i].db10);
                    float have = -1.0f;
                    s.vol->GetMasterVolume(&have);
                    if (fabsf(have - want) > 0.004f) s.vol->SetMasterVolume(want, nullptr);
                }
                auto& v = pidsOf[key];
                if (std::find(v.begin(), v.end(), s.pid) == v.end()) v.push_back(s.pid);
                if (!rootOf.count(key)) rootOf[key] = table.root(s.pid);
            } else if (touched.count(key) && !ducked) {
                // Taken out of every strip: give back the volume it had.
                if (s.vol) s.vol->SetMasterVolume(touched[key].firstVolume, nullptr);
            }
            if (s.vol) s.vol->Release();
            if (s.meter) s.meter->Release();
        }

        // Output device per program: the mic cable while Audio is off and Mic is on, else the Windows default.
        for (auto& p : pidsOf) {
            const int i = stripFor(p.first);
            if (i < 0 || (!skip.empty() && sameName(p.first, skip))) continue;
            const std::wstring want = toCable(i) ? cable : L"";
            Touched& t = touched[p.first];
            bool fresh = false;
            for (DWORD pid : p.second)
                if (std::find(t.pids.begin(), t.pids.end(), pid) == t.pids.end()) { t.pids.push_back(pid); fresh = true; }
            if (want.empty() && !t.routed) continue;
            if (t.routed && t.routedTo == want && !fresh) continue;
            route(p.second, want);
            t.routedTo = want;
            t.routed = !want.empty();
        }
        // Programs that left their strip go back to the Windows default device.
        for (auto it = touched.begin(); it != touched.end();) {
            if (stripFor(it->first) >= 0) { ++it; continue; }
            if (it->second.routed) route(it->second.pids, L"");
            it = touched.erase(it);
        }

        // One loopback tap per strip program: it feeds the meter, and B1 and Echo when they are on.
        std::map<std::wstring, int> wantTap;
        for (auto& r : rootOf) {
            const int i = stripFor(r.first);
            if (i < 0 || !needTap(i)) continue;
            wantTap[r.first] = i;
        }
        auto setFlags = [&](Tap& t, int i) {
            t.open = !quiet(i);
            t.dry = snap[i].b1 && snap[i].audio;  // with Audio off the cable already carries it
            t.b1 = snap[i].b1;
            t.echo = snap[i].echo;
            t.hear = snap[i].audio;
            t.strip = i;
        };
        std::vector<std::unique_ptr<clip::ProgramTap>> drop;
        {
            std::lock_guard<std::mutex> lk(tapMx);
            // A tap lives until its program closes or leaves B1/Echo. A program whose sound
            // session blinks out between two scans keeps its tap, so the echo never drops.
            for (auto it = taps.begin(); it != taps.end();) {
                const int i = stripFor(it->first);
                const bool dead = it->second.tap->finished() || i < 0 || !needTap(i);
                if (dead) { drop.push_back(std::move(it->second.tap)); it = taps.erase(it); }
                else { setFlags(it->second, i); ++it; }
            }
            for (auto& w : wantTap) {
                if (taps.count(w.first)) continue;
                Tap t;
                t.tap = std::make_unique<clip::ProgramTap>();
                t.tap->handsOff = true;
                t.tap->start(clip::Program{rootOf[w.first], w.first}, rate);
                setFlags(t, w.second);
                taps.emplace(w.first, std::move(t));
            }
            b1Live = !taps.empty();
            bool echo = false;
            for (auto& t : taps) echo = echo || (t.second.echo && t.second.open && t.second.hear);
            echoLive = echo;
            bool anyEcho = false;
            for (auto& t : taps) anyEcho = anyEcho || (t.second.echo && t.second.open);
            echoAny = anyEcho;
        }
        drop.clear();
    }

    // Meters come from each program's own captured sound. Windows' session meters can't be
    // trusted here: on some setups several programs report the same level as another one.
    void readMeters() {
        float peak[STRIPS] = {};
        {
            std::lock_guard<std::mutex> lk(tapMx);
            for (auto& t : taps) {
                const int i = t.second.strip;
                if (i < 0 || i >= STRIPS) continue;
                peak[i] = std::max(peak[i], t.second.tap->peakHold.exchange(0.0f));
            }
        }
        for (int i = 0; i < STRIPS; ++i) level[i] = std::max(peak[i], level[i].load() * 0.75f);  // fall back smoothly
    }

    void giveBack() {
        {
            std::lock_guard<std::mutex> lk(tapMx);
            taps.clear();
            b1Live = false;
            echoLive = false;
            echoAny = false;
        }
        std::wstring duck;
        {
            std::lock_guard<std::mutex> lk(mx);
            duck = duckName;
        }
        const clip::ProcessTable table;
        std::vector<Session> list = sessions(table);
        for (Session& s : list) {
            auto t = touched.find(lower(s.name));
            if (t != touched.end() && s.vol && !(!duck.empty() && sameName(s.name, duck))) s.vol->SetMasterVolume(t->second.firstVolume, nullptr);
            if (s.vol) s.vol->Release();
            if (s.meter) s.meter->Release();
        }
        for (auto& t : touched)
            if (t.second.routed) route(t.second.pids, L"");
        touched.clear();
        for (int i = 0; i < STRIPS; ++i) level[i] = 0.0f;
    }

    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ULONGLONG nextFull = 0;
        while (!quit.load()) {
            if (GetTickCount64() >= nextFull) {
                step();
                nextFull = GetTickCount64() + 700;
            } else {
                readMeters();
            }
            if (WaitForSingleObject(wake, 60) == WAIT_OBJECT_0) nextFull = 0;
        }
        giveBack();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

}
