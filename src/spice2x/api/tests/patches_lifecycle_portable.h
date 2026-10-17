#pragma once

#include <atomic>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "patcher/patch_manager.h"

template<typename... Args>
inline void patches_lifecycle_test_log(const Args &...) {
}

#define log_info(...) patches_lifecycle_test_log(__VA_ARGS__)
#define log_misc(...) patches_lifecycle_test_log(__VA_ARGS__)
#define log_warning(...) patches_lifecycle_test_log(__VA_ARGS__)

using ULONG = unsigned long;
using PVOID = void *;
using VOID = void;
using NTSTATUS = long;
#define CALLBACK

struct UNICODE_STRING {
    unsigned short Length;
    wchar_t *Buffer;
};

struct LDR_DLL_LOADED_NOTIFICATION_DATA {
    UNICODE_STRING *FullDllName;
};

union LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
};

using PCLDR_DLL_NOTIFICATION_DATA = const LDR_DLL_NOTIFICATION_DATA *;
using PLDR_DLL_NOTIFICATION_FUNCTION = VOID (CALLBACK *)(
    ULONG, PCLDR_DLL_NOTIFICATION_DATA, PVOID);

constexpr ULONG LDR_DLL_NOTIFICATION_REASON_LOADED = 1;

inline NTSTATUS LdrRegisterDllNotification(
        ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID, PVOID *) {
    return -1;
}

inline void *GetModuleHandleW(const wchar_t *) {
    return nullptr;
}

inline void *GetProcAddress(void *, const char *) {
    return nullptr;
}

#define NT_SUCCESS(status) ((status) >= 0)

inline std::string strtolower(const std::string &value) {
    std::string result = value;
    for (auto &character : result) {
        if ('A' <= character && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

namespace avs::game {
    inline std::string DLL_NAME = "target.dll";
}

namespace cfg {
    inline bool CONFIGURATOR_STANDALONE = false;
}

namespace fileutils {
    inline std::filesystem::path expand_config_path(const std::string &path) {
        return path;
    }

    inline std::filesystem::path get_config_file_path(
            const std::string &, const std::string &) {
        return "patch-config.json";
    }

    inline bool file_exists(const std::filesystem::path &) {
        return false;
    }
}

namespace patcher {
    extern std::atomic_int patches_lifecycle_reload_count;
    extern std::vector<std::string> ldr_target_libraries;

#ifdef PATCHER_TEST_HOOKS
    void patcher_test_after_mutex_unlock();
#endif

    void config_load();
    void reload_local_patches(bool apply_patches);
    std::vector<std::string> getExtraDlls(const std::string &firstDll);
    void loader_notification(ULONG reason, PCLDR_DLL_NOTIFICATION_DATA data, PVOID context);
}
