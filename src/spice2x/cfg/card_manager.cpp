#include "card_manager.h"

#include <cmath>
#include "external/rapidjson/document.h"
#include "external/rapidjson/prettywriter.h"
#include "util/fileutils.h"
#include "util/logging.h"
#include "util/utils.h"

using namespace rapidjson;

namespace cfg {

    CardManagerStore &CardManagerStore::get() {
        static CardManagerStore store;
        return store;
    }

    CardManagerStore::CardManagerStore() {
        bool exists = false;
        path = fileutils::get_config_file_path("cardmanager", "spicetools_card_manager.json", &exists);
        if (!exists) return;

        Document doc;
        const auto text = fileutils::text_read(path);
        doc.Parse(text.c_str());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("pages") || !doc["pages"].IsArray()) {
            load_error = "Unable to read card manager settings.";
            return;
        }
        for (const auto &page : doc["pages"].GetArray()) {
            if (!page.IsObject() || !page.HasMember("cards") || !page["cards"].IsArray()) {
                load_error = "Invalid card manager page.";
                return;
            }
            for (const auto &card : page["cards"].GetArray()) {
                if (!card.IsObject() || !card.HasMember("name") || !card["name"].IsString() ||
                        !card.HasMember("id") || !card["id"].IsString()) {
                    load_error = "Invalid saved card.";
                    return;
                }
                CardEntry entry;
                entry.name.assign(card["name"].GetString(), card["name"].GetStringLength());
                entry.id.assign(card["id"].GetString(), card["id"].GetStringLength());
                uint8_t bytes[8] {};
                if (entry.id.size() != 16 || entry.id.find('\0') != std::string::npos || !hex2bin(entry.id.c_str(), bytes) ||
                        entry.name.find('\0') != std::string::npos) {
                    load_error = "Invalid saved card.";
                    return;
                }
                entry.id = bin2hex(bytes, std::size(bytes));
                entry.search_string = strtolower(entry.name) + " " + strtolower(entry.id);
                if (card.HasMember("color")) {
                    const auto &color = card["color"];
                    if (!color.IsArray() || color.Size() != 3) {
                        load_error = "Invalid saved card color.";
                        return;
                    }
                    for (SizeType i = 0; i < 3; i++) {
                        if (!color[i].IsNumber() || !std::isfinite(color[i].GetDouble()) ||
                                color[i].GetDouble() < 0 || color[i].GetDouble() > 1) {
                            load_error = "Invalid saved card color.";
                            return;
                        }
                        entry.color[i] = color[i].GetFloat();
                    }
                }
                cards.emplace_back(std::move(entry));
            }
        }
    }

    void CardManagerStore::changed() {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        ++revision;
        dirty = true;
    }

    bool CardManagerStore::write(const std::vector<CardEntry> &entries) {
        if (!load_error.empty()) return false;
        Document doc(kObjectType);
        auto &alloc = doc.GetAllocator();
        Value list(kArrayType);
        for (const auto &entry : entries) {
            Value card(kObjectType), color(kArrayType);
            card.AddMember("name", Value(entry.name.c_str(), static_cast<SizeType>(entry.name.size()), alloc), alloc);
            card.AddMember("id", Value(entry.id.c_str(), alloc), alloc);
            for (float channel : entry.color) color.PushBack(channel, alloc);
            card.AddMember("color", color, alloc);
            list.PushBack(card, alloc);
        }
        Value page(kObjectType), pages(kArrayType);
        page.AddMember("cards", list, alloc);
        pages.PushBack(page, alloc);
        doc.AddMember("pages", pages, alloc);
        StringBuffer buffer;
        PrettyWriter<StringBuffer> writer(buffer);
        doc.Accept(writer);
        return fileutils::write_config_file("cardmanager", path, buffer.GetString());
    }

    bool CardManagerStore::save() {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (!write(cards)) return false;
        dirty = false;
        return true;
    }

    bool CardManagerStore::replace(uint64_t expected_revision, std::vector<CardEntry> next, std::string &error) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if (expected_revision != revision) {
            error = "Cards changed; reload before saving.";
            return false;
        }
        if (!load_error.empty()) {
            error = load_error;
            return false;
        }
        if (!write(next)) {
            error = "Unable to save card manager settings.";
            return false;
        }
        cards = std::move(next);
        ++revision;
        dirty = false;
        return true;
    }
}
