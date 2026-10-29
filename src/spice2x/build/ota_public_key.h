#pragma once

#include <array>
#include <cstdint>

#if __has_include("build/ota_public_key.local.h")
#include "build/ota_public_key.local.h"
#else
namespace ota {
    inline constexpr std::array<std::uint8_t, 20> kPinnedPublicKeySha1{};
}
#endif
