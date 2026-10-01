// Installer.cpp - Claude Desktop config editing
//
// Claude Desktop can be installed two ways on Windows and each reads a different config file:
//   * Microsoft Store (MSIX) package  -> %LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\Claude\claude_desktop_config.json
//   * classic installer               -> %APPDATA%\Claude\claude_desktop_config.json
// The packaged app never looks at the plain %APPDATA% file, so we update every config whose
// folder exists (and create the classic one when nothing is found at all).
#include "Installer.h"
#include "Util.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace mn {

namespace {

std::string ExePathUtf8() { return WideToUtf8(ExePath().wstring()); }

bool ReadFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    // tolerate a UTF-8 BOM
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) out.erase(0, 3);
    return true;
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

json ServerEntry() {
    return json{ {"command", ExePathUtf8()}, {"args", json::array({"--mcp"})} };
}

// Config files to touch: every candidate whose folder exists; if none, the classic path.
std::vector<std::filesystem::path> TargetConfigs() {
    std::error_code ec;
    std::vector<std::filesystem::path> targets;
    auto candidates = ClaudeDesktopConfigPaths();
    for (const auto& p : candidates) if (std::filesystem::is_directory(p.parent_path(), ec)) targets.push_back(p);
    if (targets.empty()) targets.push_back(candidates.back());
    return targets;
}

// Adds or removes the entry in one file. Returns a one-line report; ok=false only for hard failures.
bool UpdateOneConfig(const std::filesystem::path& path, bool install, std::string& report) {
    std::string pathText = WideToUtf8(path.wstring());
    json config = json::object();
    std::string existing;
    bool hadFile = ReadFile(path, existing);
    if (hadFile && !existing.empty()) {
        config = json::parse(existing, nullptr, false);
        if (config.is_discarded() || !config.is_object()) {
            report = "could not parse " + pathText + " - left untouched";
            return false;
        }
    } else if (!install) {
        report = "no config at " + pathText;
        return true;
    }

    if (install) {
        if (!config.contains("mcpServers") || !config["mcpServers"].is_object()) config["mcpServers"] = json::object();
        config["mcpServers"][kMcpServerKey] = ServerEntry();
    } else {
        if (!config.contains("mcpServers") || !config["mcpServers"].is_object() || !config["mcpServers"].contains(kMcpServerKey)) {
            report = "not registered in " + pathText;
            return true;
        }
        config["mcpServers"].erase(kMcpServerKey);
    }

    if (hadFile) {
        std::error_code ec;
        std::filesystem::copy_file(path, path.wstring() + L".milanotepp.bak", std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!WriteFile(path, config.dump(2) + "\n")) {
        report = "could not write " + pathText;
        return false;
    }
    report = (install ? "registered in " : "removed from ") + pathText + (hadFile ? " (backup saved next to it)" : " (file created)");
    return true;
}

} // namespace

std::string ClaudeDesktopConfigPathUtf8() {
    std::string out;
    for (const auto& p : TargetConfigs()) {
        if (!out.empty()) out += "\n";
        out += WideToUtf8(p.wstring());
    }
    return out;
}

std::string ConfigSnippet() {
    json j = json{ {"mcpServers", json{ {kMcpServerKey, ServerEntry()} }} };
    return j.dump(2);
}

std::string ClaudeCodeCommand() {
    return "claude mcp add -s user " + std::string(kMcpServerKey) + " -- \"" + ExePathUtf8() + "\" --mcp";
}

bool IsInstalledInClaudeDesktop() {
    for (const auto& path : ClaudeDesktopConfigPaths()) {
        std::string text;
        if (!ReadFile(path, text)) continue;
        json config = json::parse(text, nullptr, false);
        if (config.is_discarded() || !config.is_object()) continue;
        if (!config.contains("mcpServers") || !config["mcpServers"].is_object()) continue;
        const json& servers = config["mcpServers"];
        if (!servers.contains(kMcpServerKey) || !servers[kMcpServerKey].is_object()) continue;
        std::string command = servers[kMcpServerKey].value("command", "");
        if (_stricmp(command.c_str(), ExePathUtf8().c_str()) == 0) return true;
    }
    return false;
}

InstallResult InstallClaudeDesktop() {
    InstallResult result;
    result.ok = true;
    std::string lines;
    for (const auto& path : TargetConfigs()) {
        std::string report;
        bool ok = UpdateOneConfig(path, true, report);
        if (!ok) result.ok = false;
        lines += (lines.empty() ? "" : "\n") + std::string(ok ? "- " : "- FAILED: ") + report;
    }
    result.message = "Milanote++ MCP server \"" + std::string(kMcpServerKey) + "\":\n" + lines +
        "\nRestart Claude Desktop (quit it from the tray icon, then open it again); the Milanote tools then appear in new chats.";
    if (!result.ok) result.message += "\nAdd this by hand where it failed:\n" + ConfigSnippet();
    return result;
}

InstallResult UninstallClaudeDesktop() {
    InstallResult result;
    result.ok = true;
    std::string lines;
    for (const auto& path : TargetConfigs()) {
        std::string report;
        bool ok = UpdateOneConfig(path, false, report);
        if (!ok) result.ok = false;
        lines += (lines.empty() ? "" : "\n") + std::string(ok ? "- " : "- FAILED: ") + report;
    }
    result.message = "Milanote++ MCP server \"" + std::string(kMcpServerKey) + "\":\n" + lines + "\nRestart Claude Desktop to apply.";
    return result;
}

} // namespace mn
