#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "util/ota_signature.h"

namespace ota {

    inline constexpr uint64_t max_file_bytes = 64ull * 1024ull * 1024ull;
    inline constexpr uint64_t max_total_bytes = 128ull * 1024ull * 1024ull;
    inline constexpr size_t chunk_bytes = 32768;

    struct UploadFile {
        std::string name;
        uint64_t size;
        uint32_t crc32;
    };

    struct UploadedFile {
        std::string name;
        uint64_t size;
        Signature signature;
    };

    std::string begin(const std::vector<UploadFile> &files);
    uint64_t chunk(const std::string &id, const std::string &name, uint64_t offset,
            const std::vector<uint8_t> &bytes);
    std::vector<UploadedFile> finish(const std::string &id);
    void cancel(const std::string &id);
    void apply(const std::string &id);
    uint32_t executable_crc32();

    // Called before normal launcher initialization. It returns true only for the helper process.
    bool run_update_helper(int argc, char *argv[], int &exitcode);

    // Called by the update child after its controller is ready.
    void acknowledge_update();
}
