#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <map>
#include <string>
#include <vector>
#include <strings.h>

#include "external/rapidjson/document.h"
#include "patcher/patch_manager.h"

#define _stricmp strcasecmp

template<typename... Args>
inline void patches_test_log(const Args &...) {
}

#define log_info(...) patches_test_log(__VA_ARGS__)
#define log_misc(...) patches_test_log(__VA_ARGS__)
#define log_warning(...) patches_test_log(__VA_ARGS__)

inline std::string strtolower(const std::string &input) {
    std::string output = input;
    output.erase(0, output.find_first_not_of("\t\n\v\f\r "));
    output.erase(output.find_last_not_of("\t\n\v\f\r ") + 1);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return output;
}

namespace avs::game {
    inline std::string DLL_NAME = "target.dll";

    inline std::string get_identifier() {
        return "PATCH-TEST";
    }
}

inline std::filesystem::path MODULE_PATH = "modules";

namespace cfg {
    inline bool CONFIGURATOR_STANDALONE = true;
}

namespace patcher {
    inline std::recursive_mutex patches_test_mutex;
    inline bool patches_test_apply_failure = false;
    inline bool patches_test_force_status_error = false;
    inline bool patches_test_force_integer_status_error = false;
    inline bool patches_test_force_integer_unverified = false;
    inline int patches_test_fail_on_apply_call = 0;
    inline int patches_test_apply_calls = 0;
    inline bool patches_test_save_failure = false;
    inline bool patches_test_import_result = false;
    inline std::string patches_test_import_error;
    inline int patches_test_reload_count = 0;
    inline int patches_test_save_count = 0;
    inline int patches_test_import_count = 0;
    inline std::vector<std::string> patches_test_events;

    inline std::filesystem::path config_path;
    inline bool config_dirty = false;
    inline bool setting_auto_apply = false;
    inline std::string patch_url;
    inline std::string ACTIVE_JSON_FILE;
    inline std::vector<PatchData> patches;
    inline bool local_patches_initialized = true;
    inline std::vector<size_t> patches_sorted;
    inline std::string url_fetch_errors;
    inline size_t url_recent_idx = static_cast<size_t>(-1);
    inline std::vector<std::string> url_recents;
    inline std::map<std::pair<std::string, std::string>, PatchGroup> patch_groups;

    inline std::string get_game_identifier(const std::filesystem::path &, bool) {
        return "PE-TEST";
    }

    inline StateLock::StateLock() : lock(patches_test_mutex), outermost(true) {
    }

    inline StateLock::StateLock(std::try_to_lock_t)
        : lock(patches_test_mutex, std::try_to_lock), outermost(true) {
    }

    inline StateLock::~StateLock() {
    }

    inline bool StateLock::owns_lock() const {
        return lock.owns_lock();
    }

    inline void init() {
    }

    inline bool process_pending_loader_reload() {
        return false;
    }

    inline void reload_local_patches(bool) {
        patches_test_events.emplace_back("reload");
        ++patches_test_reload_count;
        local_patches_initialized = true;
    }

    inline void config_load() {
    }

    inline bool config_save() {
        patches_test_events.emplace_back("save");
        ++patches_test_save_count;
        if (patches_test_save_failure) {
            return false;
        }
        config_dirty = false;
        return true;
    }

    inline bool import_remote_patches_to_disk() {
        patches_test_events.emplace_back("import");
        ++patches_test_import_count;
        patches.clear();
        url_fetch_errors = patches_test_import_error;
        return patches_test_import_result;
    }

    inline PatchStatus is_patch_active(PatchData &patch) {
        if (patches_test_apply_failure || patches_test_force_status_error
                || (patches_test_force_integer_status_error
                    && patch.type == PatchType::Integer)) {
            patch.error_reason = "test apply failure";
            return PatchStatus::Error;
        }
        if (patches_test_force_integer_unverified && patch.type == PatchType::Integer) {
            patch.unverified = true;
        }
        return patch.enabled ? PatchStatus::Enabled : PatchStatus::Disabled;
    }

    inline bool apply_patch(PatchData &patch, bool active) {
        ++patches_test_apply_calls;
        if (patches_test_apply_failure
                || patches_test_fail_on_apply_call == patches_test_apply_calls) {
            return false;
        }
        patch.last_status = active ? PatchStatus::Enabled : PatchStatus::Disabled;
        return true;
    }
}
