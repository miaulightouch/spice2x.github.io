#include "ota_update.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#ifdef _WIN32
#include <chrono>
#include <thread>
#endif
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include "launcher/shutdown.h"
#else
#include <unistd.h>
#endif

#ifndef SPICE_XP
#define SPICE_XP 0
#endif

namespace ota {
namespace {

    enum class UploadPhase {
        uploading,
        finished,
        applying,
    };

    struct FileState {
        UploadFile upload;
        std::string canonical_name;
        std::filesystem::path path;
        uint64_t received = 0;
    };

    struct UploadState {
        std::string id;
        std::filesystem::path module_path;
        std::filesystem::path stage_path;
        std::string active_name;
        std::vector<FileState> files;
        UploadPhase phase = UploadPhase::uploading;
#ifdef _WIN32
        HANDLE stage_lock = INVALID_HANDLE_VALUE;
        ~UploadState() {
            if (stage_lock != INVALID_HANDLE_VALUE) {
                CloseHandle(stage_lock);
            }
        }
#endif
    };

    std::mutex upload_mutex;
    std::unique_ptr<UploadState> active_upload;
    [[noreturn]] void fail(const char *message) {
        throw std::runtime_error(message);
    }

    static std::string lowercase_ascii(std::string value) {
        for (auto &character : value) {
            if (character >= 'A' && character <= 'Z') {
                character = static_cast<char>(character - 'A' + 'a');
            }
        }
        return value;
    }

    static std::wstring lowercase_ascii(std::wstring value) {
        for (auto &character : value) {
            if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
        }
        return value;
    }

#ifdef _WIN32
    static bool is_hex_id(std::string_view value) {
        if (value.size() != 32) {
            return false;
        }
        for (const auto character : value) {
            if (!((character >= '0' && character <= '9') ||
                    (character >= 'a' && character <= 'f') ||
                    (character >= 'A' && character <= 'F'))) {
                return false;
            }
        }
        return true;
    }
#endif

    static std::string random_id() {
        std::array<uint8_t, 16> bytes{};
#ifdef _WIN32
        HCRYPTPROV provider = 0;
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                CRYPT_VERIFYCONTEXT | CRYPT_SILENT) ||
                !CryptGenRandom(provider, static_cast<DWORD>(bytes.size()), bytes.data())) {
            if (provider) {
                CryptReleaseContext(provider, 0);
            }
            fail("could not obtain secure OTA upload id");
        }
        CryptReleaseContext(provider, 0);
#else
        try {
            std::random_device random;
            for (auto &byte : bytes) {
                byte = static_cast<uint8_t>(random() & 0xffu);
            }
        } catch (...) {
            fail("could not obtain secure OTA upload id");
        }
#endif

        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            result.push_back(digits[byte >> 4]);
            result.push_back(digits[byte & 0x0f]);
        }
        return result;
    }

    static std::filesystem::path module_file_path() {
#ifdef _WIN32
        std::wstring value(MAX_PATH, L'\0');
        while (true) {
            SetLastError(ERROR_SUCCESS);
            const auto length = GetModuleFileNameW(nullptr, value.data(),
                    static_cast<DWORD>(value.size()));
            if (length == 0) {
                fail("GetModuleFileNameW failed");
            }
            if (length < value.size() - 1 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                value.resize(length);
                return std::filesystem::path(std::move(value));
            }
            value.resize(value.size() * 2);
        }
#else
        std::array<char, 4096> value{};
        const auto length = readlink("/proc/self/exe", value.data(), value.size() - 1);
        if (length <= 0) {
            fail("could not determine current executable");
        }
        value[static_cast<size_t>(length)] = '\0';
        return std::filesystem::path(value.data());
#endif
    }

    static std::string active_executable_name(const std::filesystem::path &module_path) {
        const auto wide_name = lowercase_ascii(module_path.filename().wstring());
        if (wide_name != L"spice.exe" && wide_name != L"spice64.exe") {
            fail("OTA is available only from spice.exe or spice64.exe");
        }
        return std::string(wide_name.begin(), wide_name.end());
    }

#ifdef _WIN32
    static bool remove_tree_retry(const std::filesystem::path &path);

    static void cleanup_stale_staging(const std::filesystem::path &module_path) {
        std::error_code error;
        std::filesystem::directory_iterator entries(module_path, error);
        if (error) {
            throw std::runtime_error("could not inspect OTA staging directory");
        }
        const std::wstring prefix = L".spice2x-ota-";
        for (const auto &entry : entries) {
            error.clear();
            if (!entry.is_directory(error) || error) {
                continue;
            }
            const auto name = entry.path().filename().wstring();
            if (name.size() != prefix.size() + 32 ||
                    name.compare(0, prefix.size(), prefix) != 0 ||
                    !is_hex_id(std::string(name.begin() + prefix.size(), name.end()))) {
                continue;
            }
            const auto stage_path = entry.path();
            if (std::filesystem::exists(stage_path / "manifest.bin") ||
                    std::filesystem::exists(stage_path / "previous-active.backup") ||
                    std::filesystem::exists(stage_path / "previous-config.backup")) {
                continue;
            }
            const auto lock_path = stage_path / "stage.lock";
            const auto lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lock == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_NOT_FOUND && !remove_tree_retry(stage_path)) {
                    throw std::runtime_error("could not remove stale OTA staging files");
                }
                continue;
            }
            CloseHandle(lock);
            if (!remove_tree_retry(stage_path)) {
                throw std::runtime_error("could not remove stale OTA staging files");
            }
        }
    }
#endif

    static std::string canonical_upload_name(const std::string &name,
            const std::string &active_name) {
        const auto lower_name = lowercase_ascii(name);
        if (lower_name == active_name || lower_name == "spicecfg.exe") {
            return lower_name;
        }
        fail("unsupported OTA filename");
    }

    static FileState *find_file(UploadState &upload, const std::string &name) {
        const auto canonical_name = canonical_upload_name(name, upload.active_name);
        for (auto &file : upload.files) {
            if (file.canonical_name == canonical_name) {
                return &file;
            }
        }
        return nullptr;
    }

#ifndef _WIN32
    static void remove_tree(const std::filesystem::path &path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            throw std::runtime_error("could not remove OTA staging files");
        }
    }
#endif

#ifdef _WIN32
    static void release_stage_lock(UploadState &upload) {
        if (upload.stage_lock != INVALID_HANDLE_VALUE) {
            CloseHandle(upload.stage_lock);
            upload.stage_lock = INVALID_HANDLE_VALUE;
        }
    }
#endif

    static bool read_file_bytes(const std::filesystem::path &path, uint64_t offset,
            void *output, size_t size) {
        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (error || offset > file_size || size > file_size - offset) {
            return false;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            return false;
        }
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.read(static_cast<char *>(output), static_cast<std::streamsize>(size));
        return stream.good() || stream.gcount() == static_cast<std::streamsize>(size);
    }

    static uint16_t read_u16(const std::array<uint8_t, 2> &bytes) {
        return static_cast<uint16_t>(bytes[0]) |
                (static_cast<uint16_t>(bytes[1]) << 8);
    }

    static uint32_t read_u32(const std::array<uint8_t, 4> &bytes) {
        return static_cast<uint32_t>(bytes[0]) |
                (static_cast<uint32_t>(bytes[1]) << 8) |
                (static_cast<uint32_t>(bytes[2]) << 16) |
                (static_cast<uint32_t>(bytes[3]) << 24);
    }

    static uint16_t read_file_u16(const std::filesystem::path &path, uint64_t offset) {
        std::array<uint8_t, 2> bytes{};
        if (!read_file_bytes(path, offset, bytes.data(), bytes.size())) {
            fail("invalid PE header");
        }
        return read_u16(bytes);
    }

    static uint32_t read_file_u32(const std::filesystem::path &path, uint64_t offset) {
        std::array<uint8_t, 4> bytes{};
        if (!read_file_bytes(path, offset, bytes.data(), bytes.size())) {
            fail("invalid PE header");
        }
        return read_u32(bytes);
    }

    static void validate_pe(const std::filesystem::path &path, bool main_executable,
            const std::string &active_name = {}) {
        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (error || file_size < 0x40) {
            fail("OTA payload is not a PE file");
        }

        std::array<uint8_t, 2> mz{};
        if (!read_file_bytes(path, 0, mz.data(), mz.size()) || mz[0] != 'M' || mz[1] != 'Z') {
            fail("OTA payload is not a PE file");
        }
        const auto pe_offset = read_file_u32(path, 0x3c);
        if (pe_offset > file_size || file_size - pe_offset < 24) {
            fail("invalid PE header");
        }

        std::array<uint8_t, 4> signature{};
        if (!read_file_bytes(path, pe_offset, signature.data(), signature.size()) ||
                signature[0] != 'P' || signature[1] != 'E' ||
                signature[2] != '\0' || signature[3] != '\0') {
            fail("invalid PE header");
        }

        const auto machine = read_file_u16(path, pe_offset + 4);
        const auto optional_size = read_file_u16(path, pe_offset + 20);
        const auto optional_offset = static_cast<uint64_t>(pe_offset) + 24;
        if (optional_size < 68 || optional_offset > file_size ||
                optional_size > file_size - optional_offset) {
            fail("invalid PE optional header");
        }

        const auto optional_magic = read_file_u16(path, optional_offset);
        const bool is_x86 = machine == 0x014c && optional_magic == 0x010b;
        const bool is_x64 = machine == 0x8664 && optional_magic == 0x020b;
        const bool expected_main_architecture = active_name == "spice.exe"
                ? is_x86 : active_name == "spice64.exe" && is_x64;
        if (main_executable ? !expected_main_architecture : !is_x86) {
            fail("OTA payload architecture is not supported");
        }

#if SPICE_XP
        const auto major_os = read_file_u16(path, optional_offset + 40);
        const auto minor_os = read_file_u16(path, optional_offset + 42);
        const auto major_subsystem = read_file_u16(path, optional_offset + 48);
        const auto minor_subsystem = read_file_u16(path, optional_offset + 50);
        const auto version_exceeds_xp = [](uint16_t major, uint16_t minor) {
            return major > 5 || (major == 5 && minor > 1);
        };
        if (version_exceeds_xp(major_os, minor_os) ||
                version_exceeds_xp(major_subsystem, minor_subsystem)) {
            fail("OTA payload requires a newer Windows version");
        }
#endif
    }

    static uint32_t crc32_file(const std::filesystem::path &path) {
        static const auto table = [] {
            std::array<uint32_t, 256> values{};
            for (uint32_t index = 0; index < values.size(); ++index) {
                auto value = index;
                for (int bit = 0; bit < 8; ++bit) {
                    value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
                }
                values[index] = value;
            }
            return values;
        }();
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            fail("could not read OTA payload");
        }
        uint32_t crc = 0xffffffffu;
        std::array<uint8_t, 64 * 1024> buffer{};
        while (stream) {
            stream.read(reinterpret_cast<char *>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
            const auto count = stream.gcount();
            for (std::streamsize i = 0; i < count; ++i) {
                crc = (crc >> 8) ^ table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xffu];
            }
        }
        if (!stream.eof()) {
            fail("could not read OTA payload");
        }
        return crc ^ 0xffffffffu;
    }

    static Signature verify_signature_advisory(const std::filesystem::path &path) {
        try {
            auto result = verify_signature(path);
            if (result.status != "verified" && result.status != "unsigned" &&
                    result.status != "untrusted" && result.status != "invalid" &&
                    result.status != "unsupported") {
                result.status = "unsupported";
                result.detail = "signature verifier returned an unknown status";
            }
            return result;
        } catch (...) {
            Signature result{};
            result.status = "unsupported";
            result.detail = "signature verification unavailable";
            return result;
        }
    }

#ifdef _WIN32

    static std::wstring current_directory() {
        std::wstring value(MAX_PATH, L'\0');
        while (true) {
            const auto length = GetCurrentDirectoryW(static_cast<DWORD>(value.size()), value.data());
            if (length == 0) {
                fail("GetCurrentDirectoryW failed");
            }
            if (length < value.size()) {
                value.resize(length);
                return value;
            }
            value.resize(length + 1);
        }
    }

    static std::wstring environment_value(const wchar_t *name, bool &present) {
        const auto required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) {
            present = false;
            return {};
        }
        std::wstring value(required + 1, L'\0');
        const auto length = GetEnvironmentVariableW(name, value.data(),
                static_cast<DWORD>(value.size()));
        if (length == 0) {
            present = false;
            return {};
        }
        value.resize(length);
        present = true;
        return value;
    }

    static void set_environment_value(const wchar_t *name, const std::wstring *value) {
        if (!SetEnvironmentVariableW(name, value ? value->c_str() : nullptr)) {
            fail("SetEnvironmentVariableW failed");
        }
    }

    static std::vector<wchar_t> mutable_command_line(const std::wstring &command_line) {
        std::vector<wchar_t> value(command_line.begin(), command_line.end());
        value.push_back(L'\0');
        return value;
    }

    static PROCESS_INFORMATION start_process(const std::filesystem::path &application,
            const std::wstring &command_line, const std::filesystem::path &working_directory,
            const std::wstring *ready_event) {
        constexpr wchar_t ready_variable[] = L"SPICE_OTA_READY_EVENT";
        bool old_present = false;
        const auto old_value = environment_value(ready_variable, old_present);
        if (ready_event) {
            set_environment_value(ready_variable, ready_event);
        } else {
            set_environment_value(ready_variable, nullptr);
        }

        auto command = mutable_command_line(command_line);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const auto started = CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                working_directory.c_str(), &startup, &process);

        if (old_present) {
            set_environment_value(ready_variable, &old_value);
        } else {
            set_environment_value(ready_variable, nullptr);
        }
        if (!started) {
            fail("could not start OTA process");
        }
        return process;
    }

    static void close_process(PROCESS_INFORMATION &process) {
        if (process.hThread) {
            CloseHandle(process.hThread);
            process.hThread = nullptr;
        }
        if (process.hProcess) {
            CloseHandle(process.hProcess);
            process.hProcess = nullptr;
        }
    }

    static bool move_file_retry(const std::filesystem::path &from,
            const std::filesystem::path &to, DWORD flags) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (MoveFileExW(from.c_str(), to.c_str(), flags | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            const auto error = GetLastError();
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) {
                return false;
            }
            Sleep(100);
        }
        return false;
    }

    static bool delete_file_retry(const std::filesystem::path &path) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (DeleteFileW(path.c_str())) {
                return true;
            }
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) {
                return true;
            }
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) {
                return false;
            }
            Sleep(100);
        }
        return false;
    }

    static bool remove_tree_retry(const std::filesystem::path &path) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (!std::filesystem::exists(path, error) && !error) {
                return true;
            }
            if (RemoveDirectoryW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                return true;
            }
            Sleep(100);
        }
        return false;
    }

    static bool same_path(const std::filesystem::path &left,
            const std::filesystem::path &right) {
        auto left_string = lowercase_ascii(left.lexically_normal().wstring());
        auto right_string = lowercase_ascii(right.lexically_normal().wstring());
        return left_string == right_string;
    }

    static void write_u32(std::ofstream &stream, uint32_t value) {
        stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
    }

    static void write_u64(std::ofstream &stream, uint64_t value) {
        stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
    }

    static void write_wstring(std::ofstream &stream, const std::wstring &value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) {
            fail("OTA manifest string is too long");
        }
        write_u32(stream, static_cast<uint32_t>(value.size()));
        stream.write(reinterpret_cast<const char *>(value.data()),
                static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
    }

    static uint32_t read_manifest_u32(std::ifstream &stream) {
        uint32_t value = 0;
        stream.read(reinterpret_cast<char *>(&value), sizeof(value));
        if (!stream) {
            fail("invalid OTA manifest");
        }
        return value;
    }

    static uint64_t read_manifest_u64(std::ifstream &stream) {
        uint64_t value = 0;
        stream.read(reinterpret_cast<char *>(&value), sizeof(value));
        if (!stream) {
            fail("invalid OTA manifest");
        }
        return value;
    }

    static std::wstring read_wstring(std::ifstream &stream) {
        const auto length = read_manifest_u32(stream);
        if (length > 32768) {
            fail("invalid OTA manifest");
        }
        std::wstring value(length, L'\0');
        stream.read(reinterpret_cast<char *>(value.data()),
                static_cast<std::streamsize>(value.size() * sizeof(wchar_t)));
        if (!stream) {
            fail("invalid OTA manifest");
        }
        return value;
    }

    struct Manifest {
        uint32_t parent_pid = 0;
        uint64_t main_size = 0;
        uint32_t main_crc32 = 0;
        uint64_t config_size = 0;
        uint32_t config_crc32 = 0;
        bool has_config = false;
        std::filesystem::path active_path;
        std::filesystem::path config_path;
        std::wstring command_line;
        std::wstring working_directory;
        std::wstring ready_event;
        std::wstring helper_ready_event;
    };

    static void write_manifest(const std::filesystem::path &path, const Manifest &manifest) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            fail("could not write OTA manifest");
        }
        write_u32(stream, 0x32544f41u); // AOT2 in little-endian, versioned below.
        write_u32(stream, 1);
        write_u32(stream, manifest.parent_pid);
        write_u64(stream, manifest.main_size);
        write_u32(stream, manifest.main_crc32);
        write_u64(stream, manifest.config_size);
        write_u32(stream, manifest.config_crc32);
        write_u32(stream, manifest.has_config ? 1u : 0u);
        write_wstring(stream, manifest.active_path.wstring());
        write_wstring(stream, manifest.config_path.wstring());
        write_wstring(stream, manifest.command_line);
        write_wstring(stream, manifest.working_directory);
        write_wstring(stream, manifest.ready_event);
        write_wstring(stream, manifest.helper_ready_event);
        if (!stream.good()) {
            fail("could not write OTA manifest");
        }
    }

    static Manifest read_manifest(const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            fail("could not read OTA manifest");
        }
        if (read_manifest_u32(stream) != 0x32544f41u || read_manifest_u32(stream) != 1) {
            fail("invalid OTA manifest");
        }
        Manifest manifest;
        manifest.parent_pid = read_manifest_u32(stream);
        manifest.main_size = read_manifest_u64(stream);
        manifest.main_crc32 = read_manifest_u32(stream);
        manifest.config_size = read_manifest_u64(stream);
        manifest.config_crc32 = read_manifest_u32(stream);
        manifest.has_config = read_manifest_u32(stream) != 0;
        manifest.active_path = read_wstring(stream);
        manifest.config_path = read_wstring(stream);
        manifest.command_line = read_wstring(stream);
        manifest.working_directory = read_wstring(stream);
        manifest.ready_event = read_wstring(stream);
        manifest.helper_ready_event = read_wstring(stream);
        return manifest;
    }

    static void validate_manifest(const Manifest &manifest, const std::string &id,
            const std::filesystem::path &module_path,
            const std::filesystem::path &stage_path) {
        const auto active_name = active_executable_name(manifest.active_path);
        if (!same_path(manifest.active_path, module_path / active_name) ||
                manifest.parent_pid == 0 || manifest.command_line.empty() ||
                manifest.working_directory.empty()) {
            fail("invalid OTA manifest paths");
        }
        if (manifest.has_config &&
                !same_path(manifest.config_path, module_path / "spicecfg.exe")) {
            fail("invalid OTA manifest paths");
        }
        const auto expected_stage = module_path / (".spice2x-ota-" + id);
        if (!same_path(stage_path, expected_stage) || !std::filesystem::exists(stage_path)) {
            fail("invalid OTA staging path");
        }
        if (manifest.ready_event != L"Local\\Spice2xOtaReady-" +
                std::wstring(id.begin(), id.end())) {
            fail("invalid OTA readiness event");
        }
        if (manifest.helper_ready_event != L"Local\\Spice2xOtaHelperReady-" +
                std::wstring(id.begin(), id.end())) {
            fail("invalid OTA helper readiness event");
        }
    }

    struct Replacement {
        std::filesystem::path active;
        std::filesystem::path config;
        std::filesystem::path active_backup;
        std::filesystem::path config_backup;
        std::filesystem::path previous_active_backup;
        std::filesystem::path previous_config_backup;
        bool active_old = false;
        bool active_new = false;
        bool config_old = false;
        bool config_new = false;
        bool previous_active = false;
        bool previous_config = false;
    };

    static bool restore_replacement(Replacement &replacement) {
        bool restored = true;
        if (replacement.config_new) {
            restored = delete_file_retry(replacement.config) && restored;
        }
        if (replacement.config_old) {
            restored = move_file_retry(replacement.config_backup, replacement.config, 0) && restored;
        }
        if (replacement.active_new) {
            restored = delete_file_retry(replacement.active) && restored;
        }
        if (replacement.active_old) {
            restored = move_file_retry(replacement.active_backup, replacement.active, 0) && restored;
        }
        if (replacement.previous_config) {
            if (std::filesystem::exists(replacement.config_backup) &&
                    !delete_file_retry(replacement.config_backup)) {
                restored = false;
            }
            restored = move_file_retry(replacement.previous_config_backup,
                    replacement.config_backup, 0) && restored;
        }
        if (replacement.previous_active) {
            if (std::filesystem::exists(replacement.active_backup) &&
                    !delete_file_retry(replacement.active_backup)) {
                restored = false;
            }
            restored = move_file_retry(replacement.previous_active_backup,
                    replacement.active_backup, 0) && restored;
        }
        return restored;
    }

    static void replace_files(const Manifest &manifest, const std::filesystem::path &stage_path,
            Replacement &replacement) {
        replacement.active = manifest.active_path;
        replacement.config = manifest.config_path;
        replacement.active_backup = replacement.active.parent_path() /
                (".spice2x-previous-" + active_executable_name(manifest.active_path));
        replacement.config_backup = replacement.config.parent_path() /
                ".spice2x-previous-spicecfg.exe";
        replacement.previous_active_backup = stage_path / "previous-active.backup";
        replacement.previous_config_backup = stage_path / "previous-config.backup";

        const auto active_stage = stage_path / (active_executable_name(manifest.active_path) + ".part");
        std::error_code error;
        if (std::filesystem::exists(replacement.active_backup, error)) {
            if (error || !move_file_retry(replacement.active_backup,
                    replacement.previous_active_backup, 0)) {
                fail("could not rotate the previous Spice backup");
            }
            replacement.previous_active = true;
        } else if (error) {
            fail("could not inspect the previous Spice backup");
        }
        if (!move_file_retry(replacement.active, replacement.active_backup, 0)) {
            fail("could not back up active Spice executable");
        }
        replacement.active_old = true;
        if (!move_file_retry(active_stage, replacement.active, 0)) {
            fail("could not install updated Spice executable");
        }
        replacement.active_new = true;

        if (manifest.has_config) {
            const auto config_stage = stage_path / "spicecfg.exe.part";
            error.clear();
            if (std::filesystem::exists(replacement.config_backup, error)) {
                if (error || !move_file_retry(replacement.config_backup,
                        replacement.previous_config_backup, 0)) {
                    fail("could not rotate the previous spicecfg.exe backup");
                }
                replacement.previous_config = true;
            } else if (error) {
                fail("could not inspect the previous spicecfg.exe backup");
            }
            error.clear();
            if (std::filesystem::exists(replacement.config, error)) {
                if (error || !move_file_retry(replacement.config, replacement.config_backup, 0)) {
                    fail("could not back up spicecfg.exe");
                }
                replacement.config_old = true;
            } else if (error) {
                fail("could not inspect spicecfg.exe");
            }
            if (!move_file_retry(config_stage, replacement.config, 0)) {
                fail("could not install updated spicecfg.exe");
            }
            replacement.config_new = true;
        }
    }

    static void validate_staged_files(const Manifest &manifest,
            const std::filesystem::path &stage_path) {
        const auto active_stage = stage_path / (active_executable_name(manifest.active_path) + ".part");
        std::error_code error;
        if (!std::filesystem::exists(active_stage, error) || error ||
                std::filesystem::file_size(active_stage, error) != manifest.main_size || error ||
                crc32_file(active_stage) != manifest.main_crc32) {
            fail("staged Spice executable changed");
        }
        validate_pe(active_stage, true, active_executable_name(manifest.active_path));
        if (manifest.has_config) {
            const auto config_stage = stage_path / "spicecfg.exe.part";
            if (!std::filesystem::exists(config_stage, error) || error ||
                    std::filesystem::file_size(config_stage, error) != manifest.config_size || error ||
                    crc32_file(config_stage) != manifest.config_crc32) {
                fail("staged spicecfg.exe changed");
            }
            validate_pe(config_stage, false);
        }
    }

    static void wait_for_parent(uint32_t parent_pid) {
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
        if (!process) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) {
                return;
            }
            fail("could not wait for the original Spice process");
        }
        const auto result = WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        if (result != WAIT_OBJECT_0) {
            fail("could not wait for the original Spice process");
        }
    }

    static bool child_is_alive(PROCESS_INFORMATION &child) {
        if (GetProcessId(child.hProcess) != child.dwProcessId) {
            return false;
        }
        return WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT;
    }

    static void terminate_child(PROCESS_INFORMATION &child) {
        if (child.hProcess && child_is_alive(child)) {
            TerminateProcess(child.hProcess, 1);
            WaitForSingleObject(child.hProcess, 5000);
        }
        close_process(child);
    }

    static bool start_restored(const Manifest &manifest) {
        try {
            auto child = start_process(manifest.active_path, manifest.command_line,
                    manifest.working_directory, nullptr);
            close_process(child);
            return true;
        } catch (...) {
            return false;
        }
    }

    static int run_helper_job(const std::string &id) {
        const auto helper_path = module_file_path();
        const auto module_path = helper_path.parent_path();
        const auto stage_path = module_path / (".spice2x-ota-" + id);
        const auto manifest_path = stage_path / "manifest.bin";
        Manifest manifest;
        bool manifest_loaded = false;
        bool parent_gone = false;
        bool replacement_started = false;
        HANDLE helper_ready_event = nullptr;
        HANDLE ready_event = nullptr;
        PROCESS_INFORMATION child{};
        Replacement replacement;
        try {
            manifest = read_manifest(manifest_path);
            manifest_loaded = true;
            validate_manifest(manifest, id, module_path, stage_path);
            validate_staged_files(manifest, stage_path);

            helper_ready_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                    manifest.helper_ready_event.c_str());
            if (!helper_ready_event || !SetEvent(helper_ready_event)) {
                fail("could not acknowledge OTA helper initialization");
            }
            CloseHandle(helper_ready_event);
            helper_ready_event = nullptr;

            wait_for_parent(manifest.parent_pid);
            parent_gone = true;
            validate_staged_files(manifest, stage_path);

            SetLastError(ERROR_SUCCESS);
            ready_event = CreateEventW(nullptr, TRUE, FALSE, manifest.ready_event.c_str());
            if (!ready_event || GetLastError() == ERROR_ALREADY_EXISTS) {
                if (ready_event) {
                    CloseHandle(ready_event);
                    ready_event = nullptr;
                }
                fail("could not create OTA readiness event");
            }

            replacement_started = true;
            replace_files(manifest, stage_path, replacement);
            bool updated = false;
            child = start_process(manifest.active_path, manifest.command_line,
                    manifest.working_directory, &manifest.ready_event);
            const HANDLE waits[] = {ready_event, child.hProcess};
            const auto ready = WaitForMultipleObjects(2, waits, FALSE, 30000);
            updated = ready == WAIT_OBJECT_0 && child_is_alive(child);
            if (updated) {
                close_process(child);
                CloseHandle(ready_event);
                ready_event = nullptr;
                remove_tree_retry(stage_path);
                return EXIT_SUCCESS;
            }
        } catch (...) {
            // All post-parent failures are handled below so the original process is restarted.
        }

        if (helper_ready_event) {
            CloseHandle(helper_ready_event);
        }
        if (ready_event) {
            CloseHandle(ready_event);
        }
        terminate_child(child);
        if (!parent_gone || !manifest_loaded) {
            return EXIT_FAILURE;
        }

        const auto restored = replacement_started ? restore_replacement(replacement) : true;
        if (restored) {
            remove_tree_retry(stage_path);
            start_restored(manifest);
        }
        return EXIT_FAILURE;
    }

#endif // _WIN32

} // namespace

std::string begin(const std::vector<UploadFile> &files) {
    std::lock_guard<std::mutex> lock(upload_mutex);
    if (active_upload) {
        fail("another OTA upload is already active");
    }
    if (files.empty() || files.size() > 2) {
        fail("OTA requires one or two files");
    }

    const auto executable_path = module_file_path();
    const auto module_path = executable_path.parent_path();
    const auto active_name = active_executable_name(executable_path);
#ifdef _WIN32
    cleanup_stale_staging(module_path);
#endif
    bool has_main = false;
    bool has_config = false;
    uint64_t total_size = 0;
    std::vector<FileState> states;
    states.reserve(files.size());
    for (const auto &file : files) {
        if (file.size > max_file_bytes || total_size > max_total_bytes - file.size) {
            fail("OTA payload is too large");
        }
        const auto canonical_name = canonical_upload_name(file.name, active_name);
        if (canonical_name == active_name) {
            if (has_main) {
                fail("OTA payload contains duplicate files");
            }
            has_main = true;
        } else {
            if (has_config) {
                fail("OTA payload contains duplicate files");
            }
            has_config = true;
        }
        total_size += file.size;
        states.push_back(FileState {file, canonical_name, {}, 0});
    }
    if (!has_main) {
        fail("OTA payload is missing the active Spice executable");
    }

    auto id = random_id();
    auto stage_path = module_path / (".spice2x-ota-" + id);
    while (std::filesystem::exists(stage_path)) {
        id = random_id();
        stage_path = module_path / (".spice2x-ota-" + id);
    }
    // The stage is beside the executable so replacement remains on one volume.
#ifdef _WIN32
    HANDLE stage_lock = INVALID_HANDLE_VALUE;
 #endif
    try {
        std::filesystem::create_directories(stage_path);
#ifdef _WIN32
        stage_lock = CreateFileW((stage_path / "stage.lock").c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (stage_lock == INVALID_HANDLE_VALUE) {
            fail("could not lock OTA staging files");
        }
#endif
        for (auto &state : states) {
            state.path = stage_path / (state.canonical_name + ".part");
            std::ofstream stream(state.path, std::ios::binary | std::ios::trunc);
            if (!stream) {
                fail("could not create OTA staging file");
            }
        }
    } catch (...) {
#ifdef _WIN32
        if (stage_lock != INVALID_HANDLE_VALUE) {
            CloseHandle(stage_lock);
        }
#endif
        std::error_code ignored;
        std::filesystem::remove_all(stage_path, ignored);
        throw;
    }

    try {
        active_upload = std::make_unique<UploadState>();
        active_upload->id = std::move(id);
        active_upload->module_path = module_path;
        active_upload->stage_path = stage_path;
        active_upload->active_name = active_name;
        active_upload->files = std::move(states);
        active_upload->phase = UploadPhase::uploading;
#ifdef _WIN32
        active_upload->stage_lock = stage_lock;
        stage_lock = INVALID_HANDLE_VALUE;
#endif
    } catch (...) {
        active_upload.reset();
#ifdef _WIN32
        if (stage_lock != INVALID_HANDLE_VALUE) {
            CloseHandle(stage_lock);
        }
#endif
        std::error_code ignored;
        std::filesystem::remove_all(stage_path, ignored);
        throw;
    }
    return active_upload->id;
}

uint64_t chunk(const std::string &id, const std::string &name, uint64_t offset,
        const std::vector<uint8_t> &bytes) {
    std::lock_guard<std::mutex> lock(upload_mutex);
    if (!active_upload || active_upload->id != id) {
        fail("unknown OTA upload");
    }
    if (active_upload->phase != UploadPhase::uploading) {
        fail("OTA upload is no longer receiving data");
    }
    auto *file = find_file(*active_upload, name);
    if (!file) {
        fail("unknown OTA filename");
    }
    if (bytes.size() > chunk_bytes || offset != file->received ||
            bytes.size() > file->upload.size - file->received) {
        fail("invalid OTA chunk");
    }
    if (!bytes.empty()) {
        std::fstream stream(file->path, std::ios::in | std::ios::out | std::ios::binary);
        if (!stream) {
            fail("could not open OTA staging file");
        }
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.write(reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            fail("could not write OTA staging file");
        }
    }
    file->received += bytes.size();
    return file->received;
}

std::vector<UploadedFile> finish(const std::string &id) {
    std::lock_guard<std::mutex> lock(upload_mutex);
    if (!active_upload || active_upload->id != id) {
        fail("unknown OTA upload");
    }
    if (active_upload->phase != UploadPhase::uploading) {
        fail("OTA upload is not receiving data");
    }

    for (const auto &file : active_upload->files) {
        if (file.received != file.upload.size) {
            fail("OTA upload is incomplete");
        }
        std::error_code error;
        if (std::filesystem::file_size(file.path, error) != file.upload.size || error ||
                crc32_file(file.path) != file.upload.crc32) {
            fail("OTA payload checksum mismatch");
        }
        validate_pe(file.path, file.canonical_name == active_upload->active_name,
                active_upload->active_name);
    }

    std::vector<UploadedFile> result;
    result.reserve(active_upload->files.size());
    for (const auto &file : active_upload->files) {
        result.push_back(UploadedFile {
                file.upload.name, file.upload.size, verify_signature_advisory(file.path)});
    }
    active_upload->phase = UploadPhase::finished;
    return result;
}

void cancel(const std::string &id) {
    std::lock_guard<std::mutex> lock(upload_mutex);
    if (!active_upload || active_upload->id != id) {
        return;
    }
    if (active_upload->phase == UploadPhase::applying) {
        return;
    }
    const auto stage_path = active_upload->stage_path;
#ifdef _WIN32
    release_stage_lock(*active_upload);
    const auto removed = remove_tree_retry(stage_path);
    // Release the logical upload even when cleanup needs a later begin() retry.
    active_upload.reset();
    if (!removed) {
        throw std::runtime_error("could not remove OTA staging files");
    }
#else
    remove_tree(stage_path);
    active_upload.reset();
#endif
}

uint32_t executable_crc32() {
    return crc32_file(module_file_path());
}

void apply(const std::string &id) {
    std::lock_guard<std::mutex> lock(upload_mutex);
    if (!active_upload || active_upload->id != id) {
        fail("unknown OTA upload");
    }
    if (active_upload->phase != UploadPhase::finished) {
        fail("OTA upload is not ready to apply");
    }

#ifdef _WIN32
    const auto active_path = active_upload->module_path / active_upload->active_name;
    const auto helper_path = active_upload->module_path / ".spice2x-ota-helper.exe";
    const auto manifest_path = active_upload->stage_path / "manifest.bin";
    const auto ready_event = L"Local\\Spice2xOtaReady-" +
            std::wstring(active_upload->id.begin(), active_upload->id.end());
    const auto helper_ready_event = L"Local\\Spice2xOtaHelperReady-" +
            std::wstring(active_upload->id.begin(), active_upload->id.end());

    SetLastError(ERROR_SUCCESS);
    HANDLE helper_event = CreateEventW(nullptr, TRUE, FALSE, helper_ready_event.c_str());
    if (!helper_event || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (helper_event) {
            CloseHandle(helper_event);
        }
        fail("could not create OTA helper readiness event");
    }
    PROCESS_INFORMATION helper{};

    Manifest manifest;
    manifest.parent_pid = GetCurrentProcessId();
    const auto *main_file = find_file(*active_upload, active_upload->active_name);
    if (!main_file) {
        fail("OTA payload is missing the active Spice executable");
    }
    manifest.main_size = main_file->upload.size;
    manifest.main_crc32 = main_file->upload.crc32;
    for (const auto &file : active_upload->files) {
        if (file.canonical_name == "spicecfg.exe") {
            manifest.config_size = file.upload.size;
            manifest.config_crc32 = file.upload.crc32;
            manifest.has_config = true;
        }
    }
    manifest.active_path = active_path;
    manifest.config_path = active_upload->module_path / "spicecfg.exe";
    manifest.command_line = GetCommandLineW();
    manifest.working_directory = current_directory();
    manifest.ready_event = ready_event;
    manifest.helper_ready_event = helper_ready_event;

    try {
        write_manifest(manifest_path, manifest);
        if (!delete_file_retry(helper_path)) {
            fail("previous OTA helper is still running");
        }
        if (!CopyFileW(active_path.c_str(), helper_path.c_str(), TRUE)) {
            fail("could not create OTA helper");
        }
        std::wstring command = L"spice-ota-helper.exe --spice-ota-helper " +
                std::wstring(active_upload->id.begin(), active_upload->id.end());
        helper = start_process(helper_path, command, active_upload->module_path, nullptr);
        const auto ready = WaitForSingleObject(helper_event, 10000);
        const auto alive = GetProcessId(helper.hProcess) == helper.dwProcessId &&
                WaitForSingleObject(helper.hProcess, 0) == WAIT_TIMEOUT;
        if (ready != WAIT_OBJECT_0 || !alive) {
            fail("OTA helper did not initialize");
        }
        close_process(helper);
        CloseHandle(helper_event);
        active_upload->phase = UploadPhase::applying;
        try {
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                launcher::shutdown(0);
            }).detach();
        } catch (...) {
            launcher::shutdown(0);
        }
    } catch (...) {
        if (helper.hProcess) {
            TerminateProcess(helper.hProcess, 1);
        }
        close_process(helper);
        CloseHandle(helper_event);
        DeleteFileW(helper_path.c_str());
        DeleteFileW(manifest_path.c_str());
        throw;
    }
#else
    fail("OTA apply requires Windows");
#endif
}

bool run_update_helper(int argc, char *argv[], int &exitcode) {
#ifdef _WIN32
    if (argc < 3 || argv == nullptr || argv[1] == nullptr || argv[2] == nullptr ||
            std::string(argv[1]) != "--spice-ota-helper") {
        return false;
    }
    exitcode = EXIT_FAILURE;
    try {
        const std::string id(argv[2]);
        if (!is_hex_id(id)) {
            return true;
        }
        exitcode = run_helper_job(id);
    } catch (...) {
        exitcode = EXIT_FAILURE;
    }
    return true;
#else
    (void) argc;
    (void) argv;
    (void) exitcode;
    return false;
#endif
}

void acknowledge_update() {
#ifdef _WIN32
    const std::wstring prefix = L"Local\\Spice2xOtaReady-";
    const auto id = current_update_id();
    if (id.empty()) {
        return;
    }
    const auto event_name = prefix + std::wstring(id.begin(), id.end());
    const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
    if (!event) {
        return;
    }
    SetEvent(event);
    CloseHandle(event);
#endif
}

std::string current_update_id() {
#ifdef _WIN32
    constexpr wchar_t ready_variable[] = L"SPICE_OTA_READY_EVENT";
    constexpr wchar_t prefix[] = L"Local\\Spice2xOtaReady-";
    bool present = false;
    const auto event_name = environment_value(ready_variable, present);
    const auto prefix_length = std::wstring(prefix).size();
    if (!present || event_name.size() != prefix_length + 32 ||
            event_name.compare(0, prefix_length, prefix) != 0) {
        return {};
    }
    const std::string id(event_name.begin() + prefix_length, event_name.end());
    return is_hex_id(id) ? id : std::string();
#else
    return {};
#endif
}

} // namespace ota
