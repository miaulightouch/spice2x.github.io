#include "internal.h"

#include <algorithm>
#include <atomic>
#include <windows.h>

#include "avs/game.h"
#include "cfg/configurator.h"
#include "util/fileutils.h"
#include "util/logging.h"
#include "util/utils.h"

namespace patcher {

    static std::recursive_mutex PATCH_MANAGER_STATE_MUTEX;
    static std::atomic_bool loader_reload_pending = false;
    static std::atomic_bool state_release_barrier = false;
    static thread_local size_t state_lock_depth = 0;
    static thread_local bool state_draining_pending = false;
    static thread_local bool loader_notification_active = false;
    static thread_local bool loader_callback_scope = false;

    struct FlagScope {
        bool &active;
        const bool previous;

        explicit FlagScope(bool &active) : active(active), previous(active) {
            active = true;
        }

        ~FlagScope() {
            active = previous;
        }
    };

#ifdef PATCHER_TEST_HOOKS
    void patcher_test_after_mutex_unlock();
#endif

    static void reload_pending_patches() {
        FlagScope notification_scope(loader_notification_active);
        reload_local_patches(true);
    }

    static void release_state_lock(std::unique_lock<std::recursive_mutex> &lock) {
        // The barrier and pending publication share one total order, so a callback that
        // sees the barrier cannot publish work after the final drain.
        state_release_barrier.store(true, std::memory_order_seq_cst);
        state_draining_pending = true;
        while (loader_reload_pending.exchange(false, std::memory_order_seq_cst)) {
            reload_pending_patches();
        }

        // Keep the barrier published while the mutex changes hands. Notifications that arrive
        // in this window publish pending work and return without trying to take the lock.
        // The test hook makes this handoff deterministic without affecting production builds.
        lock.unlock();
#ifdef PATCHER_TEST_HOOKS
        patcher_test_after_mutex_unlock();
#endif
        state_release_barrier.store(false, std::memory_order_seq_cst);

        // Once the barrier is down, a free-state callback either reloads immediately or leaves
        // pending work for the temporary owner below; in both cases this loop closes the handoff.
        while (loader_reload_pending.exchange(false, std::memory_order_seq_cst)) {
            StateLock reacquired;
            reload_pending_patches();
        }
        state_draining_pending = false;
    }

    StateLock::StateLock() : lock(PATCH_MANAGER_STATE_MUTEX) {
        outermost = state_lock_depth++ == 0;
    }

    StateLock::StateLock(std::try_to_lock_t)
        : lock(PATCH_MANAGER_STATE_MUTEX, std::try_to_lock) {
        if (lock.owns_lock()) {
            outermost = state_lock_depth++ == 0;
        }
    }

    StateLock::~StateLock() {
        if (!lock.owns_lock()) {
            return;
        }
        const bool drain = outermost && state_lock_depth == 1;
        --state_lock_depth;
        if (drain
            && !state_draining_pending
            && !loader_callback_scope) {
            release_state_lock(lock);
        }
    }

    bool StateLock::owns_lock() const {
        return lock.owns_lock();
    }

    std::optional<std::string> PATCH_MANAGER_CFG_PATH_OVERRIDE;

    std::filesystem::path config_path;
    bool config_dirty = false;
    bool setting_auto_apply = false;

    std::vector<std::string> setting_auto_apply_list;
    std::vector<std::string> setting_patches_enabled;

    std::map<std::string, std::string> setting_union_patches_enabled;
    std::map<std::string, int64_t> setting_int_patches_enabled;

    std::string patch_url;

    std::filesystem::path LOCAL_PATCHES_PATH("patches");
    std::string ACTIVE_JSON_FILE;

    std::vector<PatchData> patches;
    bool local_patches_initialized = false;
    std::vector<size_t> patches_sorted;
    std::map<std::pair<std::string, std::string>, PatchGroup> patch_groups;
    std::map<std::string, std::vector<std::string>> EXTRA_DLLS = {
        {"jubeat.dll", {"music_db.dll", "coin.dll"}},
        {"arkmdxp3.dll", {"gamemdx.dll"}},
        {"arkmdxp4.dll", {"gamemdx.dll"}},
        {"arkmdxbio2.dll", {"gamemdx.dll"}},
        {"arkndd.dll", {"gamendd.dll"}},
        {"arkkep.dll", {"game.dll"}},
        {"arkjc9.dll", {"gamejc9.dll"}},
        {"arkkdm.dll", {"gamekdm.dll"}},
        {"arkmmd.dll", {"gamemmd.dll"}},
        {"arkklp.dll", {"lpac.dll"}},
        {"arknck.dll", {"weac.dll"}},
        {"gdxg.dll", {"game.dll", "libshare-pj.dll", "boot.dll"}}
    };

    std::string url_fetch_errors;
    size_t url_recent_idx = -1;
    std::vector<std::string> url_recents;

    bool ldr_registered = false;
    void *ldr_notify_cookie = nullptr;
    std::vector<std::string> ldr_target_libraries;

    void init() {
        StateLock lock;
        if (PATCH_MANAGER_CFG_PATH_OVERRIDE.has_value()) {
            config_path = fileutils::expand_config_path(PATCH_MANAGER_CFG_PATH_OVERRIDE.value());
            log_info("patchmanager", "using custom config file path: {}", config_path);
        } else {
            config_path =
                fileutils::get_config_file_path("patchmanager", "spicetools_patch_manager.json");
        }

        // register for DLL load notifications so patches can be (re)applied as
        // the game's target libraries come into memory. registration happens
        // once and is left in place for the lifetime of the process.
        if (!ldr_registered) {
            ldr_target_libraries = getExtraDlls(avs::game::DLL_NAME);
            ldr_target_libraries.push_back(avs::game::DLL_NAME);

            const auto register_fn = reinterpret_cast<decltype(&LdrRegisterDllNotification)>(
                GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));

            if (register_fn && NT_SUCCESS(register_fn(
                    0, &loader_notification, nullptr, &ldr_notify_cookie))) {
                log_info("patchmanager", "registered for DLL load notifications");
            } else {
                log_warning("patchmanager", "failed to register for DLL load notifications");
            }

            ldr_registered = true;
        }

        if (!local_patches_initialized) {
            patch_url.clear();
            if (fileutils::file_exists(config_path)) {
                config_load();
            }
            if (cfg::CONFIGURATOR_STANDALONE) {
                reload_local_patches(true);
            }
        }
    }

    void apply_patches_on_start() {
        StateLock lock;
        const bool pending = loader_reload_pending.exchange(false, std::memory_order_seq_cst);
        if (!local_patches_initialized || pending) {
            reload_local_patches(true);
        }
    }

    bool process_pending_loader_reload() {
        StateLock lock;
        if (!loader_reload_pending.exchange(false, std::memory_order_seq_cst)) {
            return false;
        }
        reload_local_patches(true);
        return true;
    }

    VOID CALLBACK loader_notification(
        ULONG reason,
        PCLDR_DLL_NOTIFICATION_DATA data,
        PVOID) {
        FlagScope callback_scope(loader_callback_scope);

        if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
            const auto dll = strtolower(std::filesystem::path({
                data->Loaded.FullDllName->Buffer,
                data->Loaded.FullDllName->Length / sizeof(wchar_t)
            }).filename().string());

            if (std::ranges::find(ldr_target_libraries, dll) != ldr_target_libraries.end()) {
                // The outer reload covers every target library; coalesce re-entrant callbacks to
                // avoid recursively reloading while the loader lock is already held.
                if (loader_notification_active) {
                    return;
                }
                // Publish before trying the state lock. A contending outer scope either drains
                // this event or a free-state callback consumes it immediately.
                loader_reload_pending.store(true, std::memory_order_seq_cst);
                if (state_release_barrier.load(std::memory_order_seq_cst)) {
                    return;
                }
                if (state_lock_depth != 0) {
                    return;
                }
                StateLock lock(std::try_to_lock);
                if (!lock.owns_lock()) {
                    return;
                }
                if (state_release_barrier.load(std::memory_order_seq_cst)) {
                    return;
                }
                loader_reload_pending.exchange(false, std::memory_order_seq_cst);
                FlagScope notification_scope(loader_notification_active);
                reload_local_patches(true);
            }
        }
    }
}
