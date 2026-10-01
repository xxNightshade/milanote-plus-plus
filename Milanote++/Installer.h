// Installer.h - registers Milanote++ as an MCP server with Claude Desktop and prints the Claude Code command.
//
// Milanote++ never touches Claude credentials: the user signs in to Claude in the Claude app itself and
// that app launches "Milanote++.exe --mcp" as a local tool server. (Anthropic's policies do not allow
// third-party apps to log in with Claude.ai accounts, so an MCP server is the supported way to use a
// Claude subscription with another product.)
#pragma once

#include <string>

namespace mn {

struct InstallResult {
    bool ok = false;
    std::string message;        // human readable outcome
};

// Config file(s) Claude Desktop reads (one per line), as UTF-8.
std::string ClaudeDesktopConfigPathUtf8();

// Adds / updates mcpServers.milanote in every Claude Desktop config that exists (Store package and/or
// %APPDATA%\Claude\claude_desktop_config.json); a backup is written next to each file first.
InstallResult InstallClaudeDesktop();
// Removes the entry again.
InstallResult UninstallClaudeDesktop();
// True when the config already points at this executable.
bool IsInstalledInClaudeDesktop();

// The command a user runs to register the server with Claude Code (user scope).
std::string ClaudeCodeCommand();
// The JSON snippet for clients that are configured by hand.
std::string ConfigSnippet();

} // namespace mn
