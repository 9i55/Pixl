#pragma once
// A row of effects one sound goes through in order: the voice's or the program's. The window's thread adds, removes,
// reorders and bypasses; the audio thread runs process() under a short lock, so a change never lands mid-block.
#include "vst2_host.hpp"
#include <memory>
#include <mutex>

namespace plugins {

struct Slot {
    std::unique_ptr<vst2::Plugin> plugin;
    bool bypass = false;
};

class Chain {
public:
    static const int kMaxSlots = 8;

    // Whether the chain is on at all (the panel's switch); off, the sound passes untouched.
    std::atomic<bool> on{true};

    // Runs the sound through every effect that is not bypassed. Audio thread.
    void process(float* l, float* r, int frames) {
        if (!on.load() || count_.load() == 0) return;
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return;  // the window is changing the chain this very moment: one block plain
        for (auto& s : slots_)
            if (!s.bypass && s.plugin && !s.plugin->crashed()) s.plugin->process(l, r, frames);
    }

    // Whether any effect would change the sound (on, with an effect that is not bypassed).
    bool live() const { return on.load() && live_.load(); }

    int size() const { return (int)slots_.size(); }
    vst2::Plugin* at(int i) { return i >= 0 && i < (int)slots_.size() ? slots_[i].plugin.get() : nullptr; }
    bool bypassed(int i) const { return i >= 0 && i < (int)slots_.size() && slots_[i].bypass; }

    // Adds an effect already loaded (loading takes a moment: done before, outside the lock).
    bool add(std::unique_ptr<vst2::Plugin> p, bool bypass = false) {
        if ((int)slots_.size() >= kMaxSlots || !p) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.push_back({std::move(p), bypass});
        changed();
        return true;
    }

    // Takes an effect out; the caller closes its window and lets it go.
    std::unique_ptr<vst2::Plugin> remove(int i) {
        if (i < 0 || i >= (int)slots_.size()) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        auto p = std::move(slots_[i].plugin);
        slots_.erase(slots_.begin() + i);
        changed();
        return p;
    }

    void move(int from, int to) {
        if (from < 0 || to < 0 || from >= (int)slots_.size() || to >= (int)slots_.size() || from == to) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Slot s = std::move(slots_[from]);
        slots_.erase(slots_.begin() + from);
        slots_.insert(slots_.begin() + to, std::move(s));
        changed();
    }

    void setBypass(int i, bool b) {
        if (i < 0 || i >= (int)slots_.size()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        slots_[i].bypass = b;
        changed();
    }

    // Every effect out (Pixl closing). Their windows must be closed first.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        slots_.clear();
        changed();
    }

private:
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::atomic<int> count_{0};
    std::atomic<bool> live_{false};

    void changed() {
        count_ = (int)slots_.size();
        bool any = false;
        for (auto& s : slots_) any |= !s.bypass && s.plugin != nullptr;
        live_ = any;
    }
};

}  // namespace plugins
