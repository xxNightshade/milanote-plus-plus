// ServerLock.h - "a Milanote++ MCP server is running" signal between processes.
//
// Every "Milanote++.exe --mcp" process (started by Claude Desktop or Claude Code) keeps a handle on a named
// event in the session namespace. A named kernel object lives exactly while at least one process holds a
// handle on it - also after a crash, because the kernel closes a dead process's handles - so the setup
// window can tell "Claude is connected" apart from "Claude Desktop is merely open".
//
// Milanote++ 1.2.0 used a FILE_FLAG_DELETE_ON_CLOSE file for this. That broke whenever two servers overlapped
// (Claude restarting the server, a second Claude client, a test run): as soon as one of them exited, the file
// was marked for deletion and could not be opened any more although the other server was still running, so
// the setup window said "restart Claude Desktop to connect" while Claude was connected. Handles on a kernel
// object are counted one by one, so a server that exits no longer hides the ones still running.
#pragma once

#include "framework.h"
#include "Util.h"

namespace mn {

// "Local\" = this Windows session only; the setup window and the servers Claude starts run in the same one.
constexpr const wchar_t* kServerSignalName = L"Local\\Milanote++.McpServer";

class ServerLock {
public:
    ServerLock() : handle_(CreateEventW(nullptr, TRUE, FALSE, kServerSignalName)) {}
    ~ServerLock() { if (handle_) CloseHandle(handle_); }
    ServerLock(const ServerLock&) = delete;
    ServerLock& operator=(const ServerLock&) = delete;
    bool Held() const { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

namespace detail {

// A 1.2.0 server (still running until Claude restarts it with the new exe) holds the old lock file instead.
inline bool LegacyServerLockOpen() {
    HANDLE h = CreateFileW((AppDataDir() / L"mcp-server.lock").c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

} // namespace detail

// True while some Milanote++ MCP server process is alive.
inline bool IsServerRunning() {
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, kServerSignalName);
    if (h) {
        CloseHandle(h);
        return true;
    }
    // "access denied" still proves the event exists (a server started by an elevated process)
    if (GetLastError() == ERROR_ACCESS_DENIED) return true;
    return detail::LegacyServerLockOpen();
}

} // namespace mn
