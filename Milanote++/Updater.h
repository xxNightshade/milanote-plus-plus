// Updater.h - self-update from GitHub Releases.
//
// The contract that never changes, whatever version is running:
//   * releases live at https://github.com/<kUpdateRepo>/releases, tagged "v<major>.<minor>.<patch>"
//   * every release carries two assets: "Milanote++.exe" and "Milanote++.exe.sha256" (the hex digest)
//   * the RUNNING version does the whole job: ask the GitHub API for the latest release, compare the tag
//     with kVersion, download the exe, verify the SHA-256, then swap files (the running exe is renamed
//     to Milanote++.old.exe and the new one copied into its place). The new version is used the next
//     time the exe starts - also by Claude Desktop when it starts the MCP server again.
// Both the setup window and the MCP server call AutoUpdate() in the background, so updating keeps
// working even for people who never open the setup window. No UI in here; callers get progress callbacks.
#pragma once

#include "framework.h"
#include <string>
#include <functional>

namespace mn {

constexpr const char* kUpdateRepo = "xxNightshade/milanote-plus-plus";
constexpr const char* kUpdateAssetName = "Milanote++.exe";
constexpr const char* kUpdateHashAssetName = "Milanote++.exe.sha256";
constexpr const wchar_t* kUpdateOldExeName = L"Milanote++.old.exe";
constexpr unsigned kKeepOldExeSeconds = 7 * 24 * 60 * 60;        // the previous version stays for a rollback this long
constexpr unsigned kAutoUpdateMinIntervalSeconds = 6 * 60 * 60;   // GitHub allows 60 anonymous API calls per hour

struct UpdateInfo {
    bool configured = false;    // kUpdateRepo is set
    bool checked = false;       // the API answered
    bool available = false;     // a newer version exists
    std::string current;        // kVersion
    std::string latest;         // tag without the leading 'v'
    std::string releaseUrl;     // html page of the release
    std::string assetUrl;       // browser_download_url of Milanote++.exe
    std::string hashUrl;        // browser_download_url of Milanote++.exe.sha256 (may be empty)
    std::string notes;          // release body (trimmed)
    std::string error;          // why the check failed
};

using UpdateProgress = std::function<void(const std::string& phase, int percent)>;

// Compares dotted versions ("1.2.0" < "1.10.1"); a "-suffix" counts as older than the plain version.
int CompareVersions(const std::string& a, const std::string& b);

// Asks the GitHub API (blocking, network). Run it on a worker thread.
UpdateInfo CheckForUpdate();

// Downloads, verifies and swaps the exe (blocking, network + file I/O). Returns false with a reason on failure.
bool ApplyUpdate(const UpdateInfo& info, const UpdateProgress& progress, std::string* error);

// Puts Milanote++.old.exe back in place of the current exe (after a bad release). Blocking, no network.
bool RollbackUpdate(std::string* error);

// True when a previous version is still lying next to the exe.
bool RollbackAvailable();

// Deletes the leftover old exe (best effort; it may still be running as an MCP server).
void CleanupAfterUpdate();

// The hands-off path used at start-up by the setup window and the MCP server: rate-limited check,
// then download + verify + swap when a newer release exists. Blocking; run on a worker thread.
// `force` ignores the rate limit (the "Check for updates" button).
UpdateInfo AutoUpdate(bool force, const UpdateProgress& progress, bool* applied, std::string* error);

} // namespace mn
