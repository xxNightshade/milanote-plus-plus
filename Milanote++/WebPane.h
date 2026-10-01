// WebPane.h - a Win32 window hosting a WebView2 control, plus the JSON message plumbing that
// connects native code with the JavaScript bridge (bridge.js) running on app.milanote.com.
#pragma once

#include "framework.h"
#include <wrl.h>
#include <WebView2.h>
#include <string>
#include <functional>
#include <map>
#include <deque>
#include <memory>
#include "nlohmann/json.hpp"

namespace mn {

class WebPane {
public:
    struct Options {
        bool visible = false;              // false: a hidden host window (MCP mode)
        bool injectBridge = true;          // run bridge.js on every app.milanote.com document
        std::wstring title = L"Milanote++";
        int width = 1000;
        int height = 760;
        bool quitOnClose = false;          // PostQuitMessage when the window is closed
    };

    // ok == false -> payload is {"error": "...", "code": "..."}
    using CallCallback = std::function<void(bool ok, const nlohmann::json& payload)>;

    // Creates the shared WebView2 environment (user data folder = %LOCALAPPDATA%\Milanote++\WebView2).
    // Must be called once from the UI thread before any pane is created. onReady(hr) reports failure.
    static void InitializeEnvironment(std::function<void(HRESULT)> onReady);

    explicit WebPane(HINSTANCE instance, Options options);
    ~WebPane();

    WebPane(const WebPane&) = delete;
    WebPane& operator=(const WebPane&) = delete;

    bool Create();                                   // creates the window and (asynchronously) the WebView
    void Navigate(const std::wstring& url);
    void NavigateToString(const std::wstring& html);
    void ExecuteScript(const std::wstring& script);
    void Show(int cmd);
    HWND Hwnd() const { return hwnd_; }
    bool WebViewReady() const { return webview_ != nullptr; }
    bool BridgeReady() const { return bridgeReady_; }
    std::wstring CurrentUrl() const { return currentUrl_; }

    // Calls a tool implemented in bridge.js. Queued until the bridge reports ready.
    void Call(const std::string& tool, const nlohmann::json& args, CallCallback callback, DWORD timeoutMs = 90000);
    // Posts an arbitrary JSON message to the page (window.chrome.webview 'message' event).
    void Post(const nlohmann::json& message);

    // Events (all delivered on the UI thread)
    std::function<void(const nlohmann::json&)> onMessage;         // messages from the page that are not call replies
    std::function<void(const std::wstring& url)> onNavigated;    // NavigationCompleted
    std::function<void()> onBridgeReady;
    std::function<void()> onClosed;
    // WM_APP+n messages posted to the pane window (used to hop from worker threads to the UI thread)
    std::function<void(UINT msg, WPARAM wParam, LPARAM lParam)> onAppMessage;
    // Key presses inside the WebView (virtual-key code); return true to swallow the key
    std::function<bool(UINT virtualKey)> onAcceleratorKey;
    // WM_TIMER for timers (ids >= 2) that callers set on Hwnd()
    std::function<void(UINT_PTR timerId)> onTimer;

    void SetTitle(const std::wstring& title);

private:
    struct PendingCall {
        std::string tool;
        nlohmann::json args;
        CallCallback callback;
        ULONGLONG deadline = 0;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void RegisterWindowClass(HINSTANCE instance);
    static void OnEnvironmentReady();

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void CreateController();
    void OnControllerCreated(ICoreWebView2Controller* controller);
    void OnWebMessage(const std::wstring& json);
    void ResizeToClient();
    void FlushQueuedCalls();
    void CheckTimeouts();
    void FailAllPending(const std::string& reason);

    HINSTANCE instance_;
    Options options_;
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    EventRegistrationToken navigationCompletedToken_{};
    EventRegistrationToken navigationStartingToken_{};
    EventRegistrationToken webMessageToken_{};
    EventRegistrationToken acceleratorToken_{};
    bool bridgeReady_ = false;
    bool controllerRequested_ = false;
    std::wstring currentUrl_;
    std::wstring pendingNavigation_;
    std::wstring pendingHtml_;
    std::string bridgeSource_;
    unsigned long long nextCallId_ = 1;
    std::map<std::string, PendingCall> inFlight_;     // callId -> call
    std::deque<PendingCall> queued_;                  // waiting for the bridge
};

} // namespace mn
