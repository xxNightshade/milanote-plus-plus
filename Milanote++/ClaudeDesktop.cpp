// ClaudeDesktop.cpp - detection and launching of the Claude Desktop app
#include "ClaudeDesktop.h"
#include "Util.h"
#include <tlhelp32.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <cwctype>

namespace mn {

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Package family names of installed Claude store packages (%LOCALAPPDATA%\Packages\Claude_<publisher id>).
std::vector<std::wstring> ClaudePackageFamilies() {
    std::vector<std::wstring> families;
    std::error_code ec;
    auto packages = KnownFolder(FOLDERID_LocalAppData) / L"Packages";
    if (!std::filesystem::is_directory(packages, ec)) return families;
    for (const auto& entry : std::filesystem::directory_iterator(packages, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::wstring name = entry.path().filename().wstring();
        if (StartsWith(name, L"Claude_")) families.push_back(name);
    }
    return families;
}

// Full package names registered for a family (empty when the package is not installed any more).
std::vector<std::wstring> PackageFullNames(const std::wstring& family) {
    std::vector<std::wstring> out;
    UINT32 count = 0, length = 0;
    LONG rc = GetPackagesByPackageFamily(family.c_str(), &count, nullptr, &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0) return out;
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buffer(length);
    rc = GetPackagesByPackageFamily(family.c_str(), &count, names.data(), &length, buffer.data());
    if (rc != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < count; ++i) if (names[i]) out.emplace_back(names[i]);
    return out;
}

// The <Application Id="..."> of the package, read from its manifest (empty if it cannot be read).
std::wstring StoreAppId(const std::wstring& fullName) {
    UINT32 length = 0;
    if (GetPackagePathByFullName(fullName.c_str(), &length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) return L"";
    std::vector<wchar_t> buffer(length);
    if (GetPackagePathByFullName(fullName.c_str(), &length, buffer.data()) != ERROR_SUCCESS) return L"";
    std::filesystem::path manifest = std::filesystem::path(buffer.data()) / L"AppxManifest.xml";
    std::ifstream in(manifest, std::ios::binary);
    if (!in) return L"";
    std::stringstream ss;
    ss << in.rdbuf();
    std::string xml = ss.str();
    size_t app = xml.find("<Application ");
    if (app == std::string::npos) return L"";
    size_t end = xml.find('>', app);
    size_t idAttr = xml.find(" Id=\"", app);
    if (idAttr == std::string::npos || (end != std::string::npos && idAttr > end)) return L"";
    size_t start = idAttr + 5;
    size_t quote = xml.find('"', start);
    if (quote == std::string::npos) return L"";
    return Utf8ToWide(xml.substr(start, quote - start));
}

bool ActivateStoreApp(const std::wstring& aumid) {
    Microsoft::WRL::ComPtr<IApplicationActivationManager> manager;
    HRESULT hr = CoCreateInstance(__uuidof(ApplicationActivationManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (SUCCEEDED(hr)) {
        DWORD pid = 0;
        hr = manager->ActivateApplication(aumid.c_str(), L"", AO_NONE, &pid);
        if (SUCCEEDED(hr)) return true;
        LogWarn("ActivateApplication failed for " + WideToUtf8(aumid) + " (hr=" + std::to_string(static_cast<long>(hr)) + ")");
    }
    // fallback: the shell can start any app by its AppUserModelId
    std::wstring target = L"shell:AppsFolder\\" + aumid;
    HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace

ClaudeDesktopInstall FindClaudeDesktop() {
    ClaudeDesktopInstall info;
    for (const auto& family : ClaudePackageFamilies()) {
        if (!PackageFullNames(family).empty()) {
            info.installed = true;
            info.store = true;
            info.packageFamily = family;
            return info;
        }
    }
    std::error_code ec;
    auto classic = KnownFolder(FOLDERID_LocalAppData) / L"AnthropicClaude" / L"claude.exe";
    if (std::filesystem::exists(classic, ec)) {
        info.installed = true;
        info.classicExe = classic.wstring();
        return info;
    }
    // a leftover package folder without a registered package: treat as "installed" so the user gets an
    // "open" button rather than a download link; launching will then report what went wrong
    auto families = ClaudePackageFamilies();
    if (!families.empty()) {
        info.installed = true;
        info.store = true;
        info.packageFamily = families.front();
    }
    return info;
}

bool IsClaudeDesktopRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool running = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !running; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"claude.exe") != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) { running = true; continue; }     // cannot inspect it: assume it is the app
        wchar_t path[MAX_PATH * 2];
        DWORD size = static_cast<DWORD>(std::size(path));
        bool desktop = true;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            std::wstring p = Lower(std::wstring(path, size));
            // the desktop app lives in the store package or under %LOCALAPPDATA%\AnthropicClaude;
            // the Claude Code CLI is also called claude.exe but lives elsewhere
            desktop = p.find(L"\\windowsapps\\claude_") != std::wstring::npos || p.find(L"\\anthropicclaude\\") != std::wstring::npos;
        }
        CloseHandle(process);
        if (desktop) running = true;
    }
    CloseHandle(snapshot);
    return running;
}

bool LaunchClaudeDesktop(const ClaudeDesktopInstall& install, std::string* error) {
    if (!install.installed) {
        if (error) *error = "Claude Desktop is not installed.";
        return false;
    }
    if (install.store) {
        std::wstring appId;
        auto names = PackageFullNames(install.packageFamily);
        if (!names.empty()) appId = StoreAppId(names.front());
        if (appId.empty()) appId = L"App";
        if (ActivateStoreApp(install.packageFamily + L"!" + appId)) return true;
        if (error) *error = "Claude Desktop could not be started automatically - open it from the Start menu.";
        return false;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", install.classicExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) return true;
    if (error) *error = "Claude Desktop could not be started automatically - open it from the Start menu.";
    return false;
}

bool OpenClaudeDownloadPage() {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", kClaudeDownloadUrl, nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace mn
