#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using DWORD = std::uint32_t;
using BOOL = int;
using HANDLE = void *;

constexpr BOOL FALSE = 0;
constexpr HANDLE INVALID_HANDLE_VALUE = nullptr;
constexpr DWORD GENERIC_WRITE = 1;
constexpr DWORD FILE_SHARE_READ = 2;
constexpr DWORD OPEN_EXISTING = 3;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1;
constexpr DWORD MOVEFILE_WRITE_THROUGH = 2;

inline bool config_test_fail_move = false;
inline std::uint32_t config_test_last_error = 5;

inline DWORD GetLastError() {
    return config_test_last_error;
}

template<typename Char>
inline HANDLE CreateFileW(const Char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) {
    return reinterpret_cast<HANDLE>(1);
}

inline BOOL FlushFileBuffers(HANDLE) {
    return 1;
}

inline BOOL CloseHandle(HANDLE) {
    return 1;
}

template<typename Char>
inline BOOL MoveFileExW(const Char *from, const Char *to, DWORD) {
    if (config_test_fail_move) {
        return 0;
    }
    std::error_code error;
    std::filesystem::remove(to, error);
    std::filesystem::rename(from, to, error);
    if (error) {
        config_test_last_error = static_cast<DWORD>(error.value());
        return 0;
    }
    return 1;
}

inline const wchar_t * _wgetenv(const wchar_t *) {
    return nullptr;
}

template<typename... Args>
inline void config_test_log(const Args &...) {
}

#define log_info(...) config_test_log(__VA_ARGS__)
#define log_warning(...) config_test_log(__VA_ARGS__)
#define log_fatal(...) config_test_log(__VA_ARGS__)

namespace fileutils {
    inline std::optional<std::filesystem::path> CONFIG_DIRECTORY;

    inline std::filesystem::path expand_config_path(const std::filesystem::path &path) {
        return path;
    }

    inline bool dir_exists(const std::filesystem::path &path) {
        return std::filesystem::is_directory(path);
    }

    inline bool dir_create_recursive(const std::filesystem::path &path) {
        std::error_code error;
        return std::filesystem::create_directories(path, error) ||
            std::filesystem::is_directory(path);
    }
}

#include "cfg/config.h"
#include "launcher/options.h"

inline tinyxml2::XMLError config_test_save_file(
        tinyxml2::XMLDocument &document,
        const std::filesystem::path &path) {
    tinyxml2::XMLPrinter printer;
    document.Print(&printer);
    std::ofstream output(path, std::ios::binary);
    if (!output.is_open()) {
        return tinyxml2::XML_ERROR_FILE_COULD_NOT_BE_OPENED;
    }
    output << printer.CStr();
    return output.good() ? tinyxml2::XML_SUCCESS : tinyxml2::XML_ERROR_FILE_COULD_NOT_BE_OPENED;
}

extern std::unique_ptr<std::vector<Option>> LAUNCHER_OPTIONS;

namespace launcher {
    const std::vector<OptionDefinition> &get_option_definitions();
    std::vector<Option> merge_options(
        const std::vector<Option> &options,
        const std::vector<Option> &overrides);
}

namespace games {
    const std::vector<std::string> &get_games();
}

const std::string &eamuse_get_game();
