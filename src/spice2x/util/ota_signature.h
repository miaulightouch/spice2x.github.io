#pragma once

#include <filesystem>
#include <string>

namespace ota {
    struct Signature {
        std::string status;
        std::string signer;
        std::string detail;
    };

    Signature verify_signature(const std::filesystem::path &path);
}
