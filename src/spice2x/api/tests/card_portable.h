#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include "cfg/option.h"
#include "external/fmt/include/fmt/format.h"

#define log_info(...) ((void)0)
#define log_misc(...) ((void)0)
#define log_warning(...) ((void)0)

inline bool card_write_failure = false;
inline int card_insert_count = 0;
inline unsigned card_insert_reader = 0;
inline uint8_t card_insert_data[8] {};
inline std::mutex CARD_OVERRIDES_LOCK;
inline std::string CARD_OVERRIDES[2];
inline std::unique_ptr<std::vector<Option>> LAUNCHER_OPTIONS;

namespace launcher::Options {
    enum {
        Player1Card = 6,
        Player2Card = 7,
    };
}

inline std::string strtolower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
    return value;
}

// Hex helpers are extracted from the production utility header by the runner.
#include "card_hex.h"

inline int eamuse_get_game_keypads() { return 2; }
inline std::string eamuse_get_card_override(size_t index) {
    std::lock_guard<std::mutex> lock(CARD_OVERRIDES_LOCK);
    return CARD_OVERRIDES[index];
}
inline std::filesystem::path eamuse_get_card_path(size_t index) {
    return index == 0 ? "card0.txt" : "card1.txt";
}
inline void eamuse_card_insert(unsigned reader, const uint8_t *bytes) {
    ++card_insert_count;
    card_insert_reader = reader;
    std::copy_n(bytes, 8, card_insert_data);
}

namespace fileutils {
    inline std::filesystem::path get_config_file_path(const std::string &, const std::string &name, bool *exists) {
        if (exists) *exists = std::filesystem::exists(name);
        return name;
    }
    inline std::string text_read(const std::filesystem::path &path) {
        std::ifstream input(path);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    inline bool write_config_file(const std::string &, const std::filesystem::path &path, const std::string &text) {
        if (card_write_failure) return false;
        auto temp = path;
        temp += ".tmp";
        std::ofstream output(temp, std::ios::binary);
        output << text;
        output.close();
        if (!output) return false;
        std::filesystem::rename(temp, path);
        return true;
    }
}
