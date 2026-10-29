#include "util/ota_update.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

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

static std::vector<uint8_t> pe(uint16_t machine, uint16_t optional_magic) {
    std::vector<uint8_t> image(1024, 0);
    image[0] = 'M';
    image[1] = 'Z';
    image[0x3c] = 0x80;
    image[0x80] = 'P';
    image[0x81] = 'E';
    image[0x84] = static_cast<uint8_t>(machine);
    image[0x85] = static_cast<uint8_t>(machine >> 8);
    image[0x94] = 0xe0;
    image[0x98] = static_cast<uint8_t>(optional_magic);
    image[0x99] = static_cast<uint8_t>(optional_magic >> 8);
    image[0x98 + 40] = 5;
    image[0x98 + 48] = 5;
    image[0x200] = 0x42;
    return image;
}

static void expect_failure(const auto &function) {
    bool failed = false;
    try {
        function();
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);
}

static void upload(const std::string &id, const std::string &name,
        const std::vector<uint8_t> &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = std::min<size_t>(ota::chunk_bytes, bytes.size() - offset);
        std::vector<uint8_t> part(bytes.begin() + offset, bytes.begin() + offset + count);
        assert(ota::chunk(id, name, offset, part) == offset + count);
        offset += count;
    }
}

int main() {
    const auto main_image = pe(0x8664, 0x20b);
    const auto wrong_main_image = pe(0x14c, 0x10b);
    const auto config_image = pe(0x14c, 0x10b);
    assert(ota::current_update_id().empty());
    (void) ota::executable_crc32();

    expect_failure([&] {
        ota::begin({{"other.exe", main_image.size(), crc32(main_image)}});
    });
    expect_failure([&] {
        ota::begin({{"spice64.exe", ota::max_file_bytes + 1, 0}});
    });

    const auto id = ota::begin({
        {"spice64.exe", main_image.size(), crc32(main_image)},
        {"spicecfg.exe", config_image.size(), crc32(config_image)},
    });
    assert(id.size() == 32);
    expect_failure([&] {
        ota::chunk(id, "spice64.exe", 1, {main_image[0]});
    });
    expect_failure([&] {
        ota::chunk(id, "spice64.exe", 0, std::vector<uint8_t>(ota::chunk_bytes + 1));
    });
    upload(id, "spice64.exe", main_image);
    upload(id, "spicecfg.exe", config_image);
    const auto files = ota::finish(id);
    assert(files.size() == 2);
    assert(files[0].signature.status == "unsupported");
    ota::cancel(id);

    const auto wrong_main_id = ota::begin({
        {"spice64.exe", wrong_main_image.size(), crc32(wrong_main_image)},
    });
    upload(wrong_main_id, "spice64.exe", wrong_main_image);
    expect_failure([&] { ota::finish(wrong_main_id); });
    ota::cancel(wrong_main_id);

    const auto bad_crc_id = ota::begin({
        {"spice64.exe", main_image.size(), crc32(main_image) ^ 1u},
    });
    upload(bad_crc_id, "spice64.exe", main_image);
    expect_failure([&] { ota::finish(bad_crc_id); });
    ota::cancel(bad_crc_id);

    const auto bad_config_id = ota::begin({
        {"spice64.exe", main_image.size(), crc32(main_image)},
        {"spicecfg.exe", main_image.size(), crc32(main_image)},
    });
    upload(bad_config_id, "spice64.exe", main_image);
    upload(bad_config_id, "spicecfg.exe", main_image);
    expect_failure([&] { ota::finish(bad_config_id); });
    ota::cancel(bad_config_id);

    std::cout << "OTA update engine tests passed: limits, sequencing, CRC, PE architecture and advisory signatures.\n";
}
