// Util.h - small helpers shared by every part of Milanote++ (strings, paths, logging, resources)
#pragma once

#include "framework.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace mn {

constexpr const wchar_t* kAppName = L"Milanote++";
constexpr const char* kVersion = "1.2.1";
constexpr const wchar_t* kMilanoteOrigin = L"https://app.milanote.com";
constexpr const wchar_t* kMilanoteLoginUrl = L"https://app.milanote.com/login";
// Lightweight same-origin document used as the host page for the API bridge: it is served by
// Milanote's own API, so fetch()/WebSocket calls made from it carry the user's session cookies
// without loading the whole (heavy) web app.
constexpr const wchar_t* kBridgeHostUrl = L"https://app.milanote.com/api/ping";
constexpr const char* kMcpServerKey = "milanote";

// ---------------------------------------------------------------- strings
inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------- paths
inline std::filesystem::path ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n));
}

inline std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p) {
        out = p;
    }
    if (p) CoTaskMemFree(p);
    return out;
}

// %LOCALAPPDATA%\Milanote++  (WebView2 profile, logs)
inline std::filesystem::path AppDataDir() {
    auto dir = KnownFolder(FOLDERID_LocalAppData) / kAppName;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

inline std::filesystem::path WebViewUserDataDir() {
    auto dir = AppDataDir() / L"WebView2";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Every place a Claude Desktop config can live, most likely first:
//  * Microsoft Store / MSIX install: %LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\Claude\claude_desktop_config.json
//    (the packaged app never sees the plain %APPDATA% file)
//  * classic installer:               %APPDATA%\Claude\claude_desktop_config.json
inline std::vector<std::filesystem::path> ClaudeDesktopConfigPaths() {
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    auto packages = KnownFolder(FOLDERID_LocalAppData) / L"Packages";
    if (std::filesystem::is_directory(packages, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(packages, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::wstring name = entry.path().filename().wstring();
            if (StartsWith(name, L"Claude_")) {
                paths.push_back(entry.path() / L"LocalCache" / L"Roaming" / L"Claude" / L"claude_desktop_config.json");
            }
        }
    }
    paths.push_back(KnownFolder(FOLDERID_RoamingAppData) / L"Claude" / L"claude_desktop_config.json");
    return paths;
}

// The config file Claude Desktop is most likely using (first existing one, else the classic path).
inline std::filesystem::path ClaudeDesktopConfigPath() {
    std::error_code ec;
    auto paths = ClaudeDesktopConfigPaths();
    for (const auto& p : paths) if (std::filesystem::exists(p, ec)) return p;
    return paths.back();
}

// True when some Claude Desktop installation is present (its config folder exists).
inline bool ClaudeDesktopDetected() {
    std::error_code ec;
    for (const auto& p : ClaudeDesktopConfigPaths()) if (std::filesystem::is_directory(p.parent_path(), ec)) return true;
    return false;
}

// ---------------------------------------------------------------- logging (stderr + file; never stdout, which belongs to MCP)
class Log {
public:
    static Log& Instance() { static Log log; return log; }

    void EnableFile(bool enable) { std::lock_guard<std::mutex> lock(mutex_); fileEnabled_ = enable; }
    void EnableStderr(bool enable) { std::lock_guard<std::mutex> lock(mutex_); stderrEnabled_ = enable; }

    void Write(const char* level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
        std::string line = std::string(stamp) + " [" + level + "] " + message + "\n";
        if (stderrEnabled_) {
            std::fputs(line.c_str(), stderr);
            std::fflush(stderr);
        }
        if (fileEnabled_) {
            if (!file_.is_open()) {
                file_.open(AppDataDir() / L"milanote++.log", std::ios::app | std::ios::binary);
            }
            if (file_.is_open()) {
                file_ << line;
                file_.flush();
            }
        }
    }

private:
    std::mutex mutex_;
    std::ofstream file_;
    bool fileEnabled_ = true;
    bool stderrEnabled_ = true;
};

inline void LogInfo(const std::string& m) { Log::Instance().Write("info", m); }
inline void LogWarn(const std::string& m) { Log::Instance().Write("warn", m); }
inline void LogError(const std::string& m) { Log::Instance().Write("error", m); }

// ---------------------------------------------------------------- embedded resources (RCDATA)
inline std::string LoadTextResource(int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return std::string();
    HGLOBAL handle = LoadResource(module, res);
    if (!handle) return std::string();
    const char* data = static_cast<const char*>(LockResource(handle));
    DWORD size = SizeofResource(module, res);
    if (!data || !size) return std::string();
    std::string text(data, size);
    // strip a UTF-8 BOM if the file was saved with one
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

} // namespace mn
