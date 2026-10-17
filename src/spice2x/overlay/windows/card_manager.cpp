#include <random>
#include <algorithm>
#include <games/io.h>
#include "card_manager.h"

#include "misc/eamuse.h"
#include "misc/clipboard.h"
#include "util/utils.h"
#include "util/fileutils.h"
#include "cfg/configurator.h"
#include "overlay/imgui/extensions.h"

namespace overlay::windows {

    CardManager::CardManager(SpiceOverlay *overlay) : Window(overlay) {
        this->title = "Card Manager";
        this->init_size = overlay::apply_scaling_to_vector(ImVec2(420, 420));

        if (cfg::CONFIGURATOR_STANDALONE) {
            this->init_pos = ImVec2(40, 40);
        } else {
            this->init_pos = ImVec2(
                    ImGui::GetIO().DisplaySize.x / 2 - this->init_size.x / 2,
                    ImGui::GetIO().DisplaySize.y / 2 - this->init_size.y / 2);
        }

        this->toggle_button = games::OverlayButtons::ToggleCardManager;

        // load -card0 / -card1
        // -card0 / -card1 override
        {
            std::lock_guard<std::mutex> lock(CARD_OVERRIDES_LOCK);
            if (!CARD_OVERRIDES[0].empty()) {
                const CardEntry card0 = {
                    .name = "P1 Default (-card0)",
                    .id = CARD_OVERRIDES[0],
                    .search_string = "p1 default (-card0)",
                    .read_only = true,
                    .color = {0.9f, 0.9f, 0.9f}
                };
                card_cmd_overrides[0].emplace(card0);
                this->loaded_card[0] = card0;
            }
            if (eamuse_get_game_keypads() > 1 && !CARD_OVERRIDES[1].empty()) {
                const CardEntry card1 = {
                    .name = "P2 Default (-card1)",
                    .id = CARD_OVERRIDES[1],
                    .search_string = "p2 default (-card1)",
                    .read_only = true,
                    .color = {0.9f, 0.9f, 0.9f}
                };
                card_cmd_overrides[1].emplace(card1);
                this->loaded_card[1] = card1;
            }
        }
    }

    CardManager::~CardManager() {
    }

    void CardManager::build_content() {
        std::lock_guard<std::recursive_mutex> lock(store.mutex);
        if (!store.load_error.empty()) {
            ImGui::TextUnformatted(store.load_error.c_str());
            return;
        }
        const bool cards_changed = seen_revision != store.revision;
        if (cards_changed) {
            current_card = nullptr;
            editor_stale = true;
            seen_revision = store.revision;
        }
        for (size_t i = 0; i < 2; i++) {
            const auto id = eamuse_get_card_override(i);
            if (id.empty()) {
                loaded_card[i].reset();
                continue;
            }
            if (!cards_changed && loaded_card[i] && loaded_card[i]->id == id) continue;
            const auto entry = std::find_if(cards.begin(), cards.end(), [&](const auto &card) { return card.id == id; });
            if (entry != cards.end()) loaded_card[i] = *entry;
            else if (!loaded_card[i] || loaded_card[i]->id != id) {
                loaded_card[i] = CardEntry { .name = "Override", .id = id, .read_only = true, .color = {0.9f, 0.9f, 0.9f} };
            }
        }
        ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "Active card overrides");
        ImGui::SameLine();
        ImGui::HelpMarker(
            "Click to insert card now, or press Insert Card key. Auto Card Insert will also use these cards.\n\n"
            "If no override is set, pressing Insert Card will read from card0.txt / card1.txt.");
        if (ImGui::BeginTable("CardSetTable", eamuse_get_game_keypads() > 1 ? 2 : 1, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Player 1");
            if (eamuse_get_game_keypads() > 1) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Player 2");
            }
            ImGui::TableNextRow();
            for (size_t i = 0; i < 2; i++) {
                if (eamuse_get_game_keypads() > (int)i) {
                    ImGui::TableNextColumn();
                    if (build_card(i) && this->loaded_card[i].has_value()) {
                        insert_card_over_api(i, this->loaded_card[i].value());
                    }
                }
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1, 0.7f, 0, 1), "Available cards");
        build_card_list();
        ImGui::Separator();
        ImGui::Spacing();
        build_footer();

        build_card_editor();
        seen_revision = store.revision;
    }

    bool CardManager::build_card(int reader) {
        ImGui::PushID(reader);
        bool clicked = false;
        if (this->loaded_card[reader].has_value()) {
            const auto &card = this->loaded_card[reader].value();
            const ImVec4 color(card.color[0], card.color[1], card.color[2], 1.f);
            float bg_luminance = (0.299f * card.color[0] + 0.587 * card.color[1] + 0.114 * card.color[2]);

            // text color
            ImVec4 text_color;
            if (0.5f < bg_luminance) {
                text_color = ImVec4(0.f, 0.f, 0.f, 1.f); // black
            } else {
                text_color = ImVec4(1.f, 1.f, 1.f, 1.f); // white
            }

            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
            ImGui::PushStyleColor(ImGuiCol_Text, text_color);
            if (ImGui::Button(fmt::format(
                    "  {}  \n  {} {} {} {}  ",
                    card.name.empty() ? "<blank>" : card.name.substr(0, 19),
                    card.id.substr(0, 4).c_str(),
                    card.id.substr(4, 4).c_str(),
                    card.id.substr(8, 4).c_str(),
                    card.id.substr(12, 4).c_str()
                    ).c_str())) {

                clicked = true;
            }
            ImGui::PopStyleColor(4);
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("   (No override set)   \n"
                          "  xxxx xxxx xxxx xxxx  ");
            ImGui::EndDisabled();
        }

        ImGui::PopID();
        return clicked;
    }

    void CardManager::open_card_editor() {
        editor_stale = false;
        if (this->current_card) {
            const auto card = this->current_card;
            snprintf(this->name_buffer, std::size(this->name_buffer), "%s", card->name.c_str());
            snprintf(this->card_buffer, std::size(this->card_buffer), "%s", card->id.c_str());
            this->color_buffer[0] = card->color[0];
            this->color_buffer[1] = card->color[1];
            this->color_buffer[2] = card->color[2];
            ImGui::OpenPopup("Card Editor");
        }
    }

    void CardManager::build_card_editor() {
        // new/edit card popup
        if (ImGui::BeginPopupModal("Card Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (editor_stale) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            // card ID field (only editable for new cards)
            ImGui::BeginDisabled(this->current_card);
            ImGui::InputTextWithHint("Card ID", "E0040100FFFFFFFF",
                    this->card_buffer,
                    std::size(this->card_buffer),
                    ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
            ImGui::EndDisabled();
            if (this->current_card == nullptr) {
                ImGui::SameLine();
                if (ImGui::Button("Generate")) {
                    generate_ea_card(this->card_buffer);
                }
            } else {
                ImGui::SameLine();
                if (ImGui::Button("Copy")) {
                    clipboard::copy_text(this->current_card->id);
                }
            }

            // name field
            ImGui::InputTextWithHint("Card Name", "Main Card",
                    this->name_buffer, std::size(this->name_buffer));

            // color
            ImGui::ColorEdit3("Color", this->color_buffer, ImGuiColorEditFlags_DisplayHex);
            ImGui::SameLine();
            if (ImGui::Button("Random")) {
                generate_random_color();
            }

            ImGui::Separator();

            // add/update button
            ImGui::BeginDisabled(strlen(this->card_buffer) != 16);
            if (ImGui::Button(this->current_card ? "Update Card" : "Save Card")) {
                if (this->current_card) {
                    // update existing card
                    this->current_card->name = strtrim(this->name_buffer);
                    this->current_card->color[0] = this->color_buffer[0];
                    this->current_card->color[1] = this->color_buffer[1];
                    this->current_card->color[2] = this->color_buffer[2];
                    generate_search_string(this->current_card);

                    // ensure loaded cards are kept up to date
                    // note: does not handle cases where multiple cards have the same ID
                    for (size_t i = 0; i < 2; i++) {
                        if (this->loaded_card[i].has_value() &&
                            !this->loaded_card[i].value().read_only &&
                            this->loaded_card[i].value().id == this->current_card->id) {
                            this->loaded_card[i] = *this->current_card;
                        }
                    }

                } else {
                    // create a new card
                    CardEntry card {
                        .name = strtrim(this->name_buffer),
                        .id = std::string(this->card_buffer),
                        .color = {this->color_buffer[0], this->color_buffer[1], this->color_buffer[2]}
                    };
                    generate_search_string(&card);
                    this->cards.emplace_back(card);

                    // mark this card as the selected one
                    this->current_card = &this->cards.back();
                }
                this->config_save();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();

            // delete current card button
            if (this->current_card) {
                ImGui::SameLine();
                if (ImGui::Button("Delete Card")) {
                    std::erase_if(this->cards, [&](CardEntry &card) {
                        return &card == this->current_card;
                    });
                    this->current_card = nullptr;
                    this->config_save();
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void CardManager::build_card_selectable(CardEntry &card) {
        // generate card name
        std::string card_name = "";
        if (card.id.length() == 16) {
            card_name += card.id.substr(0, 4);
            card_name += " ";
            card_name += card.id.substr(4, 4);
            card_name += " ";
            card_name += card.id.substr(8, 4);
            card_name += " ";
            card_name += card.id.substr(12, 4);
        } else {
            card_name += card.id;
        }
        if (!card.name.empty()) {
            card_name += " - ";
            card_name += card.name;
        }

        ImGui::PushID(&card);

        // color button
        ImVec4 color(card.color[0], card.color[1], card.color[2], 1.f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
        ImGui::SmallButton(" ");
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        // selectable item
        if (ImGui::Selectable(card_name.c_str(), this->current_card == &card)) {
            this->current_card = &card;
        }

        ImGui::PopID();
    }

    void CardManager::build_card_list() {

        // search for card
        //
        // setting ImGuiInputTextFlags_CallbackCharFilter and pressing escape doesn't cause below
        // to return true, making it necessary to provide a callback...
        ImGui::SetNextItemWidth(overlay::apply_scaling(240));
        if (ImGui::InputTextWithHint("", "Type here to search..", &this->search_filter)) {
            this->current_card = nullptr;
            this->search_filter_in_lower_case = strtolower(this->search_filter);
        }
        if (!this->search_filter.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                this->search_filter.clear();
                this->search_filter_in_lower_case.clear();
            }
        }

        // toolbar

        // set card as p1/p2
        for (size_t i = 0; i < 2; i++) {
            if (eamuse_get_game_keypads() > (int)i) {
                if (i != 0) {
                    ImGui::SameLine();
                }
                ImGui::PushID(i);
                ImGui::BeginDisabled(this->current_card == nullptr);
                if (ImGui::Button(i == 0 ? "Load P1" : "Load P2") && this->current_card) {
                    this->loaded_card[i] = *this->current_card;
                    log_info(
                        "cardmanager",
                        "[P{}] update override and insert card: {} ({})",
                        i+1,
                        this->current_card->id,
                        this->current_card->name
                    );

                    // update override
                    std::lock_guard<std::mutex> lock(CARD_OVERRIDES_LOCK);
                    CARD_OVERRIDES[i] = this->current_card->id;

                    // insert card over api
                    insert_card_over_api(i, this->loaded_card[i].value());
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        // edit selected card
        ImGui::SameLine();
        ImGui::BeginDisabled(this->current_card == nullptr || this->current_card->read_only);
        if (ImGui::Button("Edit")) {
            open_card_editor();
        }

        // move selected up/down the list
        if (this->search_filter.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Move Up")) {
                for (auto it = this->cards.begin(); it != this->cards.end(); ++it) {
                    if (&*it == this->current_card && it != this->cards.begin()) {
                        std::iter_swap(it, it - 1);
                        this->current_card = &*(it - 1);
                        store.changed();
                        break;
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Move Down")) {
                for (auto it = this->cards.begin(); it != this->cards.end(); ++it) {
                    if (&*it == this->current_card && (it + 1) != this->cards.end()) {
                        std::iter_swap(it, it + 1);
                        this->current_card = &*(it + 1);
                        store.changed();
                        break;
                    }
                }
            }
        }
        ImGui::EndDisabled();

        ImGui::Spacing();

        // cards list
        // use all available vertical space, minus the footer that follows in
        // build_content(): a separator + spacing, then a row of buttons
        const ImGuiStyle &style = ImGui::GetStyle();
        const float footer_height =
            style.ItemSpacing.y * 3.f     // spacing around the separator and before the footer
            + 1.f                         // separator line
            + ImGui::GetFrameHeightWithSpacing(); // footer button row
        if (ImGui::BeginChild(
                "cards",
                ImVec2(0, ImGui::GetContentRegionAvail().y - footer_height))) {

            // extra vertical padding between card rows
            const ImVec2 item_spacing = ImGui::GetStyle().ItemSpacing;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                    ImVec2(item_spacing.x, item_spacing.y + overlay::apply_scaling(2)));

            // -card0 / -card1 override
            for (size_t i = 0; i < 2; i++) {
                if (card_cmd_overrides[i].has_value()) {
                    build_card_selectable(card_cmd_overrides[i].value());
                }
            }

            // cards from card manager JSON
            for (auto &card : this->cards) {

                if (!this->search_filter_in_lower_case.empty() && !card.search_string.empty()) {
                    const bool matched =
                    card.search_string.find(this->search_filter_in_lower_case) != std::string::npos;

                    if (!matched) {
                        continue;
                    }
                }

                build_card_selectable(card);
            }

            ImGui::PopStyleVar(); // ImGuiStyleVar_ItemSpacing
        }
        ImGui::EndChild();
    }

    void CardManager::build_footer() {

        // add new card
        if (ImGui::Button("Add New Card")) {
            editor_stale = false;
            memset(this->name_buffer, 0, sizeof(this->name_buffer));
            memset(this->card_buffer, 0, sizeof(this->card_buffer));
            generate_random_color();
            this->current_card = nullptr;
            ImGui::OpenPopup("Card Editor");
        }

        // save button
        if (this->config_dirty) {
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                this->config_save();
            }
        }
    }

    void CardManager::config_save() {
        store.changed();
        if (!store.save()) log_warning("cardmanager", "unable to save config file");
    }

    void CardManager::generate_search_string(CardEntry *card) {
        card->search_string = strtolower(card->name) + " " + strtolower(card->id);
    }

    void CardManager::generate_random_color() {
        // these are colors on a hue wheel, starting from red
        static const char colors[48][7] = {
            "FF0000","FF2000","FF4000","FF6000","FF8000","FFAA00","FFCC00","FFEE00",
            "FFFF00","DDFF00","CCFF00","AAFF00","80FF00","60FF00","40FF00","20FF00",
            "00FF00","00FF20","00FF40","00FF60","00FF80","00FFAA","00FFCC","00FFDD",
            "00FFFF","00DDFF","00CCFF","0099FF","0080FF","0060FF","0040FF","0020FF",
            "0000FF","2000FF","4000FF","6000FF","8000FF","AA00FF","CC00FF","DD00FF",
            "FF00FF","FF00EE","FF00CC","FF00AA","FF0080","FF0060","FF0040","FF0020"
            };

        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> uniform(12, 36);

        // skip, ignoring half the hue wheel close to current index
        static int index = 0;
        index = (index + uniform(generator)) % 48;

        const auto hex = colors[index];

        uint8_t r, g, b;
        std::sscanf(hex, "%02hhx%02hhx%02hhx", &r, &g, &b);
        this->color_buffer[0] = r / 255.f;
        this->color_buffer[1] = g / 255.f;
        this->color_buffer[2] = b / 255.f;
    }

    void CardManager::insert_card_over_api(int reader, CardEntry &card) {
        uint8_t card_bin[8];
        if (card.id.length() == 16 && hex2bin(card.id.c_str(), card_bin)) {
            eamuse_card_insert(reader, card_bin);
        }
    }
}
