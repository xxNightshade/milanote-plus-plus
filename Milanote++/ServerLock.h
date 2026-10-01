// ServerLock.h - "a Milanote++ MCP server is running" signal between processes.
//
// Every "Milanote++.exe --mcp" process (started by Claude Desktop or Claude Code) holds an open handle on
// %LOCALAPPDATA%\Milanote++\mcp-server.lock with FILE_FLAG_DELETE_ON_CLOSE. The file therefore exists exactly
// while at least one server is alive - also after a crash, because the kernel closes the handle - and the
// setup window can tell "Claude is connected" apart from "Claude Desktop is merely open".
#pragma once

#include "framework.h"
#include "Util.h"

namespace mn {

inline std::filesystem::path ServerLockPath() { return AppDataDir() / L"mcp-server.lock"; }

class ServerLock {
public:
    ServerLock() {
        handle_ = CreateFileW(ServerLockPath().c_str(), GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }
    ~ServerLock() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ServerLock(const ServerLock&) = delete;
    ServerLock& operator=(const ServerLock&) = delete;
    bool Held() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// True while some Milanote++ MCP server process is alive.
inline bool IsServerRunning() {
    HANDLE h = CreateFileW(ServerLockPath().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

} // namespace mn
