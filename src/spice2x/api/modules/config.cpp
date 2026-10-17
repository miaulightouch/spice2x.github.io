#include "config.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "external/rapidjson/document.h"
#include "cfg/config.h"
#include "games/io.h"
#include "launcher/launcher.h"
#include "launcher/options.h"
#include "misc/eamuse.h"

using namespace std::placeholders;
using namespace rapidjson;
using api::Response;

namespace {

constexpr SizeType MAX_STRING_LENGTH = 4096;

Value json_string(const std::string &value, Document::AllocatorType &alloc) {
    return Value(value.c_str(), static_cast<SizeType>(value.size()), alloc);
}

bool read_string(const Value &value, std::string &out) {
    if (!value.IsString() || value.GetStringLength() > MAX_STRING_LENGTH) {
        return false;
    }
    out.assign(value.GetString(), value.GetStringLength());
    return out.find('\0') == std::string::npos;
}

const OptionDefinition *find_definition(const std::string &name) {
    for (const auto &definition : launcher::get_option_definitions()) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

bool is_known_game(const std::string &game) {
    const auto &games = ::games::get_games();
    return std::find(games.begin(), games.end(), game) != games.end();
}

std::vector<Option> effective_options(
        const ConfigOptionSnapshot &snapshot,
        const std::string &game) {
    std::vector<Option> options;
    const auto &definitions = launcher::get_option_definitions();
    options.reserve(definitions.size());

    for (const auto &definition : definitions) {
        auto value = std::string();
        for (const auto &persisted : snapshot.options) {
            if (persisted.get_definition().name == definition.name) {
                value = persisted.value;
                break;
            }
        }
        options.emplace_back(definition, std::move(value));
    }

    if (LAUNCHER_OPTIONS) {
        options = launcher::merge_options(options, *LAUNCHER_OPTIONS);
    }

    options.erase(std::remove_if(options.begin(), options.end(), [&game](const Option &option) {
        const auto &definition = option.get_definition();
        return !definition.game_name.empty() && definition.game_name != game;
    }), options.end());
    return options;
}

const Option *find_option(const std::vector<Option> &options, const std::string &name) {
    for (const auto &option : options) {
        if (option.get_definition().name == name) {
            return &option;
        }
    }
    return nullptr;
}

const char *option_type_name(OptionType type) {
    switch (type) {
        case OptionType::Bool: return "bool";
        case OptionType::Text: return "text";
        case OptionType::Integer: return "integer";
        case OptionType::Hex: return "hex";
        case OptionType::Enum: return "enum";
    }
    return "text";
}

bool valid_uint32(const std::string &value) {
    if (value.empty()) {
        return true;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    })) {
        return false;
    }
    try {
        size_t end = 0;
        const auto parsed = std::stoull(value, &end, 10);
        return end == value.size() && parsed <= std::numeric_limits<uint32_t>::max();
    } catch (...) {
        return false;
    }
}

bool valid_uint64_hex(const std::string &value) {
    if (value.empty()) {
        return true;
    }
    auto digits = value;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits.erase(0, 2);
    }
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    })) {
        return false;
    }
    try {
        size_t end = 0;
        std::stoull(value, &end, 16);
        return end == value.size();
    } catch (...) {
        return false;
    }
}

bool valid_value(const OptionDefinition &definition, const std::string &value) {
    switch (definition.type) {
        case OptionType::Bool:
            return value.empty() || value == "/ENABLED";
        case OptionType::Text:
            return true;
        case OptionType::Integer:
            return valid_uint32(value);
        case OptionType::Hex:
            return valid_uint64_hex(value);
        case OptionType::Enum:
            return value.empty() || std::any_of(definition.elements.begin(), definition.elements.end(),
                [&value](const auto &element) { return element.first == value; });
    }
    return false;
}

void add_options(Response &res, const std::string &game, const ConfigOptionSnapshot &snapshot) {
    auto &alloc = res.doc()->GetAllocator();
    const auto options = effective_options(snapshot, game);
    Value result(kObjectType);
    result.AddMember("game", json_string(game, alloc), alloc);
    result.AddMember("revision", snapshot.revision, alloc);
    result.AddMember("restart_required", true, alloc);

    Value option_list(kArrayType);
    for (const auto &option : options) {
        const auto &definition = option.get_definition();
        Value item(kObjectType);
        item.AddMember("name", json_string(definition.name, alloc), alloc);
        item.AddMember("title", json_string(definition.title, alloc), alloc);
        item.AddMember("description", json_string(definition.desc, alloc), alloc);
        item.AddMember("category", json_string(definition.category, alloc), alloc);
        item.AddMember("type", StringRef(option_type_name(definition.type)), alloc);
        auto output_value = definition.sensitive ? json_string("", alloc) : json_string(option.value, alloc);
        item.AddMember("value", output_value, alloc);
        item.AddMember("sensitive", definition.sensitive, alloc);
        item.AddMember("has_value", !option.value.empty(), alloc);
        item.AddMember("hidden", definition.hidden, alloc);
        item.AddMember("disabled", definition.disabled || option.disabled, alloc);

        Value elements(kArrayType);
        for (const auto &element : definition.elements) {
            Value choice(kObjectType);
            choice.AddMember("value", json_string(element.first, alloc), alloc);
            choice.AddMember("label", json_string(element.second, alloc), alloc);
            elements.PushBack(choice, alloc);
        }
        item.AddMember("elements", elements, alloc);
        option_list.PushBack(item, alloc);
    }
    result.AddMember("options", option_list, alloc);
    res.add_data(result);
}

} // namespace

namespace api::modules {

Config::Config() : Module("config", true) {
    functions["get_games"] = std::bind(&Config::get_games, this, _1, _2);
    functions["get_options"] = std::bind(&Config::get_options, this, _1, _2);
    functions["set_options"] = std::bind(&Config::set_options, this, _1, _2);
}

void Config::get_games(Request &req, Response &res) {
    if (req.params.Size() != 0) {
        return error(res, "expected no parameters");
    }

    auto &alloc = res.doc()->GetAllocator();
    Value result(kObjectType);
    result.AddMember("current", json_string(eamuse_get_game(), alloc), alloc);
    Value game_list(kArrayType);
    for (const auto &game : ::games::get_games()) {
        game_list.PushBack(json_string(game, alloc), alloc);
    }
    result.AddMember("games", game_list, alloc);
    res.add_data(result);
}

void Config::get_options(Request &req, Response &res) {
    if (req.params.Size() != 1) {
        return error(res, "expected [game:string]");
    }
    std::string game;
    if (!read_string(req.params[0], game)) {
        return error_type(res, "game", "string of bounded length without NUL");
    }
    if (!is_known_game(game)) {
        return error_unknown(res, "game", game);
    }

    add_options(res, game, ::Config::getInstance().getOptionsSnapshot(game));
}

void Config::set_options(Request &req, Response &res) {
    if (req.params.Size() != 3) {
        return error(res, "expected [game:string, revision:uint32, changes:array]");
    }

    std::string game;
    if (!read_string(req.params[0], game)) {
        return error_type(res, "game", "string of bounded length without NUL");
    }
    if (!is_known_game(game)) {
        return error_unknown(res, "game", game);
    }
    if (!req.params[1].IsUint()) {
        return error_type(res, "revision", "uint32");
    }
    if (!req.params[2].IsArray() || req.params[2].Size() > launcher::get_option_definitions().size()) {
        return error_type(res, "changes", "bounded array");
    }

    const auto snapshot = ::Config::getInstance().getOptionsSnapshot(game);
    const auto options = effective_options(snapshot, game);
    std::set<std::string> changed_names;
    std::vector<std::pair<std::string, std::string>> changes;
    changes.reserve(req.params[2].Size());

    for (const auto &change : req.params[2].GetArray()) {
        if (!change.IsObject() || change.MemberCount() != 2 ||
                !change.HasMember("name") || !change.HasMember("value")) {
            return error(res, "each change must contain exactly name and value");
        }
        std::string name;
        std::string value;
        if (!read_string(change["name"], name)) {
            return error_type(res, "change.name", "string of bounded length without NUL");
        }
        if (!read_string(change["value"], value)) {
            return error_type(res, "change.value", "string of bounded length without NUL");
        }
        if (!changed_names.insert(name).second) {
            return error(res, "duplicate option name: " + name);
        }
        const auto *definition = find_definition(name);
        if (definition == nullptr || (!definition->game_name.empty() && definition->game_name != game)) {
            return error_unknown(res, "option", name);
        }
        const auto *option = find_option(options, name);
        if (definition->disabled || option == nullptr || option->disabled) {
            return error(res, "Option is disabled: " + name);
        }
        if (!valid_value(*definition, value)) {
            return error(res, "Invalid value for option: " + name);
        }
        changes.emplace_back(std::move(name), std::move(value));
    }

    const auto result = ::Config::getInstance().updateOptions(game, req.params[1].GetUint(), changes);
    if (result == ConfigOptionUpdateResult::Stale) {
        return error(res, "stale configuration revision; reload options and retry");
    }
    if (result == ConfigOptionUpdateResult::SaveFailed) {
        return error(res, "unable to save configuration; existing disk configuration was preserved");
    }

    add_options(res, game, ::Config::getInstance().getOptionsSnapshot(game));
}

} // namespace api::modules
