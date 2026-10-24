#pragma once
// Finds the VST2 effects on this PC without running any of them: each .dll / .vst3 file's export table is read from
// disk, and a file that exports VSTPluginMain (or main) is a VST2 plugin. Kilohearts' kHs files carry VST2 and VST3
// in one binary, so the ones installed as .vst3 are found too.
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace plugins {

struct Found {
    std::wstring name;    // "kHs Reverb"
    std::wstring path;
    std::wstring vendor;  // the folder it came from ("Kilohearts"), for grouping
};

// True when the 64-bit DLL at path exports VSTPluginMain or main (read from the file, never loaded).
inline bool exportsVst2(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    auto readAt = [&](long pos, void* to, size_t n) { return fseek(f, pos, SEEK_SET) == 0 && fread(to, 1, n, f) == n; };
    bool found = false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (readAt(0, &dos, sizeof dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && readAt(dos.e_lfanew, &nt, sizeof nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        const long secPos = dos.e_lfanew + (long)offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        auto toFile = [&](DWORD rva) -> long {
            for (auto& s : sections)
                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + (std::max)(s.Misc.VirtualSize, s.SizeOfRawData))
                    return (long)(rva - s.VirtualAddress + s.PointerToRawData);
            return -1;
        };
        IMAGE_EXPORT_DIRECTORY ex{};
        if (dir.VirtualAddress && !sections.empty() && readAt(secPos, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER))) {
            const long exPos = toFile(dir.VirtualAddress);
            if (exPos >= 0 && readAt(exPos, &ex, sizeof ex) && ex.NumberOfNames < 100000) {
                std::vector<DWORD> names(ex.NumberOfNames);
                const long namesPos = toFile(ex.AddressOfNames);
                if (namesPos >= 0 && (names.empty() || readAt(namesPos, names.data(), names.size() * sizeof(DWORD)))) {
                    for (DWORD rva : names) {
                        char buf[16] = {};
                        const long at = toFile(rva);
                        if (at >= 0 && readAt(at, buf, sizeof buf - 1) && (!strcmp(buf, "VSTPluginMain") || !strcmp(buf, "main"))) {
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    fclose(f);
    return found;
}

// The usual places plugins install to, then the user's own folder.
inline std::vector<std::wstring> standardFolders() {
    std::vector<std::wstring> out;
    auto known = [](REFKNOWNFOLDERID id) {
        PWSTR p = nullptr;
        std::wstring s;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
        CoTaskMemFree(p);
        return s;
    };
    const std::wstring common = known(FOLDERID_ProgramFilesCommonX64), programs = known(FOLDERID_ProgramFilesX64);
    if (!common.empty()) {
        out.push_back(common + L"\\VST3");
        out.push_back(common + L"\\VST2");
        out.push_back(common + L"\\Steinberg\\VST2");
    }
    if (!programs.empty()) {
        out.push_back(programs + L"\\VSTPlugins");
        out.push_back(programs + L"\\Steinberg\\VSTPlugins");
    }
    return out;
}

// What a search already read: a file that has not changed since (same size, same time) is not opened again.
struct ScanCache {
    struct Seen { uint64_t size = 0, time = 0; bool vst2 = false; };
    std::map<std::wstring, Seen> files;  // by lower-case path

    static std::wstring where() {
        PWSTR p = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir = std::wstring(p) + L"\\Pixl";
        CoTaskMemFree(p);
        return dir;
    }
    void load() {
        const std::wstring dir = where();
        FILE* f = nullptr;
        if (dir.empty() || _wfopen_s(&f, (dir + L"\\plugin-scan.txt").c_str(), L"rt, ccs=UTF-8") != 0 || !f) return;
        wchar_t line[2048];
        while (fgetws(line, 2048, f)) {
            unsigned long long size = 0, time = 0;
            int vst2 = 0, used = 0;
            if (swscanf_s(line, L"%llu %llu %d %n", &size, &time, &vst2, &used) < 3 || used <= 0) continue;
            std::wstring path = line + used;
            while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r')) path.pop_back();
            if (!path.empty()) files[path] = Seen{size, time, vst2 != 0};
        }
        fclose(f);
    }
    void save() const {
        const std::wstring dir = where();
        if (dir.empty()) return;
        CreateDirectoryW(dir.c_str(), nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, (dir + L"\\plugin-scan.txt").c_str(), L"wt, ccs=UTF-8") != 0 || !f) return;
        for (const auto& e : files) fwprintf(f, L"%llu %llu %d %s\n", e.second.size, e.second.time, e.second.vst2 ? 1 : 0, e.first.c_str());
        fclose(f);
    }
};

// Every VST2 effect under the folders (and their subfolders, a few levels down), one per name, sorted by name.
// Never follows a folder link (they can loop), looks inside a .vst3 bundle only where its binary lives, and
// stops after so many entries or seconds, so a huge folder (a whole drive, a sample library) cannot hold it.
inline std::vector<Found> scan(const std::vector<std::wstring>& folders, ScanCache* cache = nullptr) {
    std::vector<Found> out;
    std::set<std::wstring> seen;
    auto lower = [](std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; };
    struct Walker {
        std::vector<Found>& out;
        std::set<std::wstring>& seen;
        decltype(lower)& low;
        ScanCache* cache;
        std::map<std::wstring, ScanCache::Seen> now;
        int entries = 0;
        ULONGLONG until = GetTickCount64() + 20000;
        bool spent() const { return entries > 60000 || GetTickCount64() > until; }
        void walk(const std::wstring& dir, const std::wstring& vendor, int depth) {
            if (depth > 5 || spent()) return;
            WIN32_FIND_DATAW d;
            HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                if (++entries > 60000) break;
                const std::wstring n = d.cFileName;
                if (n == L"." || n == L"..") continue;
                const std::wstring full = dir + L"\\" + n;
                const size_t dot = n.find_last_of(L'.');
                const std::wstring ext = dot == std::wstring::npos ? L"" : low(n.substr(dot));
                if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    // A .vst3 bundle keeps its binary in Contents\x86_64-win; the rest of it is pictures and presets.
                    if (ext == L".vst3") walk(full + L"\\Contents\\x86_64-win", depth == 0 ? n : vendor, depth + 1);
                    else walk(full, depth == 0 ? n : vendor, depth + 1);
                    continue;
                }
                if (ext != L".dll" && ext != L".vst3") continue;
                const std::wstring name = n.substr(0, dot);
                if (seen.count(low(name))) continue;
                const uint64_t size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
                const uint64_t time = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
                const std::wstring key = low(full);
                bool vst2 = false;
                auto known = cache ? cache->files.find(key) : std::map<std::wstring, ScanCache::Seen>::iterator();
                if (cache && known != cache->files.end() && known->second.size == size && known->second.time == time) vst2 = known->second.vst2;
                else vst2 = exportsVst2(full);
                now[key] = ScanCache::Seen{size, time, vst2};
                if (!vst2) continue;
                seen.insert(low(name));
                out.push_back({name, full, vendor});
            } while (!spent() && FindNextFileW(h, &d));
            FindClose(h);
        }
    } walker{out, seen, lower, cache, {}};
    for (const auto& f : folders)
        if (!f.empty()) walker.walk(f, L"", 0);
    if (cache) cache->files = std::move(walker.now);
    std::sort(out.begin(), out.end(), [&](const Found& a, const Found& b) { return lower(a.name) < lower(b.name); });
    return out;
}

// Searches on a thread of its own, so + Add opens at once on any PC; the last results stay until the next search.
class Scanner {
public:
    // A new search, unless one is running. done runs on the searching thread when it has finished.
    void start(std::vector<std::wstring> folders, std::function<void()> done) {
        auto s = state_;
        if (s->busy.exchange(true)) return;
        std::thread([s, folders = std::move(folders), done = std::move(done)] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ScanCache cache;
            cache.load();
            std::vector<Found> found = scan(folders, &cache);
            cache.save();
            {
                std::lock_guard<std::mutex> lk(s->mx);
                s->found = std::move(found);
                s->searched = true;
            }
            if (SUCCEEDED(com)) CoUninitialize();
            s->busy = false;
            if (done) done();
        }).detach();
    }
    std::vector<Found> found() const {
        std::lock_guard<std::mutex> lk(state_->mx);
        return state_->found;
    }
    bool busy() const { return state_->busy.load(); }
    bool searched() const {
        std::lock_guard<std::mutex> lk(state_->mx);
        return state_->searched;
    }

private:
    struct State {
        mutable std::mutex mx;
        std::vector<Found> found;
        bool searched = false;
        std::atomic<bool> busy{false};
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace plugins
