#include "keypads_portable.h"
#include "api/modules/keypads.h"
#include "external/rapidjson/document.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace rapidjson;

static Document call(api::modules::Keypads &module, const std::string &function,
        const std::string &params = "[]") {
    Document input;
    const auto json = "{\"id\":1,\"module\":\"keypads\",\"function\":\""
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

static void expect_error(const Document &result) {
    assert(result["errors"].Size() == 1);
    assert(result["data"].Empty());
}

int main() {
    keypad_count = 2;
    keypad_override_history.clear();
    games::iidx::TDJ_MODE = false;
    games::popn::pikapika = false;
    std::copy_n("TST", 4, avs::game::MODEL);

    api::modules::Keypads module;
    assert(!module.requires_password("capabilities"));
    assert(!module.requires_password("insert_card"));
    auto capabilities = call(module, "capabilities");
    assert(capabilities["errors"].Empty());
    assert(std::string(capabilities["data"][0]["model"].GetString()) == "TST");
    assert(capabilities["data"][0]["keypad_count"].GetInt() == 2);
    assert(capabilities["data"][0]["keys"].GetBool());
    assert(capabilities["data"][0]["insert_card"].GetBool());
    expect_error(call(module, "capabilities", "[0]"));

    auto insert = call(module, "insert_card", "[1]");
    assert(insert["errors"].Empty());
    assert(keypad_override_history.size() == 2);
    assert(keypad_override_history[0] == (1u << EAM_IO_INSERT));
    assert(keypad_override_history[1] == 0);
    expect_error(call(module, "insert_card", "[2]"));
    expect_error(call(module, "insert_card", "[]"));

    keypad_override_history.clear();
    assert(call(module, "write", "[0,\"1A\"]")["errors"].Empty());
    assert(keypad_override_history.size() == 4);
    assert(keypad_override_history[0] == (1u << EAM_IO_KEYPAD_1));
    assert(keypad_override_history[2] == (1u << EAM_IO_KEYPAD_00));

    std::copy_n("LDJ", 4, avs::game::MODEL);
    games::iidx::TDJ_MODE = true;
    capabilities = call(module, "capabilities");
    assert(!capabilities["data"][0]["keys"].GetBool());
    assert(capabilities["data"][0]["insert_card"].GetBool());

    games::iidx::TDJ_MODE = false;
    games::popn::pikapika = true;
    capabilities = call(module, "capabilities");
    assert(!capabilities["data"][0]["keys"].GetBool());
    std::cout << "Keypad capability and Insert Card API tests passed\n";
}
