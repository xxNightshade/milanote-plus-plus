// WebPane.cpp - WebView2 hosting and native <-> bridge.js messaging
#include "WebPane.h"
#include "Util.h"
#include "Resource.h"
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace mn {

namespace {

constexpr const wchar_t* kWindowClass = L"MilanotePP.WebPane";
constexpr UINT_PTR kTimeoutTimerId = 1;

ComPtr<ICoreWebView2Environment> g_environment;
bool g_environmentRequested = false;
HRESULT g_environmentResult = E_PENDING;
std::function<void(HRESULT)> g_environmentCallback;
std::vector<WebPane*> g_panesWaitingForEnvironment;
bool g_classRegistered = false;

} // namespace

// ---------------------------------------------------------------- environment
void WebPane::InitializeEnvironment(std::function<void(HRESULT)> onReady) {
    g_environmentCallback = std::move(onReady);
    if (g_environment) {
        if (g_environmentCallback) g_environmentCallback(S_OK);
        return;
    }
    if (g_environmentRequested) return;
    g_environmentRequested = true;

    std::wstring userData = WebViewUserDataDir().wstring();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                g_environmentResult = result;
                if (SUCCEEDED(result) && environment) {
                    g_environment = environment;
                    LogInfo("WebView2 environment ready");
                    WebPane::OnEnvironmentReady();
                } else {
                    LogError("WebView2 environment creation failed, hr=0x" + [] (HRESULT h) { char b[16]; sprintf_s(b, "%08X", static_cast<unsigned>(h)); return std::string(b); }(result));
                }
                if (g_environmentCallback) g_environmentCallback(result);
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        g_environmentResult = hr;
        LogError("CreateCoreWebView2EnvironmentWithOptions failed (is the WebView2 Runtime installed?)");
        if (g_environmentCallback) g_environmentCallback(hr);
    }
}

void WebPane::OnEnvironmentReady() {
    auto waiting = std::move(g_panesWaitingForEnvironment);
    g_panesWaitingForEnvironment.clear();
    for (WebPane* pane : waiting) {
        if (pane && pane->hwnd_) pane->CreateController();
    }
}

// ---------------------------------------------------------------- window
void WebPane::RegisterWindowClass(HINSTANCE instance) {
    if (g_classRegistered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WebPane::WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MILANOTE));
    wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    g_classRegistered = true;
}

WebPane::WebPane(HINSTANCE instance, Options options)
    : instance_(instance), options_(std::move(options)) {
    if (options_.injectBridge) {
        bridgeSource_ = LoadTextResource(IDR_BRIDGE_JS);
        if (bridgeSource_.empty()) LogError("bridge.js resource is missing from the executable");
    }
}

WebPane::~WebPane() {
    FailAllPending("window closed");
    if (controller_) controller_->Close();
    controller_.Reset();
    webview_.Reset();
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool WebPane::Create() {
    RegisterWindowClass(instance_);
    DWORD style = WS_OVERLAPPEDWINDOW;
    hwnd_ = CreateWindowExW(0, kWindowClass, options_.title.c_str(), style,
                            CW_USEDEFAULT, CW_USEDEFAULT, options_.width, options_.height,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        LogError("CreateWindowEx failed");
        return false;
    }
    if (options_.visible) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
    SetTimer(hwnd_, kTimeoutTimerId, 1000, nullptr);
    if (g_environment) {
        CreateController();
    } else {
        g_panesWaitingForEnvironment.push_back(this);
        if (!g_environmentRequested) InitializeEnvironment(nullptr);
    }
    return true;
}

void WebPane::Show(int cmd) {
    if (hwnd_) ShowWindow(hwnd_, cmd);
    if (cmd != SW_HIDE && hwnd_) SetForegroundWindow(hwnd_);
}

void WebPane::SetTitle(const std::wstring& title) {
    if (hwnd_) SetWindowTextW(hwnd_, title.c_str());
}

LRESULT CALLBACK WebPane::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WebPane* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WebPane*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<WebPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WebPane::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ResizeToClient();
        return 0;
    case WM_TIMER:
        if (wParam == kTimeoutTimerId) CheckTimeouts();
        else if (onTimer) onTimer(wParam);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimeoutTimerId);
        FailAllPending("window closed");
        if (controller_) controller_->Close();
        controller_.Reset();
        webview_.Reset();
        hwnd_ = nullptr;
        if (onClosed) onClosed();
        if (options_.quitOnClose) PostQuitMessage(0);
        return 0;
    default:
        if (msg >= WM_APP && msg < 0xC000 && onAppMessage) {
            onAppMessage(msg, wParam, lParam);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WebPane::ResizeToClient() {
    if (!controller_ || !hwnd_) return;
    RECT bounds{};
    GetClientRect(hwnd_, &bounds);
    controller_->put_Bounds(bounds);
}

// ---------------------------------------------------------------- WebView2
void WebPane::CreateController() {
    if (controllerRequested_ || !g_environment || !hwnd_) return;
    controllerRequested_ = true;
    g_environment->CreateCoreWebView2Controller(
        hwnd_,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (FAILED(result) || !controller) {
                    LogError("WebView2 controller creation failed");
                    FailAllPending("WebView2 could not be created");
                    return S_OK;
                }
                OnControllerCreated(controller);
                return S_OK;
            }).Get());
}

void WebPane::OnControllerCreated(ICoreWebView2Controller* controller) {
    controller_ = controller;
    controller_->get_CoreWebView2(&webview_);
    if (!webview_) {
        LogError("get_CoreWebView2 failed");
        return;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(options_.visible ? TRUE : FALSE);
        settings->put_AreDevToolsEnabled(TRUE);
    }

    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                bridgeReady_ = false;     // the page (and the bridge living in it) is going away
                return S_OK;
            }).Get(), &navigationStartingToken_);

    webview_->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                LPWSTR source = nullptr;
                if (SUCCEEDED(sender->get_Source(&source)) && source) {
                    currentUrl_ = source;
                    CoTaskMemFree(source);
                }
                BOOL success = FALSE;
                args->get_IsSuccess(&success);
                if (!success) {
                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_WebErrorStatus(&status);
                    LogWarn("navigation failed, status " + std::to_string(static_cast<int>(status)) + " for " + WideToUtf8(currentUrl_));
                }
                if (success && options_.injectBridge && !bridgeSource_.empty() && StartsWith(currentUrl_, kMilanoteOrigin)) {
                    ExecuteScript(Utf8ToWide(bridgeSource_));
                }
                if (onNavigated) onNavigated(currentUrl_);
                return S_OK;
            }).Get(), &navigationCompletedToken_);

    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR text = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&text)) && text) {
                    std::wstring message(text);
                    CoTaskMemFree(text);
                    OnWebMessage(message);
                }
                return S_OK;
            }).Get(), &webMessageToken_);

    controller_->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                args->get_KeyEventKind(&kind);
                UINT key = 0;
                args->get_VirtualKey(&key);
                if ((kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN || kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                    && onAcceleratorKey && onAcceleratorKey(key)) {
                    args->put_Handled(TRUE);
                }
                return S_OK;
            }).Get(), &acceleratorToken_);

    ResizeToClient();
    controller_->put_IsVisible(TRUE);

    if (!pendingHtml_.empty()) {
        std::wstring html = std::move(pendingHtml_);
        pendingHtml_.clear();
        NavigateToString(html);
    } else if (!pendingNavigation_.empty()) {
        std::wstring url = std::move(pendingNavigation_);
        pendingNavigation_.clear();
        Navigate(url);
    }
}

void WebPane::Navigate(const std::wstring& url) {
    if (!webview_) {
        pendingNavigation_ = url;
        pendingHtml_.clear();
        return;
    }
    bridgeReady_ = false;
    webview_->Navigate(url.c_str());
}

void WebPane::NavigateToString(const std::wstring& html) {
    if (!webview_) {
        pendingHtml_ = html;
        pendingNavigation_.clear();
        return;
    }
    bridgeReady_ = false;
    webview_->NavigateToString(html.c_str());
}

void WebPane::ExecuteScript(const std::wstring& script) {
    if (!webview_) return;
    webview_->ExecuteScript(script.c_str(), nullptr);
}

void WebPane::Post(const json& message) {
    if (!webview_) return;
    webview_->PostWebMessageAsJson(Utf8ToWide(message.dump()).c_str());
}

// ---------------------------------------------------------------- bridge calls
void WebPane::Call(const std::string& tool, const json& args, CallCallback callback, DWORD timeoutMs) {
    PendingCall call;
    call.tool = tool;
    call.args = args.is_null() ? json::object() : args;
    call.callback = std::move(callback);
    call.deadline = GetTickCount64() + timeoutMs;
    queued_.push_back(std::move(call));
    if (bridgeReady_) FlushQueuedCalls();
}

void WebPane::FlushQueuedCalls() {
    while (bridgeReady_ && webview_ && !queued_.empty()) {
        PendingCall call = std::move(queued_.front());
        queued_.pop_front();
        std::string id = std::to_string(nextCallId_++);
        json message = { {"callId", id}, {"tool", call.tool}, {"args", call.args} };
        inFlight_[id] = std::move(call);
        Post(message);
    }
}

void WebPane::OnWebMessage(const std::wstring& text) {
    json message = json::parse(WideToUtf8(text), nullptr, false);
    if (message.is_discarded() || !message.is_object()) return;

    if (message.contains("callId")) {
        std::string id = message["callId"].is_string() ? message["callId"].get<std::string>() : message["callId"].dump();
        auto it = inFlight_.find(id);
        if (it == inFlight_.end()) return;
        PendingCall call = std::move(it->second);
        inFlight_.erase(it);
        if (message.contains("error")) {
            json err = { {"error", message["error"]}, {"code", message.value("code", "error")} };
            if (call.callback) call.callback(false, err);
        } else {
            if (call.callback) call.callback(true, message.contains("result") ? message["result"] : json());
        }
        return;
    }

    std::string type = message.value("type", "");
    if (type == "ready") {
        bridgeReady_ = true;
        LogInfo("bridge ready on " + message.value("origin", std::string("?")));
        FlushQueuedCalls();
        if (onBridgeReady) onBridgeReady();
        return;
    }
    if (type == "log") {
        Log::Instance().Write(message.value("level", "info").c_str(), "[bridge] " + message.value("message", ""));
        return;
    }
    if (onMessage) onMessage(message);
}

void WebPane::CheckTimeouts() {
    ULONGLONG now = GetTickCount64();
    std::vector<std::pair<std::string, PendingCall>> expired;
    for (auto it = inFlight_.begin(); it != inFlight_.end();) {
        if (it->second.deadline <= now) {
            expired.emplace_back(it->first, std::move(it->second));
            it = inFlight_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& e : expired) {
        if (e.second.callback) e.second.callback(false, json{ {"error", "Milanote did not answer in time (" + e.second.tool + ")"}, {"code", "timeout"} });
    }
    std::deque<PendingCall> keep;
    std::vector<PendingCall> failed;
    while (!queued_.empty()) {
        PendingCall c = std::move(queued_.front());
        queued_.pop_front();
        if (c.deadline <= now) failed.push_back(std::move(c)); else keep.push_back(std::move(c));
    }
    queued_ = std::move(keep);
    for (auto& c : failed) {
        std::string reason = g_environment
            ? "The Milanote bridge did not start (no network, or app.milanote.com could not be loaded)"
            : "WebView2 is not available - install the Microsoft Edge WebView2 Runtime";
        if (c.callback) c.callback(false, json{ {"error", reason}, {"code", "bridge"} });
    }
}

void WebPane::FailAllPending(const std::string& reason) {
    auto inFlight = std::move(inFlight_);
    inFlight_.clear();
    auto queued = std::move(queued_);
    queued_.clear();
    for (auto& kv : inFlight) if (kv.second.callback) kv.second.callback(false, json{ {"error", reason}, {"code", "bridge"} });
    for (auto& c : queued) if (c.callback) c.callback(false, json{ {"error", reason}, {"code", "bridge"} });
}

} // namespace mn
