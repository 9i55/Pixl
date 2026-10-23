#pragma once
// Finds the VST2 effects on this PC without running any of them: each .dll / .vst3 file's export table is read from
// disk, and a file that exports VSTPluginMain (or main) is a VST2 plugin. Kilohearts' kHs files carry VST2 and VST3
// in one binary, so the ones installed as .vst3 are found too.
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
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

// Every VST2 effect under the folders (and their subfolders, a few levels down), one per name, sorted by name.
inline std::vector<Found> scan(const std::vector<std::wstring>& folders) {
    std::vector<Found> out;
    std::set<std::wstring> seen;
    auto lower = [](std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; };
    struct Walker {
        std::vector<Found>& out;
        std::set<std::wstring>& seen;
        decltype(lower)& low;
        void walk(const std::wstring& dir, const std::wstring& vendor, int depth) {
            if (depth > 5) return;
            WIN32_FIND_DATAW d;
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &d);
            if (h == INVALID_HANDLE_VALUE) return;
            do {
                const std::wstring n = d.cFileName;
                if (n == L"." || n == L"..") continue;
                const std::wstring full = dir + L"\\" + n;
                if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // A .vst3 bundle keeps its binary in Contents\x86_64-win.
                    walk(full, depth == 0 ? n : vendor, depth + 1);
                    continue;
                }
                const size_t dot = n.find_last_of(L'.');
                const std::wstring ext = dot == std::wstring::npos ? L"" : low(n.substr(dot));
                if (ext != L".dll" && ext != L".vst3") continue;
                const std::wstring name = n.substr(0, dot);
                if (seen.count(low(name)) || !exportsVst2(full)) continue;
                seen.insert(low(name));
                out.push_back({name, full, vendor});
            } while (FindNextFileW(h, &d));
            FindClose(h);
        }
    } walker{out, seen, lower};
    for (const auto& f : folders)
        if (!f.empty()) walker.walk(f, L"", 0);
    std::sort(out.begin(), out.end(), [&](const Found& a, const Found& b) { return lower(a.name) < lower(b.name); });
    return out;
}

}  // namespace plugins
