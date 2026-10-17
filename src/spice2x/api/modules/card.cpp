#include "card.h"
#include <fstream>
#include <functional>
#include <cmath>
#include "cfg/card_manager.h"
#include "external/rapidjson/document.h"
#include "launcher/launcher.h"
#include "launcher/options.h"
#include "util/logging.h"
#include "util/utils.h"
#include "misc/eamuse.h"

using namespace std::placeholders;
using namespace rapidjson;


namespace api::modules {

    static bool normalize_card_id(const std::string &value, std::string &card_id) {
        if (value.size() != 16 || value.find('\0') != std::string::npos) {
            return false;
        }

        uint8_t card_bin[8] {};
        if (!hex2bin(value.c_str(), card_bin)) {
            return false;
        }

        card_id = bin2hex(card_bin, std::size(card_bin));
        return true;
    }

    static bool read_card_id(const std::filesystem::path &path, std::string &card_id) {
        std::ifstream file(path);
        char buffer[16] {};
        if (!file.read(buffer, std::size(buffer))) {
            return false;
        }

        return normalize_card_id(std::string(buffer, std::size(buffer)), card_id);
    }

    // CARD_OVERRIDES is mutable by manager_set_active; launcher options retain startup CLI values.
    static std::string launcher_card_override(size_t index) {
        if (!LAUNCHER_OPTIONS || index > 1) return {};
        const auto option_index = index == 0
                ? launcher::Options::Player1Card
                : launcher::Options::Player2Card;
        if (LAUNCHER_OPTIONS->size() <= static_cast<size_t>(option_index)) return {};
        const auto &option = LAUNCHER_OPTIONS->at(option_index);
        std::string card_id;
        return option.is_active() && normalize_card_id(option.value, card_id)
                ? card_id
                : std::string();
    }

    Card::Card() : Module("card") {
        functions["get_cards"] = std::bind(&Card::get_cards, this, _1, _2);
        functions["insert"] = std::bind(&Card::insert, this, _1, _2);
        functions["manager_get"] = std::bind(&Card::manager_get, this, _1, _2);
        functions["manager_save"] = std::bind(&Card::manager_save, this, _1, _2);
        functions["manager_set_active"] = std::bind(&Card::manager_set_active, this, _1, _2);
        require_password("get_cards");
        require_password("manager_get");
        require_password("manager_save");
        require_password("manager_set_active");
    }

    void Card::add_manager_state(Response &res) {
        auto &store = cfg::CardManagerStore::get();
        std::lock_guard<std::recursive_mutex> lock(store.mutex);
        if (!store.load_error.empty()) return error(res, store.load_error);
        auto &alloc = res.doc()->GetAllocator();
        Value state(kObjectType), cards(kArrayType), active(kArrayType), read_only(kArrayType);
        state.AddMember("revision", store.revision, alloc);
        for (const auto &entry : store.cards) {
            Value card(kObjectType), color(kArrayType);
            card.AddMember("name", Value(entry.name.c_str(), alloc), alloc);
            card.AddMember("id", Value(entry.id.c_str(), alloc), alloc);
            for (float channel : entry.color) color.PushBack(channel, alloc);
            card.AddMember("color", color, alloc);
            cards.PushBack(card, alloc);
        }
        {
            std::lock_guard<std::mutex> overrides_lock(CARD_OVERRIDES_LOCK);
            const auto keypad_count = eamuse_get_game_keypads();
            for (size_t index = 0; index < std::size(CARD_OVERRIDES); index++) {
                const auto &id = CARD_OVERRIDES[index];
                active.PushBack(Value(index < static_cast<size_t>(keypad_count) ? id.c_str() : "", alloc), alloc);
                const auto startup_id = launcher_card_override(index);
                if (index >= static_cast<size_t>(keypad_count) || startup_id.empty()) {
                    continue;
                }
                Value card(kObjectType);
                const auto name = std::string("P") + std::to_string(index + 1)
                        + " Default (-card" + std::to_string(index) + ")";
                const auto source = std::string("-card") + std::to_string(index);
                card.AddMember("name", Value(name.c_str(), alloc), alloc);
                card.AddMember("id", Value(startup_id.c_str(), alloc), alloc);
                card.AddMember("source", Value(source.c_str(), alloc), alloc);
                read_only.PushBack(card, alloc);
            }
        }
        state.AddMember("cards", cards, alloc);
        state.AddMember("active", active, alloc);
        state.AddMember("read_only", read_only, alloc);
        res.add_data(state);
    }

    void Card::manager_get(Request &req, Response &res) {
        if (!req.params.Empty()) return error(res, "manager_get takes no parameters.");
        add_manager_state(res);
    }

    void Card::manager_save(Request &req, Response &res) {
        if (req.params.Size() != 2 || !req.params[0].IsUint64() || !req.params[1].IsArray()) {
            return error(res, "Expected revision and cards array.");
        }
        if (req.params[1].Size() > 4096) return error(res, "At most 4096 cards can be saved per request.");
        std::vector<cfg::CardEntry> cards;
        for (const auto &value : req.params[1].GetArray()) {
            if (!value.IsObject() || value.MemberCount() != 3 ||
                    !value.HasMember("name") || !value["name"].IsString() || value["name"].GetStringLength() > 256 ||
                    !value.HasMember("id") || !value["id"].IsString() ||
                    !value.HasMember("color") || !value["color"].IsArray() || value["color"].Size() != 3) {
                return error(res, "Expected card name, 16-character id and RGB color.");
            }
            cfg::CardEntry entry;
            entry.name.assign(value["name"].GetString(), value["name"].GetStringLength());
            const std::string id(value["id"].GetString(), value["id"].GetStringLength());
            if (entry.name.find('\0') != std::string::npos || !normalize_card_id(id, entry.id)) {
                return error(res, "Invalid card id/name.");
            }
            for (SizeType i = 0; i < 3; i++) {
                const auto &channel = value["color"][i];
                if (!channel.IsNumber() || !std::isfinite(channel.GetDouble()) ||
                        channel.GetDouble() < 0 || channel.GetDouble() > 1) {
                    return error(res, "Card color must contain three numbers from 0 to 1.");
                }
                entry.color[i] = channel.GetFloat();
            }
            entry.search_string = strtolower(entry.name) + " " + strtolower(entry.id);
            cards.emplace_back(std::move(entry));
        }
        std::string failure;
        auto &store = cfg::CardManagerStore::get();
        std::lock_guard<std::recursive_mutex> lock(store.mutex);
        if (!store.replace(req.params[0].GetUint64(), std::move(cards), failure)) return error(res, failure);
        add_manager_state(res);
    }

    void Card::manager_set_active(Request &req, Response &res) {
        if (req.params.Size() != 2 || !req.params[0].IsUint() || req.params[0].GetUint() > 1 ||
                req.params[0].GetUint() >= static_cast<unsigned>(eamuse_get_game_keypads()) ||
                !req.params[1].IsString()) {
            return error(res, "Expected reader 0 or 1 and a card id (empty clears the override).");
        }
        const std::string value(req.params[1].GetString(), req.params[1].GetStringLength());
        std::string id;
        if (!value.empty() && !normalize_card_id(value, id)) return error(res, "Invalid card id.");
        auto &store = cfg::CardManagerStore::get();
        std::lock_guard<std::recursive_mutex> lock(store.mutex);
        if (!store.load_error.empty()) return error(res, store.load_error);
        {
            std::lock_guard<std::mutex> overrides_lock(CARD_OVERRIDES_LOCK);
            CARD_OVERRIDES[req.params[0].GetUint()] = id;
        }
        add_manager_state(res);
    }

    /**
     * get_cards()
     */
    void Card::get_cards(Request &req, Response &res) {
        auto &alloc = res.doc()->GetAllocator();

        for (int index = 0; index < eamuse_get_game_keypads(); index++) {
            std::string card_id;
            std::string filename;
            const auto card_override = eamuse_get_card_override(index);
            const bool has_override = !card_override.empty();

            if (has_override) {
                if (!normalize_card_id(card_override, card_id)) {
                    continue;
                }
            } else {
                const auto path = eamuse_get_card_path(index);
                if (!read_card_id(path, card_id)) {
                    continue;
                }

                const auto filename_u8 = path.filename().u8string();
                filename.assign(filename_u8.begin(), filename_u8.end());
            }

            Value card(kObjectType);
            card.AddMember("index", index, alloc);
            card.AddMember("card_id", Value(card_id.c_str(), alloc), alloc);
            card.AddMember("source", Value(has_override ? "override" : "file", alloc), alloc);
            if (!has_override) {
                card.AddMember("file_name", Value(filename.c_str(), alloc), alloc);
            }
            res.add_data(card);
        }
    }

    /**
     * insert(index, card_id)
     * index: uint in range [0, 1]
     * card_id: hex string of length 16
     */
    void Card::insert(Request &req, Response &res) {

        // check params
        if (req.params.Size() < 2)
            return error_params_insufficient(res);
        if (!req.params[0].IsUint())
            return error_type(res, "index", "uint");
        if (!req.params[1].IsString())
            return error_type(res, "card_id", "hex string");
        if (req.params[1].GetStringLength() != 16)
            return error_size(res, "card_id", 16);

        // get params
        auto index = req.params[0].GetUint();
        std::string card_hex;
        if (!normalize_card_id(std::string(req.params[1].GetString(), req.params[1].GetStringLength()), card_hex)) {
            return error_type(res, "card_id", "hex string");
        }

        // convert to binary
        uint8_t card_bin[8] {};
        if (!hex2bin(card_hex.c_str(), card_bin)) {
            return error_type(res, "card_id", "hex string");
        }

        // log
        if (LOGGING) {
            log_info("api::card", "inserting card: {}", card_hex);
        }

        // insert card
        eamuse_card_insert(index & 1, card_bin);
    }
}
