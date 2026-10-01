// Milanote++.cpp - entry point
//
//   Milanote++.exe              setup window: sign in to Milanote, connect Claude, test
//   Milanote++.exe --login      same, but opens Milanote's sign-in page right away
//   Milanote++.exe --mcp        MCP server over stdio (this is what Claude Desktop / Claude Code launch)
//   Milanote++.exe --install    register with Claude Desktop from the command line
//   Milanote++.exe --uninstall  remove the registration
//   Milanote++.exe --print-config   print the JSON snippet + the Claude Code command
//   Milanote++.exe --whoami     command-line connection test
//   Milanote++.exe --update     check GitHub Releases and install a newer version now
//   Milanote++.exe --rollback   put the previous version back
//   Milanote++.exe --version
//   Milanote++.exe --help
#include "framework.h"
#include "Milanote++.h"
#include "Util.h"
#include "McpServer.h"
#include "SetupApp.h"
#include "Installer.h"
#include "Updater.h"
#include "WebPane.h"
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

// Makes stdout/stderr usable when started from a console (GUI subsystem apps are detached by default).
bool AttachToParentConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);
    std::fputs("\n", stdout);
    return true;
}

void PrintOrShow(bool haveConsole, const std::string& text, bool error) {
    if (haveConsole) {
        std::fputs(text.c_str(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    } else {
        MessageBoxW(nullptr, mn::Utf8ToWide(text).c_str(), mn::kAppName, error ? MB_ICONERROR : MB_ICONINFORMATION);
    }
}

const char* kHelp =
    "Milanote++ - Milanote for Claude (MCP server)\n"
    "\n"
    "  Milanote++.exe               open the setup window (sign in, connect Claude, test)\n"
    "  Milanote++.exe --login       open Milanote's sign-in page\n"
    "  Milanote++.exe --mcp         run the MCP server on stdio (used by Claude Desktop / Claude Code)\n"
    "  Milanote++.exe --install     register this exe in Claude Desktop's config\n"
    "  Milanote++.exe --uninstall   remove it from Claude Desktop's config\n"
    "  Milanote++.exe --print-config\n"
    "  Milanote++.exe --whoami      test the Milanote connection from the command line\n"
    "  Milanote++.exe --update      check GitHub Releases and install a newer version now\n"
    "  Milanote++.exe --rollback    put the previous version back (after a bad update)\n"
    "  Milanote++.exe --version\n";

int RunWhoami(HINSTANCE instance, bool haveConsole) {
    mn::WebPane::Options options;
    options.visible = false;
    options.injectBridge = true;
    mn::WebPane pane(instance, options);
    int exitCode = 1;
    mn::WebPane::InitializeEnvironment([&](HRESULT hr) {
        if (FAILED(hr)) {
            PrintOrShow(haveConsole, "WebView2 Runtime is not installed: https://developer.microsoft.com/microsoft-edge/webview2/", true);
            PostQuitMessage(1);
        }
    });
    if (!pane.Create()) return 1;
    pane.Navigate(mn::kBridgeHostUrl);
    pane.Call("milanote_whoami", json::object(), [&](bool ok, const json& payload) {
        if (ok) {
            PrintOrShow(haveConsole, "Connected to Milanote:\n" + payload.dump(2), false);
            exitCode = 0;
        } else {
            PrintOrShow(haveConsole, "Not connected: " + payload.value("error", std::string("unknown error")), true);
        }
        PostQuitMessage(exitCode);
    }, 60000);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return exitCode;
}

} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);

    std::wstring mode = args.empty() ? L"" : args[0];
    int result = 0;

    if (mode == L"--mcp") {
        // stdout belongs to the MCP transport: log to stderr + file only
        mn::Log::Instance().EnableStderr(true);
        mn::McpServer server(hInstance);
        result = server.Run();
    } else if (mode == L"--login") {
        mn::SetupApp app(hInstance);
        result = app.Run(true);
    } else if (mode == L"--install" || mode == L"--uninstall" || mode == L"--print-config" || mode == L"--help" || mode == L"-h" || mode == L"/?" || mode == L"--whoami"
               || mode == L"--update" || mode == L"--rollback" || mode == L"--version") {
        bool haveConsole = AttachToParentConsole();
        if (haveConsole) mn::Log::Instance().EnableStderr(false);
        if (mode == L"--install") {
            mn::InstallResult r = mn::InstallClaudeDesktop();
            PrintOrShow(haveConsole, r.message + "\n\nFor Claude Code run:\n  " + mn::ClaudeCodeCommand(), !r.ok);
            result = r.ok ? 0 : 1;
        } else if (mode == L"--uninstall") {
            mn::InstallResult r = mn::UninstallClaudeDesktop();
            PrintOrShow(haveConsole, r.message, !r.ok);
            result = r.ok ? 0 : 1;
        } else if (mode == L"--print-config") {
            PrintOrShow(haveConsole, "Claude Desktop (" + mn::ClaudeDesktopConfigPathUtf8() + "):\n" + mn::ConfigSnippet() + "\n\nClaude Code:\n  " + mn::ClaudeCodeCommand(), false);
        } else if (mode == L"--whoami") {
            result = RunWhoami(hInstance, haveConsole);
        } else if (mode == L"--version") {
            PrintOrShow(haveConsole, std::string("Milanote++ ") + mn::kVersion + " (updates from https://github.com/" + mn::kUpdateRepo + "/releases)", false);
        } else if (mode == L"--update") {
            bool applied = false;
            std::string error;
            mn::UpdateInfo info = mn::AutoUpdate(true, [&](const std::string& phase, int percent) {
                if (haveConsole) { std::printf("\r%s %d%%   ", phase.c_str(), percent); std::fflush(stdout); }
            }, &applied, &error);
            if (haveConsole) std::fputs("\n", stdout);
            if (applied) PrintOrShow(haveConsole, "Updated to " + info.latest + ". It is used the next time Milanote++ starts (restart Claude Desktop to update its connection too).", false);
            else if (!error.empty() && info.available) { PrintOrShow(haveConsole, "Update to " + info.latest + " failed: " + error, true); result = 1; }
            else if (info.checked) PrintOrShow(haveConsole, "Milanote++ " + std::string(mn::kVersion) + " is up to date" + (info.error.empty() ? "" : " (" + info.error + ")"), false);
            else { PrintOrShow(haveConsole, "Update check failed: " + (error.empty() ? info.error : error), true); result = 1; }
        } else if (mode == L"--rollback") {
            std::string error;
            if (mn::RollbackUpdate(&error)) PrintOrShow(haveConsole, "The previous version is back in place; it is used the next time Milanote++ starts.", false);
            else { PrintOrShow(haveConsole, "Rollback failed: " + error, true); result = 1; }
        } else {
            PrintOrShow(haveConsole, kHelp, false);
        }
        if (haveConsole) FreeConsole();
    } else {
        mn::SetupApp app(hInstance);
        result = app.Run(false);
    }

    CoUninitialize();
    return result;
}
