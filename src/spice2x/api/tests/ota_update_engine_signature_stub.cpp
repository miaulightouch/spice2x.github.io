#include "util/ota_signature.h"

namespace ota {
    Signature verify_signature(const std::filesystem::path &) {
        return {"unsigned", {}, "Wine OTA harness"};
    }
}
