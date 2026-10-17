#include "card_portable.h"
#include "api/modules/card.h"
#include "cfg/card_manager.h"
#include "external/rapidjson/document.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

using namespace rapidjson;

static Document call(api::modules::Card &module, const std::string &function, const std::string &params = "[]") {
    Document input;
    const auto json = "{\"id\":1,\"module\":\"card\",\"function\":\"" + function + "\",\"params\":" + params + "}";
    input.Parse(json.c_str());
    assert(!input.HasParseError());
    api::Request request(input);
    api::Response response(request.id);
    module.handle(request, response);
    Document output;
    output.Parse(response.get_string().c_str());
    assert(!output.HasParseError());
    return output;
}

static const std::string card_a = "{\"name\":\"Player A\",\"id\":\"e004010000000001\",\"color\":[0,0.5,1]}";
static const std::string card_b = "{\"name\":\"Player B\",\"id\":\"E004010000000002\",\"color\":[1,0,0]}";
static constexpr char command_line_card[] = "E004010000000099";
static const std::string path = "spicetools_card_manager.json";

static void expect_error(const Document &result) {
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "legacy") {
        std::ofstream(path) << "{\"pages\":[{\"cards\":[" << card_a << "]},{\"cards\":[{\"name\":\"第二張\",\"id\":\"E004010000000002\"}]}]}";
    } else if (mode == "invalid") {
        std::ofstream(path) << "{truncated";
    } else if (mode == "invalid-color") {
        std::ofstream(path) << "{\"pages\":[{\"cards\":[{\"name\":\"Bad\",\"id\":\"E004010000000001\",\"color\":[0,\"x\",1]}]}]}";
    } else if (mode == "invalid-id") {
        std::ofstream(path) << "{\"pages\":[{\"cards\":[{\"name\":\"Bad\",\"id\":\"E004\\u000001000000000\"}]}]}";
    }
    if (!mode.starts_with("invalid") || mode == "invalid-startup") {
        LAUNCHER_OPTIONS = std::make_unique<std::vector<Option>>();
        for (size_t index = 0; index <= launcher::Options::Player2Card; index++) {
            LAUNCHER_OPTIONS->emplace_back(OptionDefinition {}, "");
        }
        const auto startup_card = mode == "invalid-startup" ? "not-a-card" : command_line_card;
        (*LAUNCHER_OPTIONS)[launcher::Options::Player1Card].value = startup_card;
        CARD_OVERRIDES[0] = startup_card;
    }
    api::modules::Card module;
    assert(module.requires_password("get_cards"));
    assert(module.requires_password("manager_get"));
    assert(module.requires_password("manager_save"));
    assert(module.requires_password("manager_set_active"));
    assert(!module.requires_password("insert"));

    if (mode.starts_with("invalid") && mode != "invalid-startup") {
        const auto before = fileutils::text_read(path);
        expect_error(call(module, "manager_get"));
        expect_error(call(module, "manager_save", "[0,[" + card_a + "]]"));
        expect_error(call(module, "manager_set_active", "[0,\"E004010000000001\"]"));
        assert(fileutils::text_read(path) == before);
        std::cout << "Card malformed-file preservation passed: " << mode << '\n';
        return 0;
    }
    auto initial = call(module, "manager_get");
    assert(initial["errors"].Empty());
    if (mode == "invalid-startup") {
        assert(initial["data"][0]["read_only"].Empty());
        std::cout << "Invalid startup card is omitted from read-only state\n";
        return 0;
    }
    assert(initial["data"][0]["read_only"].Size() == 1);
    assert(std::string(initial["data"][0]["read_only"][0]["source"].GetString()) == "-card0");
    assert(std::string(initial["data"][0]["read_only"][0]["id"].GetString()) == command_line_card);
    assert(initial["data"][0]["cards"].Size() == (mode == "legacy" ? 2u : 0u));
    if (mode == "legacy") {
        assert(std::string(initial["data"][0]["cards"][0]["id"].GetString()) == "E004010000000001");
        assert(initial["data"][0]["cards"][0]["color"][1].GetDouble() == 0.5);
    }
    auto selected_saved = call(module, "manager_set_active", "[0,\"e004010000000001\"]");
    assert(selected_saved["errors"].Empty());
    assert(std::string(selected_saved["data"][0]["active"][0].GetString())
            == "E004010000000001");
    assert(std::string(selected_saved["data"][0]["read_only"][0]["id"].GetString())
            == command_line_card);
    expect_error(call(module, "manager_get", "[0]"));
    auto saved = call(module, "manager_save", "[0,[" + card_a + "," + card_b + "]]" );
    assert(saved["errors"].Empty());
    auto revision = saved["data"][0]["revision"].GetUint64();
    const auto disk = fileutils::text_read(path);
    Document stored;
    stored.Parse(disk.c_str());
    assert(stored["pages"][0]["cards"].Size() == 2);
    assert(std::string(stored["pages"][0]["cards"][0]["id"].GetString()) == "E004010000000001");

    auto selected_command_line = call(module, "manager_set_active", "[0,\"E004010000000099\"]");
    assert(selected_command_line["errors"].Empty());
    assert(std::string(selected_command_line["data"][0]["active"][0].GetString()) == command_line_card);
    auto selected_saved_again = call(module, "manager_set_active", "[0,\"E004010000000001\"]");
    assert(selected_saved_again["errors"].Empty());

    for (const auto &params : std::vector<std::string> {
        "[0,[]]", "[-1,[]]", "[1,{},0]",
        "[1,[{\"name\":\"A\",\"id\":\"E004010000000001\",\"color\":[0,1,2]}]]",
        "[1,[{\"name\":\"A\",\"id\":\"E004010000000001\",\"color\":[0,\"x\",1]}]]",
        "[1,[{\"name\":\"A\",\"id\":\"E004\\u000001000000000\",\"color\":[0,0,0]}]]",
        "[1,[{\"name\":\"A\\u0000B\",\"id\":\"E004010000000001\",\"color\":[0,0,0]}]]",
        "[1,[{\"name\":\"A\",\"id\":\"bad\",\"color\":[0,0,0]}]]",
        "[1,[{\"name\":\"A\",\"id\":\"E004010000000001\",\"color\":[0,0,0],\"path\":\"x\"}]]"
    }) {
        expect_error(call(module, "manager_save", params));
        assert(fileutils::text_read(path) == disk);
        assert(cfg::CardManagerStore::get().revision == revision);
    }

    card_write_failure = true;
    expect_error(call(module, "manager_save", "[" + std::to_string(revision) + ",[]]"));
    assert(fileutils::text_read(path) == disk);
    assert(cfg::CardManagerStore::get().cards.size() == 2);
    assert(cfg::CardManagerStore::get().revision == revision);
    card_write_failure = false;

    auto active = call(module, "manager_set_active", "[1,\"e004010000000001\"]");
    assert(active["errors"].Empty());
    assert(std::string(active["data"][0]["active"][1].GetString()) == "E004010000000001");
    assert(card_insert_count == 0);
    assert(call(module, "insert", "[1,\"e004010000000001\"]")["errors"].Empty());
    assert(card_insert_count == 1 && card_insert_reader == 1);
    expect_error(call(module, "insert", "[1,\"E004\\u000001000000000\"]"));
    assert(card_insert_count == 1);
    for (const auto &params : {"[2,\"\"]", "[0,\"nothex\"]", "[0,\"E004\\u000001000000000\"]", "[0,\"\",1]"}) {
        expect_error(call(module, "manager_set_active", params));
        assert(eamuse_get_card_override(1) == "E004010000000001");
    }
    assert(call(module, "manager_set_active", "[1,\"\"]")["errors"].Empty());
    assert(eamuse_get_card_override(1).empty());

    auto &store = cfg::CardManagerStore::get();
    {
        std::lock_guard<std::recursive_mutex> lock(store.mutex);
        store.cards[0].name = "native edit";
        store.changed();
        assert(store.save());
    }
    expect_error(call(module, "manager_save", "[" + std::to_string(revision) + ",[]]"));
    assert(store.cards[0].name == "native edit");

    revision = store.revision;
    std::atomic_int wins = 0;
    auto writer = [&] {
        api::modules::Card own_module;
        auto result = call(own_module, "manager_save", "[" + std::to_string(revision) + ",[" + card_b + "," + card_a + "]]" );
        if (result["errors"].Empty()) ++wins;
    };
    std::thread first(writer), second(writer);
    first.join(); second.join();
    assert(wins == 1);
    assert(store.cards[0].id == "E004010000000002");
    assert(store.revision == revision + 1);
    auto duplicates = call(module, "manager_save", "[" + std::to_string(store.revision) + ",[" + card_a + "," + card_a + "]]" );
    assert(duplicates["errors"].Empty());
    assert(store.cards.size() == 2 && store.cards[0].id == store.cards[1].id);
    std::cout << "Card API/store validation, persistence and concurrent-writer tests passed: " << mode << '\n';
}
