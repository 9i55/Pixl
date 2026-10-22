#pragma once
// Updates from the project's GitHub releases. A release carries Pixl.exe and
// Pixl.exe.sha256; the app checks the latest release's tag against its own version,
// downloads the new exe next to itself, checks its SHA-256, then swaps it in.

#include "../version.hpp"
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <algorithm>
#include <string>
#include <vector>
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")

namespace update {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;

    static Version parse(const std::string& text) {
        Version v;
        size_t i = 0;
        while (i < text.size() && (text[i] < '0' || text[i] > '9')) ++i;  // skips a leading "v"
        int part[3] = {0, 0, 0}, n = 0;
        bool digits = false;
        for (; i < text.size() && n < 3; ++i) {
            const char c = text[i];
            if (c >= '0' && c <= '9') { part[n] = part[n] * 10 + (c - '0'); digits = true; }
            else if (c == '.') ++n;
            else break;
        }
        v.major = part[0]; v.minor = part[1]; v.patch = part[2];
        v.valid = digits;
        return v;
    }
    bool newerThan(const Version& o) const {
        if (major != o.major) return major > o.major;
        if (minor != o.minor) return minor > o.minor;
        return patch > o.patch;
    }
    std::wstring text() const {
        return std::to_wstring(major) + L"." + std::to_wstring(minor) + L"." + std::to_wstring(patch);
    }
};

inline Version current() { return Version::parse(PIXL_VERSION_STR); }

// GET into memory (small answers only) or into a file. Follows GitHub's redirects.
inline bool fetch(const std::wstring& url, std::string* body, const std::wstring& file, ULONGLONG most) {
    HINTERNET net = InternetOpenW(L"Pixl-Updater/" PIXL_VERSION_STR, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return false;
    DWORD wait = 15000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &wait, sizeof wait);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &wait, sizeof wait);
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
    HINTERNET req = InternetOpenUrlW(net, url.c_str(), headers, (DWORD)-1,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI, 0);
    bool ok = false;
    if (req) {
        DWORD status = 0, len = sizeof status;
        HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
        if (status == 200) {
            HANDLE out = INVALID_HANDLE_VALUE;
            if (!file.empty()) out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.empty() || out != INVALID_HANDLE_VALUE) {
                std::vector<char> buf(64 * 1024);
                ULONGLONG total = 0;
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!InternetReadFile(req, buf.data(), (DWORD)buf.size(), &got)) { ok = false; break; }
                    if (got == 0) break;
                    total += got;
                    if (total > most) { ok = false; break; }
                    if (body) body->append(buf.data(), got);
                    DWORD put = 0;
                    if (out != INVALID_HANDLE_VALUE && (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got)) { ok = false; break; }
                }
                if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
                if (!ok && !file.empty()) DeleteFileW(file.c_str());
            }
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(net);
    return ok;
}

inline std::string jsonString(const std::string& json, const char* key) {
    const std::string tag = std::string("\"") + key + "\"";
    size_t at = json.find(tag);
    if (at == std::string::npos) return "";
    at = json.find(':', at + tag.size());
    if (at == std::string::npos) return "";
    at = json.find('"', at);
    if (at == std::string::npos) return "";
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

struct Latest {
    Version version;
    std::wstring tag;
};

// Asks GitHub for the newest release. Returns false when offline or nothing usable came back.
inline bool latest(Latest& out) {
    std::string body;
    const std::wstring api = L"https://api.github.com/repos/" PIXL_REPO L"/releases/latest";
    if (!fetch(api, &body, L"", 1 << 20)) return false;
    const std::string tag = jsonString(body, "tag_name");
    out.version = Version::parse(tag);
    out.tag.assign(tag.begin(), tag.end());
    return out.version.valid;
}

inline bool jsonTrue(const std::string& json, const char* key) {
    const std::string tag = std::string("\"") + key + "\"";
    size_t at = json.find(tag);
    if (at == std::string::npos) return false;
    at = json.find(':', at + tag.size());
    if (at == std::string::npos) return false;
    at = json.find_first_not_of(" \t\r\n", at + 1);
    return at != std::string::npos && json.compare(at, 4, "true") == 0;
}

inline std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)std::max(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

// control.json (a secret gist of the repo owner) lets the developer turn Pixl off for everyone, or require a
// minimum version (older copies must update before they can be used).
struct Control {
    bool disabled = false;
    Version minVersion;
    std::wstring message;
};

inline void readControl(const std::string& json, Control& out) {
    out.disabled = jsonTrue(json, "disabled");
    out.minVersion = Version::parse(jsonString(json, "minVersion"));
    out.message = wide(jsonString(json, "message"));
}

// GitHub keeps the raw file cached for five minutes; a query that changes every time makes
// it read the file fresh, so an edit to the gist is seen on the next check.
inline bool control(Control& out) {
    std::string body;
    const std::wstring url = std::wstring(PIXL_CONTROL_URL) + L"?t=" + std::to_wstring(GetTickCount64());
    if (!fetch(url, &body, L"", 64 * 1024)) return false;
    readControl(body, out);
    return true;
}

inline std::string sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<UCHAR> buf(64 * 1024);
        bool ok = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
            if (got == 0) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            static const char* hx = "0123456789abcdef";
            for (UCHAR b : digest) { hex.push_back(hx[b >> 4]); hex.push_back(hx[b & 15]); }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

// Downloads the release's Pixl.exe to `dest` and checks it against Pixl.exe.sha256.
inline bool download(const Latest& l, const std::wstring& dest, std::wstring& why) {
    const std::wstring base = L"https://github.com/" PIXL_REPO L"/releases/download/" + l.tag + L"/";
    std::string sum;
    if (!fetch(base + L"Pixl.exe.sha256", &sum, L"", 4096)) { why = L"Couldn't get the update from GitHub. Check your connection and try again."; return false; }
    std::string want;
    for (char c : sum) {
        if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) want.push_back(c);
        else if (!want.empty()) break;
    }
    if (want.size() != 64) { why = L"The update on GitHub has no valid checksum, so it wasn't installed."; return false; }
    if (!fetch(base + L"Pixl.exe", nullptr, dest, 200ull << 20)) { why = L"The download didn't finish. Try again."; return false; }
    if (sha256File(dest) != want) {
        DeleteFileW(dest.c_str());
        why = L"The downloaded file didn't match its checksum, so it wasn't installed.";
        return false;
    }
    return true;
}

}
