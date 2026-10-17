#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cfg {

    struct CardEntry {
        std::string name = "unnamed";
        std::string id = "E0040100FFFFFFFF";
        std::string search_string = "";
        bool read_only = false;
        float color[3] {};
    };

    class CardManagerStore {
    public:
        static CardManagerStore &get();

        std::recursive_mutex mutex;
        std::vector<CardEntry> cards;
        uint64_t revision = 0;
        bool dirty = false;
        std::string load_error;

        void changed();
        bool save();
        bool replace(uint64_t expected_revision, std::vector<CardEntry> next, std::string &error);

    private:
        CardManagerStore();
        std::filesystem::path path;
        bool write(const std::vector<CardEntry> &entries);
    };
}
