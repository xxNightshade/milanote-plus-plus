// Updater.cpp - GitHub Releases self-update (WinHTTP for the network, BCrypt for SHA-256)
#include "Updater.h"
#include "Util.h"
#include "nlohmann/json.hpp"
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

using nlohmann::json;

namespace mn {

namespace {

constexpr const wchar_t* kUpdateMutexName = L"Local\\MilanotePP.Update";

struct HttpResponse {
    DWORD status = 0;
    std::string error;
};

// GET `url` and hand the body to `sink` chunk by chunk. Follows https redirects (GitHub asset downloads
// bounce to objects.githubusercontent.com). `progress(received, total)` is optional; total may be 0.
HttpResponse HttpGet(const std::wstring& url, const wchar_t* extraHeaders,
                     const std::function<bool(const char*, DWORD)>& sink,
                     const std::function<void(unsigned long long, unsigned long long)>& progress = {}) {
    HttpResponse out;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { out.error = "invalid URL"; return out; }

    std::wstring agent = L"Milanote++/" + Utf8ToWide(kVersion) + L" (Windows; +https://github.com/" + Utf8ToWide(kUpdateRepo) + L")";
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { out.error = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")"; return out; }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 120000);

    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = nullptr;
    if (!connection) {
        out.error = "WinHttpConnect failed (" + std::to_string(GetLastError()) + ")";
    } else {
        DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) out.error = "WinHttpOpenRequest failed (" + std::to_string(GetLastError()) + ")";
    }
    if (request) {
        if (!WinHttpSendRequest(request, extraHeaders ? extraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS, extraHeaders ? static_cast<DWORD>(-1) : 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(request, nullptr)) {
            DWORD err = GetLastError();
            out.error = (err == ERROR_WINHTTP_TIMEOUT) ? "the connection timed out"
                      : (err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_NAME_NOT_RESOLVED) ? "no internet connection"
                      : "request failed (" + std::to_string(err) + ")";
        } else {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            out.status = status;
            DWORD contentLength = 0;
            size = sizeof(contentLength);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &size, WINHTTP_NO_HEADER_INDEX)) contentLength = 0;
            unsigned long long received = 0;
            std::vector<char> buffer(64 * 1024);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) { out.error = "read failed (" + std::to_string(GetLastError()) + ")"; break; }
                if (available == 0) break;
                DWORD read = 0;
                DWORD want = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                if (!WinHttpReadData(request, buffer.data(), want, &read)) { out.error = "read failed (" + std::to_string(GetLastError()) + ")"; break; }
                if (read == 0) break;
                received += read;
                if (!sink(buffer.data(), read)) { out.error = "could not write the download"; break; }
                if (progress) progress(received, contentLength);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return out;
}

HttpResponse HttpGetString(const std::wstring& url, const wchar_t* headers, std::string* body) {
    body->clear();
    return HttpGet(url, headers, [&](const char* data, DWORD n) { body->append(data, n); return body->size() < 16u * 1024 * 1024; });
}

std::string Sha256Hex(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { if (error) *error = "cannot read " + WideToUtf8(file.wstring()); return ""; }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
        && BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buffer(1024 * 1024);
        bool ok = true;
        while (in.good()) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            std::streamsize n = in.gcount();
            if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(n), 0) != 0) { ok = false; break; }
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* digits = "0123456789abcdef";
            for (UCHAR b : digest) { hex.push_back(digits[b >> 4]); hex.push_back(digits[b & 15]); }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (hex.empty() && error) *error = "SHA-256 could not be computed";
    return hex;
}

// first 64-hex-digit token of a .sha256 file ("<hex>  Milanote++.exe", certutil output, or just the hex)
std::string ParseHashFile(const std::string& text) {
    std::string token;
    for (char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            token.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (token.size() == 64) return token;
        } else {
            token.clear();
        }
    }
    return "";
}

std::filesystem::path UpdatesDir() {
    auto dir = AppDataDir() / L"updates";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path StatePath() { return AppDataDir() / L"update-state.json"; }

json LoadState() {
    std::ifstream in(StatePath(), std::ios::binary);
    if (!in) return json::object();
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_object() ? j : json::object();
}

void SaveState(const json& state) {
    std::ofstream out(StatePath(), std::ios::binary | std::ios::trunc);
    if (out) out << state.dump(2);
}

std::string TrimNotes(std::string body) {
    if (body.size() > 1500) body = body.substr(0, 1500) + "…";
    return body;
}

// A server started before a swap keeps running from Milanote++.old.exe; such a process must not swap again.
bool RunningFromOldExe() {
    return _wcsicmp(ExePath().filename().c_str(), kUpdateOldExeName) == 0;
}

bool IsPeFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    char magic[2] = {};
    in.read(magic, 2);
    return in.gcount() == 2 && magic[0] == 'M' && magic[1] == 'Z';
}

} // namespace

// ---------------------------------------------------------------- versions
int CompareVersions(const std::string& a, const std::string& b) {
    auto split = [](std::string s, std::vector<long>& parts, std::string& suffix) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
        size_t dash = s.find_first_of("-+");
        if (dash != std::string::npos) { suffix = s.substr(dash + 1); s.erase(dash); }
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, '.')) {
            try { parts.push_back(std::stol(item)); } catch (...) { parts.push_back(0); }
        }
        while (parts.size() < 3) parts.push_back(0);
    };
    std::vector<long> pa, pb;
    std::string sa, sb;
    split(a, pa, sa);
    split(b, pb, sb);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;   // 1.2.0 is newer than 1.2.0-beta
    return sa.compare(sb) < 0 ? -1 : (sa == sb ? 0 : 1);
}

// ---------------------------------------------------------------- check
UpdateInfo CheckForUpdate() {
    UpdateInfo info;
    info.current = kVersion;
    info.configured = std::string(kUpdateRepo).find('/') != std::string::npos;
    if (!info.configured) { info.error = "update source not configured"; return info; }

    std::wstring url = L"https://api.github.com/repos/" + Utf8ToWide(kUpdateRepo) + L"/releases/latest";
    std::string body;
    HttpResponse r = HttpGetString(url, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", &body);
    if (!r.error.empty()) { info.error = "GitHub could not be reached: " + r.error; return info; }
    if (r.status == 404) { info.checked = true; info.error = "no releases published yet"; return info; }
    if (r.status == 403 || r.status == 429) { info.error = "GitHub rate limit reached - try again in an hour"; return info; }
    if (r.status != 200) { info.error = "GitHub answered HTTP " + std::to_string(r.status); return info; }

    json release = json::parse(body, nullptr, false);
    if (!release.is_object()) { info.error = "unexpected answer from GitHub"; return info; }
    std::string tag = release.value("tag_name", "");
    if (tag.empty()) { info.error = "release without a tag"; return info; }
    info.checked = true;
    info.latest = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    info.releaseUrl = release.value("html_url", "https://github.com/" + std::string(kUpdateRepo) + "/releases");
    if (release.contains("body") && release["body"].is_string()) info.notes = TrimNotes(release["body"].get<std::string>());
    if (release.contains("assets") && release["assets"].is_array()) {
        for (const auto& asset : release["assets"]) {
            std::string name = asset.value("name", "");
            if (name == kUpdateAssetName) info.assetUrl = asset.value("browser_download_url", "");
            else if (name == kUpdateHashAssetName) info.hashUrl = asset.value("browser_download_url", "");
        }
    }
    info.available = CompareVersions(info.current, info.latest) < 0;
    if (info.available && info.assetUrl.empty()) {
        info.available = false;
        info.error = "release " + info.latest + " has no " + kUpdateAssetName + " asset";
    }
    return info;
}

// ---------------------------------------------------------------- apply
bool ApplyUpdate(const UpdateInfo& info, const UpdateProgress& progress, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = why; LogWarn("update failed: " + why); return false; };
    auto report = [&](const std::string& phase, int percent) { if (progress) progress(phase, percent); };
    if (!info.available || info.assetUrl.empty()) return fail("no update to apply");
    if (RunningFromOldExe()) return fail("this process runs from the previous version's file; restart Milanote++ first");

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kUpdateMutexName);
    if (!mutex) return fail("could not create the update lock");
    if (WaitForSingleObject(mutex, 0) == WAIT_TIMEOUT) { CloseHandle(mutex); return fail("another Milanote++ process is updating right now"); }
    struct Release { HANDLE h; ~Release() { ReleaseMutex(h); CloseHandle(h); } } release{ mutex };
    if (LoadState().value("installed", "") == info.latest) {   // another process (setup window / server) just did it
        report("installed", 100);
        return true;
    }

    // 1. download
    report("downloading", 0);
    auto dir = UpdatesDir();
    auto target = dir / (L"Milanote++-" + Utf8ToWide(info.latest) + L".exe");
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return fail("cannot write to " + WideToUtf8(dir.wstring()));
        HttpResponse r = HttpGet(Utf8ToWide(info.assetUrl), L"Accept: application/octet-stream\r\n",
            [&](const char* data, DWORD n) { out.write(data, n); return out.good(); },
            [&](unsigned long long received, unsigned long long total) {
                report("downloading", total ? static_cast<int>(received * 100 / total) : 0);
            });
        if (!r.error.empty()) return fail("download failed: " + r.error);
        if (r.status != 200) return fail("download answered HTTP " + std::to_string(r.status));
    }
    std::error_code ec;
    auto size = std::filesystem::file_size(target, ec);
    if (ec || size < 100 * 1024) return fail("the downloaded file is too small to be Milanote++");
    if (!IsPeFile(target)) return fail("the downloaded file is not a Windows program");

    // 2. verify
    report("verifying", 100);
    if (info.hashUrl.empty()) return fail("release " + info.latest + " has no " + kUpdateHashAssetName + " asset, refusing to install an unverified file");
    std::string hashText;
    HttpResponse hr = HttpGetString(Utf8ToWide(info.hashUrl), L"Accept: application/octet-stream\r\n", &hashText);
    if (!hr.error.empty() || hr.status != 200) return fail("could not download the checksum");
    std::string expected = ParseHashFile(hashText);
    if (expected.empty()) return fail("the checksum file is not a SHA-256 digest");
    std::string hashError;
    std::string actual = Sha256Hex(target, &hashError);
    if (actual.empty()) return fail(hashError);
    if (actual != expected) {
        std::filesystem::remove(target, ec);
        return fail("checksum mismatch - the download was corrupted or tampered with, nothing was installed");
    }

    // 3. swap: the running exe can be renamed but not overwritten
    report("installing", 100);
    auto exe = ExePath();
    auto old = exe.parent_path() / kUpdateOldExeName;
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return fail("cannot replace " + WideToUtf8(exe.wstring()) + " (" + std::to_string(GetLastError()) + ") - is the folder writable?");
    }
    if (!CopyFileW(target.c_str(), exe.c_str(), FALSE)) {
        DWORD err = GetLastError();
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);   // put the old one back
        return fail("could not copy the new version into place (" + std::to_string(err) + ")");
    }
    std::filesystem::remove(target, ec);
    json state = LoadState();
    state["installed"] = info.latest;
    state["installedAt"] = static_cast<long long>(time(nullptr));
    SaveState(state);
    LogInfo("updated " + std::string(kVersion) + " -> " + info.latest + " at " + WideToUtf8(exe.wstring()) + " (takes effect on the next start)");
    report("installed", 100);
    return true;
}

bool RollbackAvailable() {
    std::error_code ec;
    return std::filesystem::exists(ExePath().parent_path() / kUpdateOldExeName, ec);
}

bool RollbackUpdate(std::string* error) {
    auto exe = ExePath();
    auto old = exe.parent_path() / kUpdateOldExeName;
    auto parked = exe.parent_path() / L"Milanote++.rolledback.exe";
    if (!RollbackAvailable()) { if (error) *error = "there is no previous version next to the exe"; return false; }
    DeleteFileW(parked.c_str());
    if (!MoveFileExW(exe.c_str(), parked.c_str(), MOVEFILE_REPLACE_EXISTING)) { if (error) *error = "cannot move the current exe aside"; return false; }
    if (!MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(parked.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (error) *error = "cannot restore the previous version";
        return false;
    }
    LogInfo("rolled back to the previous version (takes effect on the next start)");
    return true;
}

void CleanupAfterUpdate() {
    auto dir = ExePath().parent_path();
    std::error_code ec;
    json state = LoadState();
    long long now = static_cast<long long>(time(nullptr));
    // the previous version stays next to the exe for a while so a bad release can be rolled back
    auto old = dir / kUpdateOldExeName;
    if (std::filesystem::exists(old, ec)) {
        bool thisIsTheUpdate = state.value("installed", "") == std::string(kVersion);
        long long installedAt = state.value("installedAt", 0LL);
        if (thisIsTheUpdate && installedAt > 0 && now - installedAt > static_cast<long long>(kKeepOldExeSeconds)) {
            if (DeleteFileW(old.c_str())) LogInfo("removed the previous version " + WideToUtf8(old.wstring()));
        }
    }
    // a version that was rolled back is not needed any more
    auto parked = dir / L"Milanote++.rolledback.exe";
    if (std::filesystem::exists(parked, ec) && DeleteFileW(parked.c_str())) LogInfo("removed " + WideToUtf8(parked.wstring()));
}

// ---------------------------------------------------------------- hands-off path
UpdateInfo AutoUpdate(bool force, const UpdateProgress& progress, bool* applied, std::string* error) {
    if (applied) *applied = false;
    if (RunningFromOldExe()) {
        UpdateInfo skipped;
        skipped.current = kVersion;
        skipped.configured = true;
        skipped.error = "running from the previous version's file (a newer version is already installed - restart)";
        if (error) *error = skipped.error;
        return skipped;
    }
    json state = LoadState();
    long long now = static_cast<long long>(time(nullptr));
    long long last = state.value("lastCheck", 0LL);
    if (!force && last > 0 && now - last < static_cast<long long>(kAutoUpdateMinIntervalSeconds)) {
        UpdateInfo cached;
        cached.current = kVersion;
        cached.configured = true;
        cached.checked = true;
        cached.latest = state.value("latest", std::string(kVersion));
        cached.releaseUrl = state.value("releaseUrl", "");
        cached.available = CompareVersions(cached.current, cached.latest) < 0 && state.value("lastError", "").empty();
        if (!cached.available) return cached;    // nothing new since the last check; the next real check comes later
    }
    UpdateInfo info = CheckForUpdate();
    state["lastCheck"] = now;
    state["lastError"] = info.error;
    if (info.checked) { state["latest"] = info.latest; state["releaseUrl"] = info.releaseUrl; }
    SaveState(state);
    if (!info.error.empty() && !info.available) { if (error) *error = info.error; return info; }
    if (!info.available) return info;
    std::string applyError;
    if (ApplyUpdate(info, progress, &applyError)) { if (applied) *applied = true; }
    else if (error) *error = applyError;
    return info;
}

} // namespace mn
