#include "config_portable.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include "api/modules/config.h"
#include "external/rapidjson/document.h"
#include "external/tinyxml2/tinyxml2.h"

using namespace rapidjson;

namespace {

const std::vector<OptionDefinition> TEST_DEFINITIONS {
    {.title = "API Port", .name = "api", .display_name = "api-port", .desc = "port",
        .type = OptionType::Integer, .setting_name = "8080", .category = "API",
        .file_extension = "ini", .quick_setting_category = "Common"},
    {.title = "API Password", .name = "apipass", .desc = "password", .type = OptionType::Text,
        .category = "API", .sensitive = true},
    {.title = "Enabled", .name = "enabled", .desc = "enabled", .type = OptionType::Bool, .category = "General"},
    {.title = "Mode", .name = "mode", .desc = "mode", .type = OptionType::Enum, .game_name = "Test Game",
        .category = "Game", .elements = {{"off", "Off"}, {"on", "On"}}},
    {.title = "Mask", .name = "mask", .desc = "mask", .type = OptionType::Hex, .category = "General"},
    {.title = "Text", .name = "text", .desc = "text", .type = OptionType::Text, .category = "General"},
    {.title = "Command Line", .name = "override", .desc = "override", .type = OptionType::Text,
        .category = "General"},
    {.title = "Disabled", .name = "disabled", .desc = "disabled", .type = OptionType::Text,
        .category = "General", .disabled = true},
    {.title = "Hidden", .name = "hidden", .desc = "hidden", .type = OptionType::Bool,
        .hidden = true, .category = "General"},
    {.title = "ASIO", .name = "asio", .desc = "asio", .type = OptionType::Text,
        .category = "Audio", .picker = OptionPickerType::AsioDriver, .quick_setting_category = "Audio"},
    {.title = "Monitor", .name = "monitor", .desc = "monitor", .type = OptionType::Text,
        .category = "Display", .picker = OptionPickerType::Monitor, .quick_setting_category = "Display"},
    {.title = "CPU Affinity", .name = "cpuaffinity", .desc = "cpu", .type = OptionType::Hex,
        .category = "Performance", .picker = OptionPickerType::CpuAffinity},
    {.title = "EA Card", .name = "eacard", .desc = "card", .type = OptionType::Text,
        .category = "Network", .picker = OptionPickerType::EACard},
    {.title = "Other Game", .name = "other", .desc = "other", .type = OptionType::Text,
        .game_name = "Other Game", .category = "Game"},
};

std::vector<std::string> TEST_GAMES {"Test Game", "Other Game"};
std::string TEST_CURRENT_GAME = "Test Game";

const Value &option(const Document &result, const char *name) {
    for (const auto &item : result["data"][0]["options"].GetArray()) {
        if (std::string(item["name"].GetString()) == name) {
            return item;
        }
    }
    std::abort();
}

bool has_option(const Document &result, const char *name) {
    for (const auto &item : result["data"][0]["options"].GetArray()) {
        if (std::string(item["name"].GetString()) == name) {
            return true;
        }
    }
    return false;
}

std::string text_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

Document call(api::modules::Config &module, const std::string &function, const std::string &params) {
    Document request_document;
    const auto request_text = "{\"id\":7,\"module\":\"config\",\"function\":\"" +
        function + "\",\"params\":" + params + "}";
    request_document.Parse(request_text.c_str());
    assert(!request_document.HasParseError());
    api::Request request(request_document);
    api::Response response(request.id);
    module.handle(request, response);
    Document result;
    result.Parse(response.get_string().c_str());
    assert(!result.HasParseError());
    return result;
}

Document set_text(api::modules::Config &module, uint32_t revision, const std::string &value) {
    return call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"text\",\"value\":\"" + value + "\"}]]");
}

} // namespace

namespace launcher {

bool USE_CMD_OVERRIDE = false;

const std::vector<OptionDefinition> &get_option_definitions() {
    return TEST_DEFINITIONS;
}

std::vector<Option> merge_options(
        const std::vector<Option> &options,
        const std::vector<Option> &overrides) {
    std::vector<Option> merged;
    for (const auto &option : options) {
        const auto it = std::find_if(overrides.begin(), overrides.end(), [&option](const Option &override) {
            return option.get_definition().name == override.get_definition().name;
        });
        if (it == overrides.end() || it->value.empty()) {
            merged.push_back(option);
            continue;
        }
        auto &combined = merged.emplace_back(option.get_definition(), "");
        combined.disabled = true;
        if (USE_CMD_OVERRIDE) {
            combined.value = it->value;
        } else {
            combined.value = option.value.empty() ? it->value : option.value;
        }
        combined.conflicting = !option.value.empty();
    }
    return merged;
}

} // namespace launcher

std::unique_ptr<std::vector<Option>> LAUNCHER_OPTIONS;

namespace games {
const std::vector<std::string> &get_games() {
    return TEST_GAMES;
}
}

const std::string &eamuse_get_game() {
    return TEST_CURRENT_GAME;
}

namespace launcher {
const std::vector<std::string> &get_quick_setting_categories() {
    static const std::vector<std::string> categories {
        "Game", "Common", "Network", "Display", "Audio",
    };
    return categories;
}
}

int main() {
    const auto config_path = std::filesystem::current_path() / "spicetools.xml";
    std::ofstream(config_path) <<
        "<?xml version=\"1.0\"?>\n"
        "<games><metadata keep=\"yes\"/>"
        "<game name=\"Other Game\"><options><option name=\"other\" value=\"keep\"/>"
        "</options><custom value=\"preserve\"/></game></games>";
    CONFIG_PATH_OVERRIDE = config_path.string();

    LAUNCHER_OPTIONS = std::make_unique<std::vector<Option>>();
    for (const auto &definition : TEST_DEFINITIONS) {
        LAUNCHER_OPTIONS->emplace_back(definition, definition.name == "override" ? "command" : "");
    }

    auto &config = ::Config::getInstance();
    assert(config.getStatus());
    assert(config.getOptionsSnapshot("Test Game").revision == 0);

    api::modules::Config module;
    assert(module.password_force);
    assert(module.requires_password("get_games"));
    assert(module.requires_password("get_options"));
    assert(module.requires_password("get_option_choices"));
    assert(module.requires_password("set_options"));

    auto result = call(module, "get_games", "[]");
    assert(result["errors"].Empty());
    assert(std::string(result["data"][0]["current"].GetString()) == "Test Game");
    assert(result["data"][0]["games"].Size() == 2);

    result = call(module, "get_games", "[1]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "get_options", "[\"missing\"]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    auto revision = config.getOptionsSnapshot("Test Game").revision;
    assert(config.updateOptions("Test Game", revision, {
        {"apipass", "secret"}, {"enabled", "/ENABLED"}, {"mode", "on"}, {"mask", "0x10"}
    }) == ConfigOptionUpdateResult::Success);
    revision = config.getOptionsSnapshot("Test Game").revision;

    result = call(module, "get_options", "[\"Test Game\"]");
    assert(result["errors"].Empty());
    assert(result["data"][0]["revision"].GetUint() == revision);
    assert(result["data"][0]["restart_required"].GetBool());
    assert(option(result, "apipass")["sensitive"].GetBool());
    assert(std::string(option(result, "apipass")["value"].GetString()).empty());
    assert(option(result, "apipass")["has_value"].GetBool());
    assert(std::string(option(result, "enabled")["value"].GetString()) == "/ENABLED");
    assert(std::string(option(result, "mode")["value"].GetString()) == "on");
    assert(std::string(option(result, "api")["display_name"].GetString()) == "api-port");
    assert(std::string(option(result, "api")["setting_name"].GetString()) == "8080");
    assert(std::string(option(result, "api")["quick_setting_category"].GetString()) == "Common");
    assert(std::string(option(result, "api")["picker"].GetString()) == "none");
    assert(std::string(option(result, "api")["file_extension"].GetString()) == "ini");
    assert(result["data"][0]["quick_categories"].IsArray());
    assert(result["data"][0]["quick_categories"].Size() == 3);
    assert(std::string(result["data"][0]["quick_categories"][0].GetString()) == "Common");
    assert(std::string(result["data"][0]["quick_categories"][1].GetString()) == "Display");
    assert(std::string(result["data"][0]["quick_categories"][2].GetString()) == "Audio");
    assert(option(result, "hidden")["hidden"].GetBool());
    assert(option(result, "override")["disabled"].GetBool());
    assert(!has_option(result, "other"));

    result = call(module, "get_option_choices", "[\"Test Game\",\"asio\"]");
    assert(result["errors"].Empty() && result["data"].Size() == 1);
    assert(std::string(result["data"][0]["name"].GetString()) == "asio");
    assert(result["data"][0]["choices"].Size() == 2);
    assert(std::string(result["data"][0]["choices"][0]["value"].GetString()) == "Test ASIO");
    assert(std::string(result["data"][0]["choices"][0]["label"].GetString()) ==
        "Test ASIO (32/64-bit)");
    result = call(module, "get_option_choices", "[\"Test Game\",\"monitor\"]");
    assert(result["errors"].Empty() && result["data"][0]["choices"].Size() == 1);
    assert(std::string(result["data"][0]["choices"][0]["value"].GetString()) ==
        "\\\\.\\DISPLAY1");
    assert(std::string(result["data"][0]["choices"][0]["label"].GetString()) ==
        "\\\\.\\DISPLAY1 (Test Monitor) [Primary]");
    result = call(module, "get_option_choices", "[\"Test Game\",\"cpuaffinity\"]");
    assert(result["errors"].Empty() && result["data"][0]["choices"].Size() >= 1);
    assert(std::string(result["data"][0]["choices"][0]["value"].GetString()) == "0x1");
    assert(std::string(result["data"][0]["choices"][0]["label"].GetString()) == "CPU 0");
    result = call(module, "get_option_choices", "[\"Test Game\",\"eacard\"]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    const auto before_invalid = text_file(config_path);
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"text\",\"value\":\"changed\"},"
        "{\"name\":\"enabled\",\"value\":\"true\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    assert(text_file(config_path) == before_invalid);
    assert(config.getOptionsSnapshot("Test Game").revision == revision);

    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"text\",\"value\":\"changed\",\"extra\":1}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"other\",\"value\":\"bad\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"override\",\"value\":\"bad\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"disabled\",\"value\":\"bad\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"enabled\",\"value\":\"/ENABLED\"},"
        "{\"name\":\"enabled\",\"value\":\"\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    for (const auto *value : {"-1", "4294967296"}) {
        result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
            ", [{\"name\":\"api\",\"value\":\"" + std::string(value) + "\"}]]");
        assert(!result["errors"].Empty() && result["data"].Empty());
    }
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"mode\",\"value\":\"invalid\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"mask\",\"value\":\"0x10000000000000000\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "set_options", "[\"Test Game\"," + std::to_string(revision) +
        ", [{\"name\":\"text\",\"value\":\"a\\u0000b\"}]]");
    assert(!result["errors"].Empty() && result["data"].Empty());

    assert(config.updateOptions("Test Game", revision, {{"text", "native"}}) ==
        ConfigOptionUpdateResult::Success);
    const auto stale_revision = revision;
    revision = config.getOptionsSnapshot("Test Game").revision;
    result = set_text(module, stale_revision, "stale");
    assert(!result["errors"].Empty() && result["data"].Empty());
    result = call(module, "get_options", "[\"Test Game\"]");
    assert(std::string(option(result, "text")["value"].GetString()) != "stale");

    result = set_text(module, revision, "saved");
    assert(result["errors"].Empty() && result["data"].Size() == 1);
    assert(std::string(option(result, "text")["value"].GetString()) == "saved");
    revision = result["data"][0]["revision"].GetUint();

    const auto before_failed_save = text_file(config_path);
    config_test_fail_move = true;
    result = set_text(module, revision, "must-fail");
    config_test_fail_move = false;
    assert(!result["errors"].Empty() && result["data"].Empty());
    assert(text_file(config_path) == before_failed_save);
    assert(std::string(option(call(module, "get_options", "[\"Test Game\"]"), "text")["value"].GetString()) == "saved");

    const auto concurrent_revision = config.getOptionsSnapshot("Test Game").revision;
    std::atomic<int> successes = 0;
    std::atomic<int> stale = 0;
    auto concurrent_set = [&](const char *value) {
        auto concurrent_result = set_text(module, concurrent_revision, value);
        if (concurrent_result["errors"].Empty()) {
            successes.fetch_add(1);
        } else if (std::string(concurrent_result["errors"][0].GetString()).find("stale") != std::string::npos) {
            stale.fetch_add(1);
        }
    };
    std::thread first(concurrent_set, "thread-one");
    std::thread second(concurrent_set, "thread-two");
    first.join();
    second.join();
    assert(successes == 1 && stale == 1);

    const auto native_revision = config.getOptionsSnapshot("Test Game").revision;
    assert(config.save());
    result = set_text(module, native_revision, "after-native-save");
    assert(!result["errors"].Empty() && result["data"].Empty());

    tinyxml2::XMLDocument saved;
    assert(saved.LoadFileA(config_path.string().c_str()) == tinyxml2::XML_SUCCESS);
    const auto *other = saved.RootElement()->FirstChildElement("game");
    assert(other != nullptr && std::string(other->Attribute("name")) == "Other Game");
    assert(other->FirstChildElement("custom") != nullptr);
    assert(std::string(saved.RootElement()->FirstChildElement("metadata")->Attribute("keep")) == "yes");

    std::cout << "Config API tests passed: auth, metadata, typing, game filtering, all-or-none, "
        "stale revisions, sensitive redaction, atomic saves and concurrent native/API writes.\n";
}
