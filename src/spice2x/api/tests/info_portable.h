#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>

using DWORD = std::uint32_t;
using BOOL = int;
using HANDLE = void *;
using LONGLONG = long long;

constexpr BOOL FALSE = 0;
constexpr DWORD GENERIC_READ = 1;
constexpr DWORD FILE_SHARE_READ = 2;
constexpr DWORD FILE_SHARE_WRITE = 4;
constexpr DWORD OPEN_EXISTING = 3;
constexpr DWORD FILE_BEGIN = 0;
constexpr HANDLE INVALID_HANDLE_VALUE = nullptr;

template<typename T>
inline std::string to_string(T value) {
    std::ostringstream output;
    output << value;
    return output.str();
}

struct LARGE_INTEGER { long long QuadPart; };

struct info_test_file { std::FILE *file; };
inline int info_open_calls = 0;
inline int info_close_calls = 0;
inline int info_seek_calls = 0;
inline int info_read_calls = 0;
inline bool info_fail_size = false;
inline bool info_fail_seek = false;
inline bool info_fail_read = false;
inline std::size_t info_partial_read = static_cast<std::size_t>(-1);

inline HANDLE CreateFileA(const char *path, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) {
    ++info_open_calls;
    const auto file = std::fopen(path, "rb");
    if (!file) return INVALID_HANDLE_VALUE;
    return new info_test_file { file };
}

inline BOOL CloseHandle(HANDLE handle) {
    if (!handle) return FALSE;
    ++info_close_calls;
    const auto file = static_cast<info_test_file *>(handle);
    const auto result = std::fclose(file->file);
    delete file;
    return result == 0;
}

inline BOOL GetFileSizeEx(HANDLE handle, LARGE_INTEGER *size) {
    if (!handle || !size || info_fail_size) return FALSE;
    const auto file = static_cast<info_test_file *>(handle)->file;
    const auto current = std::ftell(file);
    if (current < 0 || std::fseek(file, 0, SEEK_END) != 0) return FALSE;
    size->QuadPart = std::ftell(file);
    if (size->QuadPart < 0 || std::fseek(file, current, SEEK_SET) != 0) return FALSE;
    return 1;
}

inline BOOL SetFilePointerEx(HANDLE handle, LARGE_INTEGER position, LARGE_INTEGER *, DWORD) {
    ++info_seek_calls;
    if (!handle || info_fail_seek) return FALSE;
    return std::fseek(static_cast<info_test_file *>(handle)->file, position.QuadPart, SEEK_SET) == 0;
}

inline BOOL ReadFile(HANDLE handle, void *buffer, DWORD size, DWORD *count, void *) {
    ++info_read_calls;
    if (!handle || !count || info_fail_read) return FALSE;
    const auto requested = std::min<std::size_t>(size, info_partial_read);
    *count = static_cast<DWORD>(std::fread(buffer, 1, requested,
            static_cast<info_test_file *>(handle)->file));
    if (std::ferror(static_cast<info_test_file *>(handle)->file)) return FALSE;
    return 1;
}

#define log_info(...) ((void)0)
#define log_warning(...) ((void)0)
#define log_misc(...) ((void)0)

inline std::string info_base64(const std::uint8_t *data, std::size_t size) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const auto left = size - i;
        const auto a = data[i];
        const auto b = left > 1 ? data[i + 1] : 0;
        const auto c = left > 2 ? data[i + 2] : 0;
        result.push_back(alphabet[a >> 2]);
        result.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        result.push_back(left > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        result.push_back(left > 2 ? alphabet[c & 63] : '=');
    }
    return result;
}

namespace crypt {
    inline std::string base64_encode(const std::uint8_t *data, std::size_t size) {
        return info_base64(data, size);
    }
}

namespace avs::game {
    inline char MODEL[4] = "TST";
    inline char DEST[2] = "X";
    inline char SPEC[2] = "S";
    inline char REV[2] = "R";
    inline char EXT[11] = "test-ext";
}

namespace avs::ea3 {
    inline std::string EA3_BOOT_URL = "test://boot";
}

namespace memutils {
    inline std::uint64_t mem_total() { return 1; }
    inline std::uint64_t mem_total_used() { return 1; }
    inline std::uint64_t mem_used() { return 1; }
    inline std::uint64_t vmem_total() { return 1; }
    inline std::uint64_t vmem_total_used() { return 1; }
    inline std::uint64_t vmem_used() { return 1; }
}

inline const char VERSION_STRING[] = "info-test";
inline const char VERSION_STRING_CFG[] = "info-test";
inline std::string LOG_FILE_PATH;
inline HANDLE LOG_FILE = INVALID_HANDLE_VALUE;
inline int LAUNCHER_ARGC = 0;
inline char **LAUNCHER_ARGV = nullptr;
