#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#define log_info(...) ((void)0)
#define log_misc(...) ((void)0)
#define log_warning(...) ((void)0)

using DWORD = std::uint32_t;

inline int _strnicmp(const char *left, const char *right, std::size_t count) {
    for (std::size_t index = 0; index < count; index++) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        const auto lower_a = static_cast<unsigned char>(std::tolower(a));
        const auto lower_b = static_cast<unsigned char>(std::tolower(b));
        if (lower_a != lower_b) return lower_a < lower_b ? -1 : 1;
        if (a == '\0' || b == '\0') return 0;
    }
    return 0;
}

namespace avs::game {
    inline char MODEL[4] = {'T', 'S', 'T', '\0'};
    inline bool is_model(const char *model) {
        return std::string(MODEL, 3) == model;
    }
}

namespace games::iidx {
    inline bool TDJ_MODE = false;
}

namespace games::popn {
    inline bool pikapika = false;
    inline bool is_pikapika_model() { return pikapika; }
}

inline int keypad_count = 2;
inline std::uint16_t keypad_override_state[2] {};
inline std::vector<std::uint16_t> keypad_override_history;

inline void eamuse_set_keypad_overrides(size_t unit, std::uint16_t state) {
    if (unit < 2) keypad_override_state[unit] = state;
    keypad_override_history.push_back(state);
}

inline int eamuse_get_game_keypads() {
    return keypad_count;
}

inline std::uint16_t eamuse_get_keypad_state(size_t unit) {
    return unit < 2 ? keypad_override_state[unit] : 0;
}

enum eam_io_keypad_scan_code {
    EAM_IO_KEYPAD_0 = 0,
    EAM_IO_KEYPAD_1 = 1,
    EAM_IO_KEYPAD_4 = 2,
    EAM_IO_KEYPAD_7 = 3,
    EAM_IO_KEYPAD_00 = 4,
    EAM_IO_KEYPAD_2 = 5,
    EAM_IO_KEYPAD_5 = 6,
    EAM_IO_KEYPAD_8 = 7,
    EAM_IO_KEYPAD_DECIMAL = 8,
    EAM_IO_KEYPAD_3 = 9,
    EAM_IO_KEYPAD_6 = 10,
    EAM_IO_KEYPAD_9 = 11,
    EAM_IO_INSERT = 13,
};
