#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <jpeglib.h>

#include "hooks/graphics/jpeg_encoder.h"
#include "../stream_format.h"

namespace {

struct DecodeError {
    jpeg_error_mgr manager {};
    std::jmp_buf jump {};
};

void decode_error(j_common_ptr info) {
    auto *error = reinterpret_cast<DecodeError *>(info->err);
    longjmp(error->jump, 1);
}

struct DecodedImage {
    int width = 0;
    int height = 0;
    int components = 0;
    std::vector<uint8_t> pixels;
};

bool decode(const std::vector<uint8_t> &jpeg, DecodedImage &image) {
    if (jpeg.empty()) {
        return false;
    }

    jpeg_decompress_struct info {};
    DecodeError error;
    info.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = decode_error;

    bool created = false;
    if (setjmp(error.jump) != 0) {
        if (created) {
            jpeg_destroy_decompress(&info);
        }
        return false;
    }

    jpeg_create_decompress(&info);
    created = true;
    jpeg_mem_src(&info, jpeg.data(), jpeg.size());
    if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&info);
        return false;
    }
    info.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&info)) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    image.width = static_cast<int>(info.output_width);
    image.height = static_cast<int>(info.output_height);
    image.components = static_cast<int>(info.output_components);
    const auto size = static_cast<size_t>(image.width)
        * static_cast<size_t>(image.height)
        * static_cast<size_t>(image.components);
    image.pixels.resize(size);
    while (info.output_scanline < info.output_height) {
        auto *row = image.pixels.data()
            + static_cast<size_t>(info.output_scanline)
            * static_cast<size_t>(image.width)
            * static_cast<size_t>(image.components);
        JSAMPROW rows[] = { row };
        if (jpeg_read_scanlines(&info, rows, 1) != 1) {
            jpeg_destroy_decompress(&info);
            return false;
        }
    }
    const bool finished = jpeg_finish_decompress(&info) != 0;
    jpeg_destroy_decompress(&info);
    return finished;
}

bool has_dimensions(const std::vector<uint8_t> &jpeg, int width, int height) {
    for (size_t index = 0; index + 8 < jpeg.size(); ++index) {
        if (jpeg[index] != 0xFF || jpeg[index + 1] != 0xC0) {
            continue;
        }
        const auto marker_height = (static_cast<int>(jpeg[index + 5]) << 8)
                | jpeg[index + 6];
        const auto marker_width = (static_cast<int>(jpeg[index + 7]) << 8)
                | jpeg[index + 8];
        return marker_width == width && marker_height == height;
    }
    return false;
}

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        std::cerr << "JPEG fallback check failed at line " << line
                  << ": " << expression << '\n';
        std::exit(1);
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

api::capture_pump::Frame make_frame(int width, int height) {
    const auto size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    auto pixels = std::shared_ptr<uint8_t[]>(new uint8_t[size]);
    for (size_t index = 0; index < size; index += 3) {
        pixels[index] = 220;
        pixels[index + 1] = 60;
        pixels[index + 2] = 20;
    }

    api::capture_pump::Frame frame;
    frame.pixels = std::move(pixels);
    frame.width = width;
    frame.height = height;
    return frame;
}

using SentChunks = std::vector<std::vector<uint8_t>>;

api::StreamSend collecting_sender(SentChunks &chunks) {
    return [&chunks](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        chunks.emplace_back(bytes, bytes + size);
        return true;
    };
}

void check_mjpeg_stream() {
    const auto formats = api::stream_formats();
    CHECK(formats.size() == 1);
    CHECK(formats.front().first == "mjpeg");
    CHECK(formats.front().second == "/stream.mjpg");

    auto writer = api::make_stream_writer("/stream.mjpg", 80, 30);
    CHECK(writer != nullptr);
    CHECK(api::make_stream_writer("/stream.h264", 80, 30) == nullptr);
    CHECK(api::make_stream_writer("/unknown", 80, 30) == nullptr);
    CHECK(writer->content_type() ==
            "multipart/x-mixed-replace; boundary=spice2xframe");

    const auto frame = make_frame(3, 5);
    SentChunks chunks;
    auto sender = collecting_sender(chunks);
    CHECK(writer->begin(sender));
    CHECK(writer->write(sender, frame));
    CHECK(chunks.size() == 3);

    const std::string header(chunks[0].begin(), chunks[0].end());
    CHECK(header.rfind("--spice2xframe\r\n", 0) == 0);
    const std::string content_length_prefix = "Content-Length: ";
    const auto length_start = header.find(content_length_prefix);
    CHECK(length_start != std::string::npos);
    const auto length_end = header.find("\r\n", length_start);
    CHECK(length_end != std::string::npos);
    const auto content_length = std::stoull(header.substr(
            length_start + content_length_prefix.size(),
            length_end - length_start - content_length_prefix.size()));
    CHECK(content_length == chunks[1].size());
    CHECK(chunks[2] == std::vector<uint8_t>({ '\r', '\n' }));

    DecodedImage decoded;
    CHECK(decode(chunks[1], decoded));
    CHECK(decoded.width == frame.width);
    CHECK(decoded.height == frame.height);
    CHECK(decoded.components == 3);

    auto failing_writer = api::make_stream_writer("/stream.mjpg", 80, 30);
    CHECK(failing_writer != nullptr);
    chunks.clear();
    int sender_calls = 0;
    api::StreamSend failing_sender = [&chunks, &sender_calls](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        chunks.emplace_back(bytes, bytes + size);
        return ++sender_calls < 2;
    };
    CHECK(!failing_writer->write(failing_sender, frame));
    CHECK(sender_calls == 2);
    CHECK(chunks.size() == 2);
}

}

int main() {
    constexpr int width = 3;
    constexpr int height = 5;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            auto *pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 3;
            pixel[0] = 220;
            pixel[1] = 60;
            pixel[2] = 20;
        }
    }

    std::vector<uint8_t> encoded { 0x12, 0x34 };
    CHECK(jpeg_encoder::encode(encoded, pixels.data(), width, height, 100));
    CHECK(encoded.size() > 4);
    CHECK(encoded[2] == 0xFF && encoded[3] == 0xD8);
    CHECK(encoded[encoded.size() - 2] == 0xFF && encoded.back() == 0xD9);

    std::vector<uint8_t> payload(encoded.begin() + 2, encoded.end());
    DecodedImage decoded;
    CHECK(decode(payload, decoded));
    CHECK(decoded.width == width && decoded.height == height && decoded.components == 3);
    int red = 0;
    int green = 0;
    int blue = 0;
    for (size_t index = 0; index < decoded.pixels.size(); index += 3) {
        red += decoded.pixels[index];
        green += decoded.pixels[index + 1];
        blue += decoded.pixels[index + 2];
    }
    CHECK(red > green + 20 * width * height);
    CHECK(red > blue + 20 * width * height);

    for (const int quality : { -20, 101 }) {
        std::vector<uint8_t> bounded;
        CHECK(jpeg_encoder::encode(bounded, pixels.data(), width, height, quality));
        DecodedImage bounded_decoded;
        CHECK(decode(bounded, bounded_decoded));
        CHECK(bounded_decoded.width == width && bounded_decoded.height == height);
    }

    const std::vector<uint8_t> prefix { 0xAB, 0xCD };
    for (const auto &invalid : {
        std::pair<const uint8_t *, std::pair<int, int>> { nullptr, { width, height } },
        std::pair<const uint8_t *, std::pair<int, int>> { pixels.data(), { 0, height } },
        std::pair<const uint8_t *, std::pair<int, int>> { pixels.data(), { width, 0 } },
    }) {
        auto unchanged = prefix;
        CHECK(!jpeg_encoder::encode(
                unchanged, invalid.first, invalid.second.first, invalid.second.second, 80));
        CHECK(unchanged == prefix);
    }

    auto unchanged = prefix;
    CHECK(!jpeg_encoder::encode(
            unchanged,
            reinterpret_cast<const uint8_t *>(1),
            std::numeric_limits<int>::max(),
            std::numeric_limits<int>::max(),
            80));
    CHECK(unchanged == prefix);

    for (const auto [edge_width, edge_height] : {
        std::pair<int, int> { 65535, 1 },
        std::pair<int, int> { 1, 65535 },
    }) {
        const auto edge_size = static_cast<size_t>(edge_width)
                * static_cast<size_t>(edge_height) * 3;
        std::vector<uint8_t> edge_pixels(edge_size, 90);
        std::vector<uint8_t> edge_encoded;
        CHECK(jpeg_encoder::encode(
                edge_encoded, edge_pixels.data(), edge_width, edge_height, 80));
        CHECK(edge_encoded.size() > 4);
        CHECK(edge_encoded[0] == 0xFF && edge_encoded[1] == 0xD8);
        CHECK(edge_encoded[edge_encoded.size() - 2] == 0xFF
                && edge_encoded.back() == 0xD9);
        CHECK(has_dimensions(edge_encoded, edge_width, edge_height));
    }

    for (const auto [too_wide, too_high] : {
        std::pair<int, int> { 65536, 1 },
        std::pair<int, int> { 1, 65536 },
    }) {
        auto unchanged_edge = prefix;
        CHECK(!jpeg_encoder::encode(
                unchanged_edge,
                reinterpret_cast<const uint8_t *>(1), too_wide, too_high, 80));
        CHECK(unchanged_edge == prefix);
    }

    check_mjpeg_stream();

    std::cout << "JPEG fallback and XP MJPEG streaming contracts passed\n";
}
