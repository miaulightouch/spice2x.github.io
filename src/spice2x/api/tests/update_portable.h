#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

template<class... Args> void log_info(Args&&...) {}
template<class... Args> void log_warning(Args&&...) {}

using DWORD = unsigned long;
constexpr DWORD CRYPT_STRING_BASE64 = 1;
inline DWORD GetModuleFileNameW(void *, wchar_t *path, size_t length) {
    constexpr wchar_t fixture[] = L"C:/game/spice.exe";
    if (length < std::size(fixture)) return length;
    std::copy(std::begin(fixture), std::end(fixture), path);
    return std::size(fixture) - 1;
}
inline bool CryptStringToBinaryA(const char *input, DWORD length, DWORD, uint8_t *bytes,
        DWORD *size, void *, void *) {
    if (std::string(input, length) != "TVo=" || *size < 2) return false;
    bytes[0] = 'M';
    bytes[1] = 'Z';
    *size = 2;
    return true;
}
constexpr const char *VERSION_STRING = "test-build";

namespace ota {
    constexpr uint64_t max_file_bytes = 64 * 1024 * 1024;
    constexpr uint64_t max_total_bytes = 128 * 1024 * 1024;
    constexpr size_t chunk_bytes = 32768;
    struct Signature { std::string status, signer, detail; };
    struct UploadFile { std::string name; uint64_t size; uint32_t crc32; };
    struct UploadedFile { std::string name; uint64_t size; Signature signature; };
    inline int begins = 0, chunks = 0, cancels = 0, applies = 0;
    inline bool fail = false;
    inline std::string active;
    inline uint32_t executable_crc32() { return 1234; }

    inline std::string begin(const std::vector<UploadFile> &files) {
        ++begins;
        if (fail || !active.empty() || files[0].name != "spice.exe" || files[0].size != 2) {
            throw std::runtime_error("Upload unavailable.");
        }
        active = "session-id";
        return active;
    }
    inline uint64_t chunk(const std::string &, const std::string &name, uint64_t offset,
            const std::vector<uint8_t> &bytes) {
        ++chunks;
        if (name != "spice.exe" || offset != 0 || bytes != std::vector<uint8_t>{'M', 'Z'}) {
            throw std::runtime_error("Invalid chunk.");
        }
        return bytes.size();
    }
    inline std::vector<UploadedFile> finish(const std::string &) {
        if (fail) throw std::runtime_error("Incomplete upload.");
        return {{"spice.exe", 2, {"invalid", "test signer", "Modified signature."}}};
    }
    inline void cancel(const std::string &) {
        ++cancels;
        if (fail) throw std::runtime_error("Staging cleanup failed.");
        active.clear();
    }
    inline void apply(const std::string &) {
        if (fail) throw std::runtime_error("Helper failed.");
        ++applies;
        active.clear();
    }
}
