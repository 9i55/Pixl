#pragma once
// Hosting VST2 effects (Kilohearts' kHs plugins and any other VST 2.4 effect). The plugin ABI below is written from
// the format's public description: a plugin DLL exports VSTPluginMain (or main), which returns an AEffect the host
// talks to through its dispatcher. VST2 rather than VST3: the same Kilohearts binaries carry both, and VST2 needs no
// SDK and a few hundred lines of host, with everything a plugin offers (its own window, its parameters, its state).
#include "pixl_effects.hpp"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vst2 {

struct AEffect;
using HostCallback = intptr_t (*)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using Dispatcher = intptr_t (*)(AEffect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessProc = void (*)(AEffect*, float** in, float** out, int32_t frames);
using SetParameterProc = void (*)(AEffect*, int32_t index, float value);
using GetParameterProc = float (*)(AEffect*, int32_t index);
using EntryPoint = AEffect* (*)(HostCallback);

struct AEffect {
    int32_t magic;  // 'VstP'
    Dispatcher dispatcher;
    ProcessProc process;  // old accumulating process, not used
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms, numParams, numInputs, numOutputs;
    int32_t flags;
    intptr_t reserved1, reserved2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void* object;
    void* user;  // the host's: points back at the Plugin
    int32_t uniqueID, version;
    ProcessProc processReplacing;
    void* processDoubleReplacing;
    char future[56];
};

struct ERect { int16_t top, left, bottom, right; };

struct TimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

enum : int32_t {
    kMagic = 'VstP',
    // effect opcodes
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19,
    effGetChunk = 23, effSetChunk = 24, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effCanDo = 51, effStartProcess = 71, effStopProcess = 72,
    // host opcodes
    hostAutomate = 0, hostVersion = 1, hostCurrentId = 2, hostIdle = 3, hostGetTime = 7, hostProcessEvents = 8,
    hostIOChanged = 13, hostSizeWindow = 15, hostGetSampleRate = 16, hostGetBlockSize = 17, hostGetInputLatency = 18,
    hostGetOutputLatency = 19, hostGetCurrentProcessLevel = 23, hostGetAutomationState = 24, hostGetVendorString = 32,
    hostGetProductString = 33, hostGetVendorVersion = 34, hostCanDo = 37, hostGetLanguage = 38,
    hostGetDirectoryPath = 41, hostUpdateDisplay = 42, hostBeginEdit = 43, hostEndEdit = 44,
    // flags
    flagHasEditor = 1, flagCanReplacing = 1 << 4, flagProgramChunks = 1 << 5, flagIsSynth = 1 << 8,
    // time info flags
    timeTransportPlaying = 1 << 1, timeNanosValid = 1 << 8, timePpqValid = 1 << 9, timeTempoValid = 1 << 10,
    timeBarsValid = 1 << 11, timeSigValid = 1 << 13,
};

static const double kSampleRate = 48000.0;
static const int kMaxBlock = 4096;

// One loaded effect. Opened, resumed and taken down on the window's thread; process() runs on the audio thread.
class Plugin {
public:
    std::wstring path, name;

    ~Plugin() { unload(); }

    // Loads the DLL and starts the effect at Pixl's rate. False (with why()) when it is not a usable VST2 effect.
    // A path like "pixl:reverb" is one of Pixl's own effects instead: nothing to load, on every PC.
    bool load(const std::wstring& file) {
        path = file;
        if (pfx::isPixlPath(file)) {
            fx_ = pfx::make(file);
            name = fx_ ? fx_->name() : file.substr(5);
            if (!fx_) { why_ = name + L" is not in this version of Pixl"; return false; }
            return true;
        }
        const size_t slash = file.find_last_of(L"\\/");
        name = file.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
        // Its own folder first, so a plugin finds the DLLs it ships beside itself.
        module_ = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module_) return fail(L"could not be opened");
        auto entry = (EntryPoint)GetProcAddress(module_, "VSTPluginMain");
        if (!entry) entry = (EntryPoint)GetProcAddress(module_, "main");
        if (!entry) return fail(L"is not a VST2 plugin");
        effect_ = start(entry);
        if (!effect_ || effect_->magic != kMagic) { effect_ = nullptr; return fail(L"did not start"); }
        if (!(effect_->flags & flagCanReplacing) || !effect_->processReplacing) return fail(L"cannot process in place");
        if (effect_->flags & flagIsSynth) return fail(L"is an instrument, not an effect");
        effect_->user = this;
        call(effOpen);
        call(effSetSampleRate, 0, 0, nullptr, (float)kSampleRate);
        call(effSetBlockSize, 0, kMaxBlock);
        const int channels = (int)(effect_->numInputs > effect_->numOutputs ? effect_->numInputs : effect_->numOutputs);
        channels_ = channels < 2 ? 2 : channels;
        in_.assign(channels_, std::vector<float>(kMaxBlock, 0.0f));
        out_.assign(channels_, std::vector<float>(kMaxBlock, 0.0f));
        inPtr_.resize(channels_);
        outPtr_.resize(channels_);
        for (int c = 0; c < channels_; ++c) { inPtr_[c] = in_[c].data(); outPtr_[c] = out_[c].data(); }
        char product[256] = {};
        call(effGetEffectName, 0, 0, product);
        if (product[0]) {
            int n = MultiByteToWideChar(CP_UTF8, 0, product, -1, nullptr, 0);
            if (n > 1) { std::wstring w(n - 1, L'\0'); MultiByteToWideChar(CP_UTF8, 0, product, -1, w.data(), n); displayName_ = w; }
        }
        call(effMainsChanged, 0, 1);
        call(effStartProcess);
        return true;
    }

    // Stops and frees the effect (its window must be closed first).
    void unload() {
        fx_.reset();
        if (effect_) {
            call(effStopProcess);
            call(effMainsChanged, 0, 0);
            call(effClose);
            effect_ = nullptr;
        }
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
    }

    bool loaded() const { return effect_ != nullptr || fx_ != nullptr; }
    // One of Pixl's own effects (its window is drawn by Pixl), else nullptr.
    pfx::Effect* builtin() const { return fx_.get(); }
    const std::wstring& why() const { return why_; }
    // What the plugin calls itself ("Reverb"), else the file's name.
    std::wstring title() const { return displayName_.empty() ? name : displayName_; }
    bool hasEditor() const { return fx_ || (effect_ && (effect_->flags & flagHasEditor)); }

    // A plugin that crashed while processing is left out from then on (Pixl keeps running).
    bool crashed() const { return crashed_.load(); }

    // Runs the effect on a stereo block in place (any length: longer blocks go in pieces).
    void process(float* l, float* r, int frames) {
        if (fx_) { fx_->process(l, r, frames); return; }
        if (!effect_ || crashed_.load()) return;
        for (int done = 0; done < frames;) {
            const int n = frames - done < kMaxBlock ? frames - done : kMaxBlock;
            const int ins = effect_->numInputs;
            if (ins == 1) {
                for (int i = 0; i < n; ++i) in_[0][i] = 0.5f * (l[done + i] + r[done + i]);
            } else {
                memcpy(in_[0].data(), l + done, n * sizeof(float));
                memcpy(in_[1].data(), r + done, n * sizeof(float));
                for (int c = 2; c < channels_; ++c) memset(in_[c].data(), 0, n * sizeof(float));
            }
            time_.samplePos = (double)samples_;
            time_.ppqPos = time_.samplePos / kSampleRate * time_.tempo / 60.0;
            if (!run(effect_, inPtr_.data(), outPtr_.data(), n)) { crashed_ = true; return; }
            samples_ += n;
            if (effect_->numOutputs == 1) {
                memcpy(l + done, out_[0].data(), n * sizeof(float));
                memcpy(r + done, out_[0].data(), n * sizeof(float));
            } else if (effect_->numOutputs >= 2) {
                memcpy(l + done, out_[0].data(), n * sizeof(float));
                memcpy(r + done, out_[1].data(), n * sizeof(float));
            }
            done += n;
        }
    }

    // The plugin's own window, inside parent (size from the plugin). False when it has none.
    bool openEditor(HWND parent, int& width, int& height) {
        if (!hasEditor()) return false;
        if (fx_) { width = height = 0; editorOpen_ = true; return true; }  // Pixl draws it
        ERect* rect = nullptr;
        call(effEditGetRect, 0, 0, &rect);
        call(effEditOpen, 0, 0, parent);
        // Some plugins know their size only once the window is open.
        rect = nullptr;
        call(effEditGetRect, 0, 0, &rect);
        width = rect ? rect->right - rect->left : 640;
        height = rect ? rect->bottom - rect->top : 400;
        editorOpen_ = true;
        return true;
    }
    void editorIdle() { if (editorOpen_) call(effEditIdle); }
    void closeEditor() { if (editorOpen_) { call(effEditClose); editorOpen_ = false; } }
    bool editorOpen() const { return editorOpen_; }

    // The plugin's whole state (its chunk, or its parameters), to keep and give back later.
    std::vector<uint8_t> saveState() {
        std::vector<uint8_t> out;
        if (fx_) {
            out.resize(1 + (size_t)fx_->count() * sizeof(float));
            out[0] = 'X';
            for (int i = 0; i < fx_->count(); ++i) { const float v = fx_->value(i); memcpy(out.data() + 1 + i * sizeof(float), &v, sizeof v); }
            return out;
        }
        if (!effect_) return out;
        if (effect_->flags & flagProgramChunks) {
            void* data = nullptr;
            const intptr_t size = call(effGetChunk, 0, 0, &data);
            if (size > 0 && data) {
                out.resize(5 + (size_t)size);
                out[0] = 'C';
                memcpy(out.data() + 1, &size, 4);
                memcpy(out.data() + 5, data, (size_t)size);
            }
            return out;
        }
        const int n = effect_->numParams;
        out.resize(1 + (size_t)n * sizeof(float));
        out[0] = 'P';
        for (int i = 0; i < n; ++i) { const float v = effect_->getParameter(effect_, i); memcpy(out.data() + 1 + i * sizeof(float), &v, sizeof v); }
        return out;
    }
    void loadState(const std::vector<uint8_t>& state) {
        if (fx_ && !state.empty() && state[0] == 'X') {
            const int n = (int)((state.size() - 1) / sizeof(float));
            for (int i = 0; i < n && i < fx_->count(); ++i) { float v; memcpy(&v, state.data() + 1 + i * sizeof(float), sizeof v); fx_->setValue(i, v); }
            return;
        }
        if (!effect_ || state.empty()) return;
        if (state[0] == 'C' && state.size() > 5) {
            int32_t size = 0;
            memcpy(&size, state.data() + 1, 4);
            if (size > 0 && (size_t)size + 5 <= state.size())
                call(effSetChunk, 0, size, (void*)(state.data() + 5));
        } else if (state[0] == 'P') {
            const int n = (int)((state.size() - 1) / sizeof(float));
            for (int i = 0; i < n && i < effect_->numParams; ++i) {
                float v; memcpy(&v, state.data() + 1 + i * sizeof(float), sizeof v);
                effect_->setParameter(effect_, i, v);
            }
        }
    }

    // The window that shows this plugin's editor (set by the host's window code), resized when the plugin asks.
    HWND editorWindow = nullptr;

private:
    HMODULE module_ = nullptr;
    AEffect* effect_ = nullptr;
    std::unique_ptr<pfx::Effect> fx_;
    std::wstring why_, displayName_;
    int channels_ = 2;
    std::vector<std::vector<float>> in_, out_;
    std::vector<float*> inPtr_, outPtr_;
    bool editorOpen_ = false;
    std::atomic<bool> crashed_{false};
    int64_t samples_ = 0;

    // The calls into the plugin's own code, each guarded: a plugin that crashes is caught here, not in Pixl.
    static AEffect* start(EntryPoint entry) {
        __try {
            return entry(&Plugin::host);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    static bool run(AEffect* effect, float** in, float** out, int32_t frames) {
        __try {
            effect->processReplacing(effect, in, out, frames);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    TimeInfo time_{0, kSampleRate, 0, 0, 120.0, 0, 0, 0, 4, 4, 0, 0, 0,
                   timeTransportPlaying | timePpqValid | timeTempoValid | timeSigValid};

    bool fail(const wchar_t* reason) {
        why_ = name + L" " + reason;
        effect_ = nullptr;
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
        return false;
    }

    intptr_t call(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f) {
        return effect_ ? dispatch(effect_, opcode, index, value, ptr, opt) : 0;
    }
    static intptr_t dispatch(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
        __try {
            return effect->dispatcher(effect, opcode, index, value, ptr, opt);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // What the host answers a plugin's questions with.
    static intptr_t host(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float) {
        Plugin* self = effect ? (Plugin*)effect->user : nullptr;
        switch (opcode) {
        case hostVersion: return 2400;
        case hostCurrentId: return effect ? effect->uniqueID : 0;
        case hostGetSampleRate: return (intptr_t)kSampleRate;
        case hostGetBlockSize: return kMaxBlock;
        case hostGetTime: return self ? (intptr_t)&self->time_ : 0;
        case hostGetCurrentProcessLevel: return 0;
        case hostGetAutomationState: return 1;  // off
        case hostGetVendorString: if (ptr) strcpy_s((char*)ptr, 64, "9i55"); return 1;
        case hostGetProductString: if (ptr) strcpy_s((char*)ptr, 64, "Pixl"); return 1;
        case hostGetVendorVersion: return 1000;
        case hostGetLanguage: return 1;  // English
        case hostCanDo: {
            const char* q = (const char*)ptr;
            if (!q) return 0;
            return (!strcmp(q, "sizeWindow") || !strcmp(q, "supplyIdle") || !strcmp(q, "sendVstTimeInfo")) ? 1 : 0;
        }
        case hostSizeWindow:
            if (self && self->editorWindow) {
                RECT r{0, 0, index, (LONG)value};
                const DWORD style = (DWORD)GetWindowLongPtrW(self->editorWindow, GWL_STYLE);
                const DWORD ex = (DWORD)GetWindowLongPtrW(self->editorWindow, GWL_EXSTYLE);
                AdjustWindowRectEx(&r, style, FALSE, ex);
                SetWindowPos(self->editorWindow, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 1;
            }
            return 0;
        case hostUpdateDisplay: case hostIdle: case hostAutomate: case hostBeginEdit: case hostEndEdit: case hostIOChanged:
            return 1;
        default:
            return 0;
        }
    }
};

}  // namespace vst2
