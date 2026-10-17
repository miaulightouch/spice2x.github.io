#include "patches_portable.h"
#include "api/modules/patches.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"

using namespace rapidjson;

static Document call(api::modules::Patches &module, const std::string &function,
        const std::string &params = "[]") {
    Document input;
    const auto json = "{\"id\":7,\"module\":\"patches\",\"function\":\""
        + function + "\",\"params\":" + params + "}";
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

static std::string state_json(api::modules::Patches &module) {
    auto result = call(module, "get");
    assert(result["errors"].Empty());
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    result["data"][0].Accept(writer);
    return buffer.GetString();
}

static patcher::PatchData make_patch(const char *id, const char *name,
        patcher::PatchType type) {
    patcher::PatchData patch{};
    patch.hash = id;
    patch.name = name;
    patch.description = "description";
    patch.caution = "caution";
    patch.game_code = "TEST";
    patch.type = type;
    patch.last_status = patcher::PatchStatus::Disabled;
    return patch;
}

int main() {
    patcher::patches.clear();
    patcher::config_dirty = false;
    patcher::setting_auto_apply = false;
    patcher::patches_test_apply_failure = false;
    patcher::patches_test_force_status_error = false;
    patcher::patches_test_force_integer_status_error = false;
    patcher::patches_test_force_integer_unverified = false;
    patcher::patches_test_fail_on_apply_call = 0;
    patcher::patches_test_apply_calls = 0;
    patcher::patches_test_save_failure = false;
    patcher::patches_test_import_result = false;
    patcher::patches_test_import_error.clear();
    patcher::patches_test_reload_count = 0;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_import_count = 0;
    patcher::patches_test_events.clear();
    patcher::patch_url.clear();
    patcher::url_recents.clear();
    patcher::patch_groups.clear();
    patcher::url_fetch_errors.clear();
    avs::game::DLL_NAME = "target.dll";
    cfg::CONFIGURATOR_STANDALONE = true;

    auto memory = make_patch("memory-id", "Memory", patcher::PatchType::Memory);
    memory.game_code = "TST";
    memory.group_id = "gameplay";
    memory.patch_number.value = 0x13579bdf;
    auto signature = make_patch("signature-id", "Signature", patcher::PatchType::Signature);
    signature.game_code = "tst";
    signature.group_id = "gameplay";
    auto union_patch = make_patch("union-id", "Union", patcher::PatchType::Union);
    union_patch.patches_union.push_back({.name = "one"});
    union_patch.patches_union.push_back({.name = "two"});
    union_patch.selected_union_name = "one";
    auto integer = make_patch("integer-id", "Integer", patcher::PatchType::Integer);
    integer.game_code = "ALT";
    integer.group_id = "gameplay";
    integer.patch_number.min = 1;
    integer.patch_number.max = 10;
    integer.patch_number.value = 3;
    patcher::patches = {memory, signature, union_patch, integer};
    patcher::patch_groups.emplace(std::make_pair(std::string("tst"), std::string("gameplay")), patcher::PatchGroup {
        .name = "Gameplay",
        .description = "Gameplay group",
        .caution = "Use with care",
        .name_in_lower_case = "gameplay",
    });
    patcher::patch_groups.emplace(std::make_pair(std::string("alt"), std::string("gameplay")), patcher::PatchGroup {
        .name = "Gameplay",
        .description = "Alternate gameplay group",
        .caution = "Use with care",
        .name_in_lower_case = "gameplay-alt",
    });

    api::modules::Patches module;
    for (const auto *function : {"get", "set", "set_auto_apply", "save", "reload", "import_url"}) {
        auto result = call(module, function, "[null]");
        assert(result["errors"].Size() == 1);
    }
    assert(module.requires_password("get"));
    assert(module.requires_password("set"));
    assert(module.requires_password("set_group"));
    assert(module.requires_password("disable_all"));
    assert(module.requires_password("import_url"));
    assert(module.requires_password("remove_recent_url"));
    assert(module.requires_password("clear_recent_urls"));

    auto result = call(module, "get");
    assert(result["errors"].Empty());
    assert(result["data"].Size() == 1);
    assert(std::string(result["data"][0]["game"].GetString()) == "PATCH-TEST");
    assert(std::string(result["data"][0]["identity"]["game_identifier"].GetString()) == "PATCH-TEST");
    assert(std::string(result["data"][0]["identity"]["pe_identifier"].GetString()) == "PE-TEST");
    assert(result["data"][0]["identity"]["patch_file"].IsString());
    assert(result["data"][0]["groups"].Size() == 2);
    assert(std::string(result["data"][0]["groups"][0]["game_code"].GetString()) == "tst");
    assert(std::string(result["data"][0]["groups"][0]["id"].GetString()) == "gameplay");
    assert(std::string(result["data"][0]["groups"][0]["name"].GetString()) == "Gameplay");
    assert(std::string(result["data"][0]["groups"][1]["game_code"].GetString()) == "alt");
    assert(std::string(result["data"][0]["groups"][1]["id"].GetString()) == "gameplay");
    assert(std::string(result["data"][0]["patches"][3]["game_code"].GetString()) == "alt");
    assert(std::string(result["data"][0]["patches"][3]["group"].GetString()) == "gameplay");
    assert(result["data"][0]["patches"].Size() == 4);
    assert(std::string(result["data"][0]["patches"][1]["type"].GetString()) == "memory");
    assert(result["data"][0]["patches"][2]["choices"].Size() == 2);
    assert(result["data"][0]["patches"][3]["min"].GetInt() == 1);
    assert(result["data"][0]["patches"][3]["max"].GetInt() == 10);
    assert(result["data"][0]["patches"][3]["value"].GetInt() == 3);
    assert(!result["data"][0]["patches"][0].HasMember("min"));
    assert(result["data"][0]["can_import"].GetBool());
    assert(std::string(result["data"][0]["import_url"].GetString()).empty());
    assert(std::string(result["data"][0]["import_errors"].GetString()).empty());
    assert(result["data"][0]["recent_urls"].Empty());

    const auto before_invalid = state_json(module);
    for (const auto &params : {
        std::string("[\"memory-id\",{\"unknown\":true} ]"),
        std::string("[\"memory-id\",{\"enabled\":1} ]"),
        std::string("[\"union-id\",{\"choice\":\"missing\"} ]"),
        std::string("[\"integer-id\",{\"value\":11} ]"),
        std::string("[\"memory-id\",{\"value\":1} ]"),
        std::string("[\"missing-id\",{\"enabled\":true} ]"),
    }) {
        result = call(module, "set", params);
        assert(result["errors"].Size() == 1);
        assert(result["data"].Empty());
        assert(state_json(module) == before_invalid);
    }

    result = call(module, "set", R"(["memory-id",{"enabled":true}])");
    assert(result["errors"].Empty());
    assert(result["data"][0]["patches"][0]["enabled"].GetBool());
    assert(std::string(result["data"][0]["patches"][0]["status"].GetString()) == "enabled");
    assert(patcher::setting_auto_apply);
    assert(patcher::config_dirty);

    // Runtime errors invalidate the observed integer value, but retain its bounds and
    // leave a separate usable patch visible in the same response.
    patcher::patches[3].patch_number.min = 10;
    patcher::patches[3].patch_number.max = 20;
    patcher::patches[3].patch_number.value = 0;
    patcher::patches[3].unverified = false;
    patcher::patches_test_force_integer_status_error = true;
    result = call(module, "get");
    assert(result["errors"].Empty());
    const auto &runtime_error_integer = result["data"][0]["patches"][3];
    assert(runtime_error_integer["min"].GetInt() == 10);
    assert(runtime_error_integer["max"].GetInt() == 20);
    assert(!runtime_error_integer.HasMember("value"));
    assert(std::string(runtime_error_integer["status"].GetString()) == "error");
    assert(runtime_error_integer["unverified"].GetBool() == false);
    assert(std::string(result["data"][0]["patches"][0]["status"].GetString()) == "enabled");
    assert(result["data"][0]["patches"][0]["error"].GetStringLength() == 0);
    patcher::patches_test_force_integer_status_error = false;

    // Standalone fatal errors report the requested enabled state as unverified; the
    // integer value is still unknown and must remain absent.
    patcher::patches[3].patch_number.value = 0;
    patcher::patches[3].unverified = false;
    patcher::patches_test_force_integer_unverified = true;
    result = call(module, "get");
    assert(result["errors"].Empty());
    const auto &unverified_integer = result["data"][0]["patches"][3];
    assert(unverified_integer["min"].GetInt() == 10);
    assert(unverified_integer["max"].GetInt() == 20);
    assert(!unverified_integer.HasMember("value"));
    assert(unverified_integer["unverified"].GetBool());
    assert(std::string(result["data"][0]["patches"][0]["status"].GetString()) == "enabled");
    patcher::patches_test_force_integer_unverified = false;
    patcher::patches[3].unverified = false;
    patcher::patches[3].patch_number.min = 1;
    patcher::patches[3].patch_number.max = 10;
    patcher::patches[3].patch_number.value = 3;

    patcher::patches_test_apply_failure = true;
    patcher::patches_test_apply_calls = 0;
    patcher::patches[1].enabled = false;
    patcher::setting_auto_apply = false;
    patcher::config_dirty = false;
    result = call(module, "set", R"(["signature-id",{"enabled":true}])");
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
    assert(!patcher::patches[1].enabled);
    assert(!patcher::setting_auto_apply);
    assert(!patcher::config_dirty);
    patcher::patches_test_apply_failure = false;

    // A failed apply must not read or mutate NumberPatch storage on a memory patch.
    const auto memory_number = patcher::patches[0].patch_number.value;
    patcher::patches_test_apply_failure = true;
    patcher::patches_test_apply_calls = 0;
    result = call(module, "set", R"(["memory-id",{"enabled":false}])");
    assert(result["errors"].Size() == 1);
    assert(patcher::patches[0].patch_number.value == memory_number);
    patcher::patches_test_apply_failure = false;

    // If rollback itself fails, expose the real post-rollback status and retain its error.
    patcher::patches_test_force_status_error = true;
    patcher::patches_test_fail_on_apply_call = 2;
    patcher::patches_test_apply_calls = 0;
    patcher::patches[0].enabled = false;
    patcher::setting_auto_apply = false;
    result = call(module, "set", R"(["memory-id",{"enabled":true}])");
    assert(result["errors"].Size() == 1);
    assert(std::string(result["errors"][0].GetString()).find("rollback failed") != std::string::npos);
    assert(!patcher::patches[0].enabled);
    assert(!patcher::setting_auto_apply);
    assert(!patcher::config_dirty);
    assert(patcher::patches[0].last_status == patcher::PatchStatus::Error);
    assert(patcher::patches[0].error_reason.find("rollback failed") != std::string::npos);
    result = call(module, "get");
    assert(result["errors"].Empty());
    assert(std::string(result["data"][0]["patches"][0]["status"].GetString()) == "error");
    assert(std::string(result["data"][0]["patches"][0]["error"].GetString()).find("rollback failed") != std::string::npos);
    patcher::patches_test_force_status_error = false;
    patcher::patches_test_fail_on_apply_call = 0;

    result = call(module, "set", R"(["union-id",{"enabled":true,"choice":"two"}])");
    assert(result["errors"].Empty());
    assert(std::string(result["data"][0]["patches"][2]["selected"].GetString()) == "two");

    result = call(module, "set", R"(["integer-id",{"enabled":true,"value":7}])");
    assert(result["errors"].Empty());
    assert(result["data"][0]["patches"][3]["value"].GetInt() == 7);

    result = call(module, "set_auto_apply", "[false]");
    assert(result["errors"].Empty());
    assert(!result["data"][0]["auto_apply"].GetBool());

    result = call(module, "save");
    assert(result["errors"].Empty());
    assert(!patcher::config_dirty);
    assert(patcher::patches_test_save_count == 1);

    patcher::config_dirty = true;
    patcher::patches_test_save_failure = true;
    result = call(module, "save");
    assert(result["errors"].Size() == 1);
    assert(patcher::config_dirty);
    patcher::patches_test_save_failure = false;

    result = call(module, "set", R"(["integer-id",{"enabled":false}])");
    assert(result["errors"].Empty());
    assert(!result["data"][0]["patches"][3]["enabled"].GetBool());
    result = call(module, "set_group", R"(["TST","gameplay",false])");
    assert(result["errors"].Empty());
    assert(!result["data"][0]["patches"][0]["enabled"].GetBool());
    assert(!result["data"][0]["patches"][1]["enabled"].GetBool());
    assert(!result["data"][0]["patches"][3]["enabled"].GetBool());
    result = call(module, "set_group", R"(["TST","gameplay",true])");
    assert(result["errors"].Empty());
    assert(result["data"][0]["patches"][0]["enabled"].GetBool());
    assert(result["data"][0]["patches"][1]["enabled"].GetBool());
    assert(!result["data"][0]["patches"][3]["enabled"].GetBool());
    result = call(module, "set_group", R"(["ALT","gameplay",true])");
    assert(result["errors"].Empty());
    assert(result["data"][0]["patches"][3]["enabled"].GetBool());

    patcher::patches_test_apply_failure = true;
    result = call(module, "set_group", R"(["TST","gameplay",false])");
    assert(result["errors"].Size() == 1);
    assert(result["data"].Size() == 1);
    assert(result["data"][0]["patches"][0].HasMember("error"));
    patcher::patches_test_apply_failure = false;

    result = call(module, "disable_all");
    assert(result["errors"].Empty());
    assert(!result["data"][0]["auto_apply"].GetBool());
    for (const auto &patch : result["data"][0]["patches"].GetArray()) {
        assert(!patch["enabled"].GetBool());
    }

    patcher::url_recents = {"https://one.example/patches", "https://two.example/patches"};
    patcher::url_recent_idx = patcher::url_recents.size() - 1;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_save_failure = true;
    result = call(module, "remove_recent_url", R"(["https://one.example/patches"])");
    assert(result["errors"].Size() == 1);
    assert(patcher::url_recents == std::vector<std::string>({
        "https://one.example/patches", "https://two.example/patches"}));
    assert(patcher::url_recent_idx == 1);
    assert(patcher::patches_test_save_count == 1);
    patcher::patches_test_save_failure = false;
    result = call(module, "remove_recent_url", R"(["https://one.example/patches"])");
    assert(result["errors"].Empty());
    assert(patcher::url_recent_idx == static_cast<size_t>(-1));
    assert(patcher::patches_test_save_count == 2);
    assert(result["data"][0]["recent_urls"].Size() == 1);
    assert(std::string(result["data"][0]["recent_urls"][0].GetString()) == "https://two.example/patches");
    patcher::url_recents = {"https://one.example/patches", "https://two.example/patches"};
    patcher::url_recent_idx = patcher::url_recents.size() - 1;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_save_failure = true;
    result = call(module, "clear_recent_urls");
    assert(result["errors"].Size() == 1);
    assert(patcher::url_recents == std::vector<std::string>({
        "https://one.example/patches", "https://two.example/patches"}));
    assert(patcher::url_recent_idx == 1);
    assert(patcher::patches_test_save_count == 1);
    patcher::patches_test_save_failure = false;
    result = call(module, "clear_recent_urls");
    assert(result["errors"].Empty());
    assert(patcher::url_recent_idx == static_cast<size_t>(-1));
    assert(patcher::patches_test_save_count == 2);
    assert(result["data"][0]["recent_urls"].Empty());

    result = call(module, "reload");
    assert(result["errors"].Empty());
    assert(result["data"][0]["patches"].Size() == 4);
    assert(patcher::patches_test_reload_count == 1);

    patcher::patch_url = "https://old.example/patches";
    patcher::url_recents = {patcher::patch_url};
    patcher::patches_test_import_count = 0;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_reload_count = 0;
    patcher::patches_test_events.clear();
    const auto before_invalid_import = state_json(module);
    for (const auto &url : {
        std::string("ftp://example.com/patches"),
        std::string("https:///patches"),
        std::string("https://user@example.com/patches"),
        std::string("https://example.com/patches?game=1"),
        std::string("https://example.com/patches#latest"),
        std::string("https://example.com/patches with-space"),
        std::string("https://:8080/patches"),
    }) {
        result = call(module, "import_url", "[\"" + url + "\"]");
        assert(result["errors"].Size() == 1);
        assert(result["data"].Empty());
        assert(patcher::patches_test_import_count == 0);
        assert(patcher::patches_test_save_count == 0);
        assert(patcher::patches_test_reload_count == 0);
        assert(state_json(module) == before_invalid_import);
    }
    const auto long_url = "https://example.com/" + std::string(4096, 'a');
    result = call(module, "import_url", "[\"" + long_url + "\"]");
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
    assert(patcher::patches_test_import_count == 0);
    assert(patcher::patches_test_reload_count == 0);
    assert(state_json(module) == before_invalid_import);

    cfg::CONFIGURATOR_STANDALONE = false;
    const auto before_mode_rejection = state_json(module);
    result = call(module, "import_url", R"(["https://example.com/patches"])");
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
    assert(patcher::patches_test_import_count == 0);
    assert(patcher::patches_test_reload_count == 0);
    assert(state_json(module) == before_mode_rejection);
    cfg::CONFIGURATOR_STANDALONE = true;

    avs::game::DLL_NAME.clear();
    const auto before_dll_rejection = state_json(module);
    result = call(module, "import_url", R"(["https://example.com/patches"])");
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
    assert(patcher::patches_test_import_count == 0);
    assert(patcher::patches_test_reload_count == 0);
    assert(state_json(module) == before_dll_rejection);
    avs::game::DLL_NAME = "target.dll";

    patcher::patches_test_import_result = true;
    patcher::patches_test_import_error = "extra.dll: download failed";
    patcher::url_recent_idx = 0;
    patcher::patches_test_import_count = 0;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_reload_count = 0;
    patcher::patches_test_events.clear();
    result = call(module, "import_url", R"(["https://example.com/patches"])");
    assert(result["errors"].Empty());
    assert(result["data"].Size() == 1);
    assert(result["data"][0]["can_import"].GetBool());
    assert(std::string(result["data"][0]["import_url"].GetString())
        == "https://example.com/patches");
    assert(std::string(result["data"][0]["import_errors"].GetString())
        == "extra.dll: download failed");
    assert(result["data"][0]["recent_urls"].Size() == 2);
    assert(patcher::url_recent_idx == static_cast<size_t>(-1));
    assert(patcher::patches_test_import_count == 1);
    assert(patcher::patches_test_save_count == 1);
    assert(patcher::patches_test_reload_count == 1);
    assert((patcher::patches_test_events == std::vector<std::string>{"import", "save", "reload"}));

    patcher::patches_test_import_result = false;
    patcher::patches_test_import_error = "download failed";
    patcher::patches_test_import_count = 0;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_reload_count = 0;
    patcher::patches_test_events.clear();
    result = call(module, "import_url", R"(["https://failed.example/patches"])");
    assert(result["errors"].Size() == 1);
    assert(std::string(result["errors"][0].GetString()) == "Unable to import patches from URL.");
    assert(patcher::patches_test_import_count == 1);
    assert(patcher::patches_test_save_count == 0);
    assert(patcher::patches_test_reload_count == 1);
    assert((patcher::patches_test_events == std::vector<std::string>{"import", "reload"}));
    assert(patcher::url_recents.size() == 2);

    patcher::patches_test_import_result = true;
    patcher::patches_test_import_error.clear();
    patcher::patches_test_save_failure = true;
    patcher::patches_test_import_count = 0;
    patcher::patches_test_save_count = 0;
    patcher::patches_test_reload_count = 0;
    patcher::patches_test_events.clear();
    result = call(module, "import_url", R"(["https://save-fails.example/patches"])");
    assert(result["errors"].Size() == 1);
    assert(std::string(result["errors"][0].GetString()).find("imported") != std::string::npos);
    assert(std::string(result["errors"][0].GetString()).find("save") != std::string::npos);
    assert(patcher::patches_test_import_count == 1);
    assert(patcher::patches_test_save_count == 1);
    assert(patcher::patches_test_reload_count == 1);
    assert((patcher::patches_test_events == std::vector<std::string>{"import", "save", "reload"}));
    assert(patcher::url_recents.size() == 3);
    patcher::patches_test_save_failure = false;

    std::cout << "Native patches API tests passed: schema, password, validation, selection, apply/save errors, URL import, and state persistence.\n";
}
