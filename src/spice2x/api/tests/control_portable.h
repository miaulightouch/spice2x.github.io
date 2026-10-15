#pragma once
#include "portable.h"
#include <cstdint>
#include <cstring>

using std::to_string;
using DWORD = unsigned long;
using UINT = unsigned int;
using BOOL = int;
using HANDLE = void *;
constexpr BOOL FALSE = 0;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_NOT_ALL_ASSIGNED = 1300;
constexpr DWORD TOKEN_ADJUST_PRIVILEGES = 32, TOKEN_QUERY = 8, SE_PRIVILEGE_ENABLED = 2;
constexpr UINT EWX_POWEROFF = 8, EWX_REBOOT = 2, EWX_FORCE = 4;
#if !SPICE_XP
// Intentionally unavailable to XP test builds: any unguarded use must fail to compile.
constexpr UINT EWX_SHUTDOWN = 1, EWX_HYBRID_SHUTDOWN = 0x00400000;
#endif
constexpr DWORD SHTDN_REASON_MAJOR_APPLICATION = 0x40000, SHTDN_REASON_MINOR_MAINTENANCE = 1;
constexpr auto SE_SHUTDOWN_NAME = "SeShutdownPrivilege";
struct TOKEN_PRIVILEGES { DWORD PrivilegeCount; struct { int Luid; DWORD Attributes; } Privileges[1]; };
inline DWORD control_last_error = 0, exit_flags = 0, adjust_error = 0;
inline int open_calls = 0, close_calls = 0, adjust_calls = 0, exit_calls = 0, stop_calls = 0;
inline bool open_ok = true, lookup_ok = true, adjust_ok = true, exit_ok = true;
inline DWORD GetLastError() { return control_last_error; }
inline void SetLastError(DWORD value) { control_last_error = value; }
inline HANDLE GetCurrentProcess() { return nullptr; }
inline BOOL OpenProcessToken(HANDLE, DWORD, HANDLE *token) {
    ++open_calls; *token = reinterpret_cast<HANDLE>(1);
    if (!open_ok) SetLastError(ERROR_ACCESS_DENIED);
    return open_ok;
}
inline BOOL LookupPrivilegeValue(const char *, const char *, int *) {
    if (!lookup_ok) SetLastError(1313);
    return lookup_ok;
}
inline BOOL AdjustTokenPrivileges(HANDLE, BOOL, TOKEN_PRIVILEGES *, DWORD, void *, void *) {
    ++adjust_calls; SetLastError(adjust_error); return adjust_ok;
}
inline BOOL CloseHandle(HANDLE) { ++close_calls; SetLastError(999); return true; }
inline BOOL ExitWindowsEx(UINT flags, DWORD) {
    ++exit_calls; exit_flags = flags;
    if (!exit_ok) SetLastError(87);
    return exit_ok;
}
inline int _stricmp(const char *a, const char *b) { return std::strcmp(a, b); }
bool acquire_shutdown_privs();
namespace launcher {
    inline void shutdown(UINT = 0) { ++stop_calls; }
    inline void restart() { ++stop_calls; }
}
namespace control_test_crypto { inline void random_bytes(uint8_t *, size_t) {} }
inline std::string bin2hex(uint8_t *, size_t) { return "test-key"; }
