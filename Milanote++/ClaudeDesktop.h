// ClaudeDesktop.h - finds, watches and launches the Claude Desktop app. No UI in here.
#pragma once

#include "framework.h"
#include <string>

namespace mn {

constexpr const wchar_t* kClaudeDownloadUrl = L"https://claude.ai/download";

struct ClaudeDesktopInstall {
    bool installed = false;
    bool store = false;             // Microsoft Store / MSIX package
    std::wstring packageFamily;     // e.g. Claude_pzs8sxrjxfjjc (store install)
    std::wstring classicExe;        // %LOCALAPPDATA%\AnthropicClaude\claude.exe (classic installer)
};

// Looks for a Claude Desktop installation (store package first, then the classic installer).
ClaudeDesktopInstall FindClaudeDesktop();

// True while a claude.exe process that belongs to the desktop app is alive (the Claude Code CLI does not count).
bool IsClaudeDesktopRunning();

// Starts Claude Desktop. On failure returns false and, when given, a short reason for the user.
bool LaunchClaudeDesktop(const ClaudeDesktopInstall& install, std::string* error = nullptr);

// Opens the download page in the default browser.
bool OpenClaudeDownloadPage();

} // namespace mn
