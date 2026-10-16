#pragma once

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#include "external/fmt/include/fmt/format.h"

template<typename... Args>
inline void resize_test_log(const Args &...) {
}

#define log_info(...) resize_test_log(__VA_ARGS__)
#define log_misc(...) resize_test_log(__VA_ARGS__)
#define log_warning(...) resize_test_log(__VA_ARGS__)

inline bool resize_test_write_failure = false;
inline std::mutex resize_test_io_mutex;
inline std::condition_variable resize_test_io_cv;
inline bool resize_test_block_write = false;
inline bool resize_test_write_started = false;
inline int resize_test_write_count = 0;

inline std::string eamuse_get_game() {
    return "Resize API Test";
}

namespace fileutils {
    inline bool file_exists(const std::filesystem::path &path) {
        return std::filesystem::exists(path);
    }

    inline std::filesystem::path get_config_file_path(
            const std::string &, const std::string &name, bool *exists) {
        *exists = file_exists(name);
        return name;
    }

    inline std::string text_read(const std::filesystem::path &path) {
        std::ifstream stream(path);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }

    inline bool write_config_file(const std::string &,
            const std::filesystem::path &path, const std::string &contents) {
        if (resize_test_write_failure) {
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(resize_test_io_mutex);
            ++resize_test_write_count;
            resize_test_io_cv.notify_all();
            if (resize_test_block_write) {
                resize_test_write_started = true;
                resize_test_io_cv.notify_all();
                resize_test_io_cv.wait(lock, [] { return !resize_test_block_write; });
            }
        }
        std::ofstream stream(path);
        stream << contents;
        return stream.good();
    }
}
