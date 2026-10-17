#pragma once

#include <filesystem>
#include <optional>

#include "overlay/window.h"
#include "cfg/card_manager.h"

namespace overlay::windows {

    using CardEntry = cfg::CardEntry;

    class CardManager : public Window {
    public:

        CardManager(SpiceOverlay *overlay);
        ~CardManager() override;

        void build_content() override;

    private:

        cfg::CardManagerStore &store = cfg::CardManagerStore::get();
        bool &config_dirty = store.dirty;
        std::vector<CardEntry> &cards = store.cards;
        uint64_t seen_revision = 0;
        bool editor_stale = false;
        char name_buffer[65] {};
        char card_buffer[17] {};
        float color_buffer[3] {};
        
        std::optional<CardEntry> card_cmd_overrides[2];

        std::optional<CardEntry> loaded_card[2];
        CardEntry *current_card = nullptr;

        std::string search_filter = "";
        std::string search_filter_in_lower_case = "";

        void config_save();

        void generate_search_string(CardEntry *card);
        void generate_random_color();

        bool build_card(int reader);
        void open_card_editor();
        void build_card_editor();
        void build_card_list();
        void build_card_selectable(CardEntry &card);
        void build_footer();

        void insert_card_over_api(int reader, CardEntry &card);
    };
}
