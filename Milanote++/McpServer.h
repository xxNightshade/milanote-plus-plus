// McpServer.h - Model Context Protocol server over stdio (JSON-RPC 2.0, newline delimited).
//
// Claude Desktop / Claude Code launch "Milanote++.exe --mcp" and talk to it through stdin/stdout.
// Tool calls are forwarded to bridge.js running in a hidden WebView2 on app.milanote.com.
// The server also offers prompts (Claude Desktop's "+" menu, Claude Code slash commands), resources
// (a guide, the board tree, any board as Markdown) and the board templates - all compiled into the exe
// from prompts.json and skill.md, so nothing has to be configured.
#pragma once

#include "framework.h"
#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include "nlohmann/json.hpp"
#include "WebPane.h"

namespace mn {

class McpServer {
public:
    explicit McpServer(HINSTANCE instance);
    ~McpServer();

    // Runs until stdin closes (or the host window is destroyed). Returns the process exit code.
    int Run();

    // The catalogue offered to the model (also shown by the setup window).
    static nlohmann::json ToolDefinitions();
    static nlohmann::json PromptDefinitions();      // prompts/list shape
    static nlohmann::json TemplateSummaries();      // [{name, title, description, variables}]
    static const std::string& Instructions();       // skill.md (the "skill" Claude reads on connect)

private:
    static constexpr UINT WM_MCP_LINE = WM_APP + 1;   // lParam = std::string* (owned)
    static constexpr UINT WM_MCP_EOF = WM_APP + 2;

    void ReaderThread();
    void HandleLine(const std::string& line);
    void HandleMessage(const nlohmann::json& message);
    void HandleToolCall(const nlohmann::json& id, const nlohmann::json& params);
    void HandlePromptGet(const nlohmann::json& id, const nlohmann::json& params);
    void HandleResourceRead(const nlohmann::json& id, const nlohmann::json& params);
    void ForwardToBridge(const nlohmann::json& id, const std::string& tool, const nlohmann::json& args);
    void Respond(const nlohmann::json& id, const nlohmann::json& result);
    void RespondError(const nlohmann::json& id, int code, const std::string& message);
    void RespondToolText(const nlohmann::json& id, const std::string& text, bool isError);
    void WriteMessage(const nlohmann::json& message);

    HINSTANCE instance_;
    std::unique_ptr<WebPane> pane_;
    std::thread reader_;
    std::thread updater_;
    std::atomic<bool> stopping_{ false };
    bool initialized_ = false;
    std::string protocolVersion_ = "2025-06-18";
};

} // namespace mn
