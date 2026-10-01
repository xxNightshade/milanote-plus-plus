// SetupApp.h - the window people see when they double-click Milanote++.exe:
// sign in to Milanote, connect Claude Desktop / Claude Code, test the connection.
// It also watches Claude Desktop and blacks the window out while it is not running, and keeps the
// "connected" line current while Claude starts and stops the MCP server.
#pragma once

#include "framework.h"
#include <memory>
#include <string>
#include <thread>
#include "nlohmann/json.hpp"
#include "WebPane.h"
#include "ClaudeDesktop.h"
#include "Updater.h"

namespace mn {

class SetupApp {
public:
    explicit SetupApp(HINSTANCE instance);
    ~SetupApp();

    // startWithLogin: open Milanote's login page immediately (used by "--login" and the milanote_login tool)
    int Run(bool startWithLogin);

private:
    static constexpr UINT_PTR kLoginPollTimer = 7;
    static constexpr UINT_PTR kClaudeWatchTimer = 8;
    static constexpr UINT kClaudeWatchIntervalMs = 2000;

    void ShowSetupPage();
    void ShowMilanote(const std::wstring& url);
    void SendStatus(bool fresh);
    void OnUiMessage(const nlohmann::json& message);
    void PollLogin();

    // Claude Desktop watch + blackout
    void WatchClaude(bool force);
    void SendClaudeStatus();
    void OnClaudeMessage(const nlohmann::json& message);

    // self-update (worker thread; results are posted to the window as WM_UPDATE_EVENT)
    static constexpr UINT WM_UPDATE_EVENT = WM_APP + 20;   // lParam = nlohmann::json* (owned)
    void StartUpdate(bool force);
    void OnUpdateMessage(const nlohmann::json& message);
    void PostUpdateEvent(nlohmann::json event);

    HINSTANCE instance_;
    std::unique_ptr<WebPane> ui_;    // visible window
    std::unique_ptr<WebPane> api_;   // hidden bridge host (shares the browser profile with ui_)
    bool onSetupPage_ = false;
    bool signedIn_ = false;
    bool loginInProgress_ = false;
    std::string setupHtml_;
    std::string overlayJs_;
    std::string blackoutJs_;

    ClaudeDesktopInstall claude_;
    bool claudeRunning_ = false;
    bool serverRunning_ = false;     // a Milanote++ MCP server is alive (ServerLock.h), as of the last watch tick
    bool claudeChecked_ = false;
    bool blackoutSkipped_ = false;   // "I use Claude Code" - until the window is closed
    std::string claudeError_;

    std::thread updateThread_;
    bool updateBusy_ = false;
    bool updateInstalled_ = false;   // a new version is in place and waits for a restart
    std::string updateLatest_;
};

} // namespace mn
