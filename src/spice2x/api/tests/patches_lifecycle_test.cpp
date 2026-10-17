#include "patches_lifecycle_portable.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

namespace patcher {
    std::atomic_int patches_lifecycle_reload_count = 0;
    std::atomic_bool patches_lifecycle_hook_enabled = false;
    std::atomic_bool patches_lifecycle_hook_entered = false;
    std::atomic_bool patches_lifecycle_hook_continue = false;
    std::atomic_int patches_lifecycle_unlock_count = 0;
    std::function<void()> patches_lifecycle_on_reload;

    void patcher_test_after_mutex_unlock() {
        ++patches_lifecycle_unlock_count;
        if (!patches_lifecycle_hook_enabled.load(std::memory_order_acquire)) {
            return;
        }
        patches_lifecycle_hook_entered.store(true, std::memory_order_release);
        while (!patches_lifecycle_hook_continue.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void config_load() {
    }

    void reload_local_patches(bool apply_patches) {
        assert(apply_patches);
        ++patches_lifecycle_reload_count;
        if (patches_lifecycle_on_reload) patches_lifecycle_on_reload();
    }

    std::vector<std::string> getExtraDlls(const std::string &) {
        return {};
    }

}

static LDR_DLL_NOTIFICATION_DATA notification_data() {
    static wchar_t path[] = L"/target.dll";
    static UNICODE_STRING name {
        static_cast<unsigned short>((std::size(path) - 1) * sizeof(wchar_t)), path};
    return {.Loaded = {.FullDllName = &name}};
}

int main() {
    patcher::ldr_target_libraries = {"target.dll"};
    auto data = notification_data();
    patcher::patches_lifecycle_reload_count = 0;

    // With no state owner, the notification keeps the native immediate reload behavior.
    patcher::loader_notification(
        LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
    assert(patcher::patches_lifecycle_reload_count == 1);

    // A nested notification must preserve the outer loader-callback scope. Otherwise
    // the state-lock destructor starts its potentially blocking drain under the loader lock.
    patcher::patches_lifecycle_on_reload = [&] {
        patcher::loader_notification(
            LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
    };
    patcher::loader_notification(
        LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
    patcher::patches_lifecycle_on_reload = {};
    assert(patcher::patches_lifecycle_reload_count == 2);
    assert(patcher::patches_lifecycle_unlock_count == 0);
    patcher::patches_lifecycle_reload_count = 1;

    // A same-thread loader re-entry queues without waiting; the outer scope drains it.
    {
        patcher::StateLock lock;
        patcher::loader_notification(
            LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
        assert(patcher::patches_lifecycle_reload_count == 1);
    }
    assert(patcher::patches_lifecycle_reload_count == 2);

    // A contending thread also returns promptly, and release of the outer lock drains it.
    {
        patcher::StateLock lock;
        std::atomic_bool callback_done = false;
        std::thread callback([&] {
            patcher::loader_notification(
                LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
            callback_done.store(true, std::memory_order_release);
        });
        for (int i = 0; i < 100 && !callback_done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(callback_done.load(std::memory_order_acquire));
        callback.join();
        assert(patcher::patches_lifecycle_reload_count == 2);
    }
    assert(patcher::patches_lifecycle_reload_count == 3);

    // Pause after the outer owner unlocks but before it clears the callback barrier. The
    // callback must publish pending work and the owner must reload it after the handoff.
    const auto before_handoff = patcher::patches_lifecycle_reload_count.load();
    patcher::patches_lifecycle_hook_enabled = true;
    patcher::patches_lifecycle_hook_entered = false;
    patcher::patches_lifecycle_hook_continue = false;
    std::thread owner([&] {
        patcher::StateLock lock;
        patcher::loader_notification(
            LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
    });
    for (int i = 0; i < 100
            && !patcher::patches_lifecycle_hook_entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(patcher::patches_lifecycle_hook_entered.load(std::memory_order_acquire));

    std::atomic_bool handoff_callback_done = false;
    std::thread handoff_callback([&] {
        patcher::loader_notification(
            LDR_DLL_NOTIFICATION_REASON_LOADED, &data, nullptr);
        handoff_callback_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 100
            && !handoff_callback_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(handoff_callback_done.load(std::memory_order_acquire));
    patcher::patches_lifecycle_hook_continue = true;
    handoff_callback.join();
    owner.join();
    patcher::patches_lifecycle_hook_enabled = false;
    assert(patcher::patches_lifecycle_reload_count == before_handoff + 2);

    std::cout << "Native patch lifecycle tests passed: immediate reload, nonblocking contention, and queued drain.\n";
}
