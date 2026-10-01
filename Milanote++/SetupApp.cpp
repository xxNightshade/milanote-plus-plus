// SetupApp.cpp - setup / sign-in window
#include "SetupApp.h"
#include "Installer.h"
#include "McpServer.h"
#include "ServerLock.h"
#include "Util.h"
#include "Resource.h"

using nlohmann::json;

namespace mn {

namespace {

constexpr const wchar_t* kSetupTitle = L"Milanote++";
constexpr const wchar_t* kMilanoteTitle = L"Milanote++";

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

} // namespace

SetupApp::SetupApp(HINSTANCE instance) : instance_(instance) {
    setupHtml_ = LoadTextResource(IDR_SETUP_HTML);
    overlayJs_ = LoadTextResource(IDR_OVERLAY_JS);
    blackoutJs_ = LoadTextResource(IDR_BLACKOUT_JS);
}

SetupApp::~SetupApp() {
    if (updateThread_.joinable()) updateThread_.detach();   // a download in flight must not block closing the window
}

int SetupApp::Run(bool startWithLogin) {
    if (setupHtml_.empty()) {
        MessageBoxW(nullptr, L"The setup page resource is missing from this build.", kSetupTitle, MB_ICONERROR);
        return 1;
    }

    WebPane::InitializeEnvironment([](HRESULT hr) {
        if (FAILED(hr)) {
            MessageBoxW(nullptr,
                L"Milanote++ needs the Microsoft Edge WebView2 Runtime, which does not seem to be installed.\n\n"
                L"Download it from https://developer.microsoft.com/microsoft-edge/webview2/ and start Milanote++ again.",
                kSetupTitle, MB_ICONERROR);
            PostQuitMessage(1);
        }
    });

    // hidden API host (bridge.js) - shares cookies with the visible window
    WebPane::Options apiOptions;
    apiOptions.visible = false;
    apiOptions.injectBridge = true;
    apiOptions.title = L"Milanote++ (API host)";
    api_ = std::make_unique<WebPane>(instance_, apiOptions);
    if (!api_->Create()) return 1;
    api_->Navigate(kBridgeHostUrl);

    // visible window
    WebPane::Options uiOptions;
    uiOptions.visible = true;
    uiOptions.injectBridge = false;
    uiOptions.title = kSetupTitle;
    uiOptions.width = 1040;
    uiOptions.height = 820;
    uiOptions.quitOnClose = true;
    ui_ = std::make_unique<WebPane>(instance_, uiOptions);
    ui_->onMessage = [this](const json& m) {
        std::string type = m.value("type", "");
        if (type == "claude") OnClaudeMessage(m);
        else if (type == "update") OnUpdateMessage(m);
        else OnUiMessage(m);
    };
    ui_->onAppMessage = [this](UINT msg, WPARAM, LPARAM lParam) {
        if (msg != WM_UPDATE_EVENT) return;
        std::unique_ptr<json> event(reinterpret_cast<json*>(lParam));
        if (!event || !ui_) return;
        std::string state = event->value("state", "");
        if (state == "installed") { updateInstalled_ = true; updateLatest_ = event->value("latest", ""); }
        if (state != "checking" && state != "downloading" && state != "verifying" && state != "installing") updateBusy_ = false;
        ui_->Post(*event);
    };
    ui_->onTimer = [this](UINT_PTR id) {
        if (id == kLoginPollTimer) PollLogin();
        else if (id == kClaudeWatchTimer) WatchClaude(false);
    };
    ui_->onAcceleratorKey = [this](UINT key) {
        if (key == VK_F2 && !onSetupPage_) { ShowSetupPage(); return true; }
        return false;
    };
    ui_->onNavigated = [this](const std::wstring& url) {
        // every page gets the blackout script; it asks for the current Claude Desktop state itself
        if (!blackoutJs_.empty()) ui_->ExecuteScript(Utf8ToWide(blackoutJs_));
        if (onSetupPage_ || !StartsWith(url, kMilanoteOrigin)) return;
        // the small "Milanote++" panel (bottom right) with the way back to the setup page
        if (!overlayJs_.empty()) ui_->ExecuteScript(Utf8ToWide(overlayJs_));
        if (!loginInProgress_) {
            // user is browsing Milanote inside the window; keep the session check going
            loginInProgress_ = true;
            SetTimer(ui_->Hwnd(), kLoginPollTimer, 3000, nullptr);
        }
    };
    if (!ui_->Create()) return 1;

    // Claude Desktop watch: blackout while it is not running, re-prompt when it goes away
    claude_ = FindClaudeDesktop();
    WatchClaude(true);
    SetTimer(ui_->Hwnd(), kClaudeWatchTimer, kClaudeWatchIntervalMs, nullptr);

    // self-update: clean up after a previous swap, then look for a newer release in the background
    CleanupAfterUpdate();
    StartUpdate(false);

    if (startWithLogin) ShowMilanote(kMilanoteLoginUrl); else ShowSetupPage();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int exitCode = static_cast<int>(msg.wParam);
    // api_ first: its pending callbacks may still post to ui_
    api_.reset();
    ui_.reset();
    return exitCode;
}

void SetupApp::ShowSetupPage() {
    onSetupPage_ = true;
    loginInProgress_ = false;
    if (ui_->Hwnd()) KillTimer(ui_->Hwnd(), kLoginPollTimer);
    ui_->SetTitle(kSetupTitle);
    ui_->NavigateToString(Utf8ToWide(setupHtml_));
    // the page asks for its status as soon as it loads
}

void SetupApp::ShowMilanote(const std::wstring& url) {
    onSetupPage_ = false;
    loginInProgress_ = true;
    ui_->SetTitle(kMilanoteTitle);
    ui_->Navigate(url);
    SetTimer(ui_->Hwnd(), kLoginPollTimer, 3000, nullptr);
}

void SetupApp::PollLogin() {
    if (onSetupPage_ || !api_) return;
    bool wasSignedIn = signedIn_;
    api_->Call("milanote_whoami", json{ {"fresh", true} }, [this, wasSignedIn](bool ok, const json&) {
        signedIn_ = ok;
        if (!ui_) return;
        // Freshly signed in while the login page was showing -> back to the setup page.
        if (ok && !wasSignedIn && !onSetupPage_ && StartsWith(ui_->CurrentUrl(), kMilanoteOrigin)) {
            ShowSetupPage();
        }
    }, 8000);
}

// ---------------------------------------------------------------- Claude Desktop watch / blackout
void SetupApp::WatchClaude(bool force) {
    bool running = IsClaudeDesktopRunning();
    bool changed = !claudeChecked_ || running != claudeRunning_;
    claudeChecked_ = true;
    claudeRunning_ = running;
    if (changed) {
        LogInfo(std::string("Claude Desktop is ") + (running ? "running" : "not running"));
        if (running) claudeError_.clear();
        else claude_ = FindClaudeDesktop();   // it may have been installed or removed meanwhile
    }
    if (changed || force) SendClaudeStatus();
}

void SetupApp::SendClaudeStatus() {
    if (!ui_) return;
    ui_->Post(json{
        {"type", "claude-status"},
        {"running", claudeRunning_},
        {"installed", claude_.installed},
        {"store", claude_.store},
        {"skipped", blackoutSkipped_},
        {"downloadUrl", WideToUtf8(kClaudeDownloadUrl)},
        {"error", claudeError_},
    });
}

void SetupApp::OnClaudeMessage(const json& m) {
    std::string action = m.value("action", "");
    if (action == "status") {
        WatchClaude(true);
    } else if (action == "open") {
        claudeError_.clear();
        std::string error;
        if (!LaunchClaudeDesktop(claude_, &error)) claudeError_ = error;
        else LogInfo("Claude Desktop launched");
        SendClaudeStatus();
    } else if (action == "download") {
        if (!OpenClaudeDownloadPage()) claudeError_ = "The browser could not be opened - go to " + WideToUtf8(kClaudeDownloadUrl);
        SendClaudeStatus();
    } else if (action == "recheck") {
        claudeError_.clear();
        claude_ = FindClaudeDesktop();
        WatchClaude(true);
    } else if (action == "skip") {
        blackoutSkipped_ = true;
        LogInfo("blackout dismissed for this session (Claude Code user)");
        SendClaudeStatus();
    }
}

// ---------------------------------------------------------------- self-update
void SetupApp::PostUpdateEvent(json event) {
    if (!ui_ || !ui_->Hwnd()) return;
    event["type"] = "update";
    event["current"] = kVersion;
    auto* heap = new json(std::move(event));
    if (!PostMessageW(ui_->Hwnd(), WM_UPDATE_EVENT, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
}

void SetupApp::StartUpdate(bool force) {
    if (updateBusy_ || !ui_ || !ui_->Hwnd()) return;
    updateBusy_ = true;
    if (updateThread_.joinable()) updateThread_.detach();
    // The worker only knows the window handle: once the window is gone, PostMessageW simply fails.
    HWND hwnd = ui_->Hwnd();
    updateThread_ = std::thread([force, hwnd]() {
        auto post = [hwnd](json e) {
            e["type"] = "update";
            e["current"] = kVersion;
            auto* heap = new json(std::move(e));
            if (!PostMessageW(hwnd, WM_UPDATE_EVENT, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
        };
        post(json{ {"state", "checking"} });
        bool applied = false;
        std::string error;
        UpdateInfo info = AutoUpdate(force, [&](const std::string& phase, int percent) {
            post(json{ {"state", phase}, {"percent", percent}, {"latest", ""} });
        }, &applied, &error);
        json e = { {"latest", info.latest}, {"releaseUrl", info.releaseUrl}, {"notes", info.notes}, {"rollback", RollbackAvailable()} };
        if (applied) {
            e["state"] = "installed";
        } else if (info.available) {              // a newer release exists but could not be installed
            e["state"] = "error";
            e["error"] = error.empty() ? info.error : error;
        } else if (info.checked) {                // GitHub answered: nothing newer (or no releases yet)
            e["state"] = "none";
            if (!info.error.empty()) e["note"] = info.error;
        } else {
            e["state"] = "error";
            e["error"] = error.empty() ? info.error : error;
        }
        post(e);
    });
}

void SetupApp::OnUpdateMessage(const json& m) {
    std::string action = m.value("action", "");
    if (action == "check") {
        StartUpdate(true);
    } else if (action == "status") {
        PostUpdateEvent(json{ {"state", updateInstalled_ ? "installed" : (updateBusy_ ? "checking" : "idle")}, {"latest", updateLatest_}, {"rollback", RollbackAvailable()} });
    } else if (action == "restart") {
        ShellExecuteW(nullptr, L"open", ExePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        PostQuitMessage(0);
    } else if (action == "rollback") {
        std::string error;
        if (RollbackUpdate(&error)) PostUpdateEvent(json{ {"state", "rolledback"}, {"rollback", false} });
        else PostUpdateEvent(json{ {"state", "error"}, {"error", error}, {"rollback", RollbackAvailable()} });
    } else if (action == "notes") {
        std::string url = m.value("url", "");
        if (!url.empty()) ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void SetupApp::SendStatus(bool fresh) {
    if (!api_) return;
    json args = json::object();
    if (fresh) args["fresh"] = true;
    api_->Call("milanote_whoami", args, [this](bool ok, const json& payload) {
        signedIn_ = ok;
        if (!ui_) return;
        json status = {
            {"type", "status"},
            {"signedIn", ok},
            {"userName", ok ? payload.value("name", "") : ""},
            {"authError", ok ? "" : (payload.value("code", "") == "auth" ? "Not signed in" : payload.value("error", "Milanote could not be reached"))},
            {"installed", IsInstalledInClaudeDesktop()},
            {"claudeDesktopFound", ClaudeDesktopDetected() || claude_.installed},
            {"claudeRunning", claudeRunning_},
            {"serverRunning", IsServerRunning()},
            {"claudeCodeCommand", ClaudeCodeCommand()},
            {"configPath", ClaudeDesktopConfigPathUtf8()},
            {"exePath", WideToUtf8(ExePath().wstring())},
            {"logPath", WideToUtf8((AppDataDir() / L"milanote++.log").wstring())},
            {"tools", McpServer::ToolDefinitions().size()},
            {"prompts", McpServer::PromptDefinitions().size()},
            {"templates", McpServer::TemplateSummaries().size()},
        };
        ui_->Post(status);
    }, 20000);
}

void SetupApp::OnUiMessage(const json& m) {
    if (m.value("type", "") != "setup" || !ui_ || !api_) return;
    std::string action = m.value("action", "");

    if (action == "status") {
        SendStatus(m.value("fresh", false));
    } else if (action == "back") {
        ShowSetupPage();
    } else if (action == "login") {
        ShowMilanote(kMilanoteLoginUrl);
    } else if (action == "open-milanote") {
        ShowMilanote(kMilanoteOrigin);
    } else if (action == "install") {
        InstallResult r = InstallClaudeDesktop();
        ui_->Post(json{ {"type", "message"}, {"target", "cdMsg"}, {"text", r.message}, {"level", r.ok ? "ok" : "bad"} });
        SendStatus(false);
    } else if (action == "uninstall") {
        InstallResult r = UninstallClaudeDesktop();
        ui_->Post(json{ {"type", "message"}, {"target", "cdMsg"}, {"text", r.message}, {"level", r.ok ? "ok" : "bad"} });
        SendStatus(false);
    } else if (action == "copy") {
        bool ok = CopyToClipboard(ui_->Hwnd(), Utf8ToWide(ClaudeCodeCommand()));
        ui_->Post(json{ {"type", "message"}, {"target", "copyMsg"}, {"text", ok ? "Copied - paste it into a terminal." : "Clipboard unavailable"}, {"level", ok ? "ok" : "bad"} });
    } else if (action == "test") {
        api_->Call("milanote_list_boards", json{ {"depth", 2} }, [this](bool ok, const json& payload) {
            if (!ui_) return;
            std::string text = ok ? ("OK - boards visible to Claude:\n" + payload.dump(2)) : ("Failed: " + payload.value("error", std::string("unknown error")));
            ui_->Post(json{ {"type", "test"}, {"ok", ok}, {"text", text} });
        }, 60000);
    }
}

} // namespace mn
