#include "util/ota_update.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef OTA_HARNESS_BEHAVIOR
#define OTA_HARNESS_BEHAVIOR 0
#endif
#ifndef OTA_HARNESS_X86
#define OTA_HARNESS_X86 0
#endif

#if OTA_HARNESS_X86
static constexpr char active_name[] = "spice.exe";
#else
static constexpr char active_name[] = "spice64.exe";
#endif

namespace launcher {
    void shutdown(UINT exit_code) {
        ExitProcess(exit_code);
    }
}

static uint32_t crc32(const std::vector<uint8_t> &bytes) {
    uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xffffffffu;
}

static std::vector<uint8_t> read_file(const char *path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

[[maybe_unused]] static bool file_exists(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void write_marker(const char *path) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "ok\n";
}

static void seed_stale_staging() {
    constexpr char stage[] = ".spice2x-ota-0123456789abcdef0123456789abcdef";
    constexpr char lock_name[] = ".spice2x-ota-0123456789abcdef0123456789abcdef\\stage.lock";
    constexpr char abandoned_stage[] = ".spice2x-ota-fedcba9876543210fedcba9876543210";
    CreateDirectoryA(stage, nullptr);
    const auto lock = CreateFileA(lock_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (lock != INVALID_HANDLE_VALUE) {
        CloseHandle(lock);
    }
    const auto partial_name = std::string(stage) + "\\" + active_name + ".part";
    std::ofstream partial(partial_name, std::ios::binary | std::ios::trunc);
    partial << "interrupted upload";
    CreateDirectoryA(abandoned_stage, nullptr);
    const auto abandoned_partial_name = std::string(abandoned_stage) + "\\" + active_name +
            ".part";
    std::ofstream abandoned(abandoned_partial_name, std::ios::binary | std::ios::trunc);
    abandoned << "interrupted before lock";
}

[[maybe_unused]] static bool update_child() {
    return GetEnvironmentVariableA("SPICE_OTA_READY_EVENT", nullptr, 0) != 0;
}

static void upload_file(const std::string &id, const char *name,
        const std::vector<uint8_t> &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = (std::min)(ota::chunk_bytes, bytes.size() - offset);
        std::vector<uint8_t> part(bytes.begin() + offset, bytes.begin() + offset + count);
        ota::chunk(id, name, offset, part);
        offset += count;
    }
}

static void upload_and_apply(const char *payload_path, const char *config_path) {
    seed_stale_staging();
    const auto payload = read_file(payload_path);
    if (payload.empty()) {
        ExitProcess(2);
    }
    std::vector<uint8_t> config;
    if (config_path) {
        config = read_file(config_path);
        if (config.empty()) {
            ExitProcess(2);
        }
    }
    std::vector<ota::UploadFile> files {
        {active_name, payload.size(), crc32(payload)},
    };
    if (!config.empty()) {
        files.push_back({"spicecfg.exe", config.size(), crc32(config)});
    }
    const auto id = ota::begin(files);
    if (!file_exists(".spice2x-ota-0123456789abcdef0123456789abcdef") &&
            !file_exists(".spice2x-ota-fedcba9876543210fedcba9876543210")) {
        write_marker("stale.cleaned");
    }
    upload_file(id, active_name, payload);
    if (!config.empty()) {
        upload_file(id, "spicecfg.exe", config);
    }
    ota::finish(id);
    ota::apply(id);
}

static void upload_and_cancel() {
    seed_stale_staging();
    char module_name[MAX_PATH]{};
    const auto module_length = GetModuleFileNameA(nullptr, module_name, sizeof(module_name));
    if (module_length == 0 || module_length >= sizeof(module_name)) {
        ExitProcess(2);
    }
    const auto payload = read_file(module_name);
    if (payload.empty()) {
        ExitProcess(2);
    }

    const auto upload = [&payload] {
        const auto id = ota::begin({{active_name, payload.size(), crc32(payload)}});
        upload_file(id, active_name, payload);
        ota::finish(id);
        return id;
    };

    const auto first_id = upload();
    if (!file_exists(".spice2x-ota-0123456789abcdef0123456789abcdef") &&
            !file_exists(".spice2x-ota-fedcba9876543210fedcba9876543210")) {
        write_marker("stale.cleaned");
    }
    ota::cancel(first_id);
    const auto second_id = ota::begin({{active_name, payload.size(), crc32(payload)}});
    ota::cancel(second_id);
    write_marker("lifecycle.ready");
}

static void cancel_with_locked_file() {
    seed_stale_staging();
    char module_name[MAX_PATH]{};
    const auto module_length = GetModuleFileNameA(nullptr, module_name, sizeof(module_name));
    if (module_length == 0 || module_length >= sizeof(module_name)) {
        ExitProcess(2);
    }
    const auto payload = read_file(module_name);
    const auto id = ota::begin({{active_name, payload.size(), crc32(payload)}});

    std::filesystem::path stage;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(".", error)) {
        if (error || !entry.is_directory(error)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind(".spice2x-ota-", 0) == 0 &&
                std::filesystem::exists(entry.path() / (std::string(active_name) + ".part"))) {
            stage = entry.path();
            break;
        }
    }
    if (stage.empty()) {
        ExitProcess(2);
    }
    const auto locked_path = stage / (std::string(active_name) + ".part");
    const auto locked = CreateFileA(locked_path.string().c_str(), GENERIC_READ, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) {
        ExitProcess(2);
    }

    bool cancel_failed = false;
    try {
        ota::cancel(id);
    } catch (const std::runtime_error &) {
        cancel_failed = true;
    }
    CloseHandle(locked);
    if (!cancel_failed) {
        ExitProcess(3);
    }

    const auto retry_id = ota::begin({{active_name, payload.size(), crc32(payload)}});
    ota::cancel(retry_id);
    write_marker("cancel-failure.ready");
}

int main(int argc, char **argv) {
    int helper_exit = 0;
    if (ota::run_update_helper(argc, argv, helper_exit)) {
        return helper_exit;
    }

#if OTA_HARNESS_BEHAVIOR == 1
    if (update_child()) {
        write_marker("success.ready");
        ota::acknowledge_update();
        Sleep(1500);
        return 0;
    }
    if (file_exists("failure.started")) {
        write_marker("rollback.ready");
        Sleep(500);
        return 0;
    }
#elif OTA_HARNESS_BEHAVIOR == 2
    if (update_child()) {
        write_marker("failure.started");
        return 0;
    }
#else
    if (update_child()) {
        write_marker("success.ready");
        ota::acknowledge_update();
        Sleep(1500);
        return 0;
    }
    if (file_exists("failure.started")) {
        write_marker("rollback.ready");
        Sleep(500);
        return 0;
    }
#endif

    if (argc == 2 && std::string(argv[1]) == "--lifecycle") {
        upload_and_cancel();
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--cancel-failure") {
        cancel_with_locked_file();
        return 0;
    }
    if ((argc != 3 && argc != 4) || std::string(argv[1]) != "--apply") {
        return 2;
    }
    upload_and_apply(argv[2], argc == 4 ? argv[3] : nullptr);
    Sleep(5000);
    return 3;
}
