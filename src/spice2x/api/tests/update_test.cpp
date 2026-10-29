#include "update_portable.h"
#include "api/modules/update.h"

#include <cassert>
#include <iostream>

using namespace rapidjson;

static Document call(api::modules::Update &module, const char *function,
        const std::string &params = "[]") {
    Document input;
    const auto json = std::string("{\"id\":1,\"module\":\"update\",\"function\":\"") +
        function + "\",\"params\":" + params + "}";
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

static void failed(const Document &response) {
    assert(response["errors"].Size() == 1);
    assert(response["data"].Empty());
}

int main() {
    const std::string files = "[[{\"name\":\"spice.exe\",\"size\":2,\"crc32\":1}]]";
    api::modules::Update owner;
    api::modules::Update stranger;
    for (const auto *function : {"status", "begin", "chunk", "finish", "apply", "cancel"}) {
        assert(owner.requires_password(function));
    }
    const auto status = call(owner, "status");
    assert(status["errors"].Empty());
    assert(status["data"][0]["chunk_bytes"].GetUint() == 32768);
    assert(status["data"][0]["executable_crc32"].GetUint() == 1234);
    assert(std::string(status["data"][0]["update_id"].GetString()) ==
        "0123456789abcdef0123456789abcdef");
    assert(std::string(status["data"][0]["executable"].GetString()) == "spice.exe");
    failed(call(owner, "status", "[1]"));

    for (const auto *params : {"[]", "[[]]", "[null]", "[[null]]",
            "[[{\"name\":\"spice.exe\",\"size\":-1,\"crc32\":0}]]",
            "[[{\"name\":\"spice.exe\",\"size\":2,\"crc32\":-1}]]",
            "[[{\"name\":\"spice.exe\",\"size\":2,\"crc32\":0,\"path\":\"C:/\"}]]"}) {
        failed(call(owner, "begin", params));
    }
    assert(ota::begins == 0);
    assert(call(owner, "begin", files)["errors"].Empty());
    failed(call(owner, "begin", files));
    failed(call(stranger, "finish", "[\"session-id\"]"));
    failed(call(stranger, "cancel", "[\"session-id\"]"));
    failed(call(owner, "chunk", "[]"));
    failed(call(owner, "chunk", "[\"wrong-id\",\"spice.exe\",0,\"TVo=\"]"));
    failed(call(owner, "chunk", "[\"session-id\",\"spice.exe\",-1,\"TVo=\"]"));
    failed(call(owner, "chunk", "[\"session-id\",\"spice.exe\",0,\"####\"]"));
    failed(call(owner, "chunk", "[\"session-id\",\"spice.exe\",0,\"A\"]"));
    failed(call(owner, "chunk", "[\"session-id\",\"spice.exe\",0,\"\"]"));
    assert(ota::chunks == 0);
    const auto chunk = call(owner, "chunk", "[\"session-id\",\"spice.exe\",0,\"TVo=\"]");
    assert(chunk["data"][0]["received"].GetUint() == 2);
    ota::fail = true;
    failed(call(owner, "finish", "[\"session-id\"]"));
    failed(call(owner, "apply", "[\"session-id\"]"));
    ota::fail = false;
    const auto finish = call(owner, "finish", "[\"session-id\"]");
    assert(std::string(finish["data"][0]["files"][0]["signature"]["status"].GetString()) == "invalid");
    assert(call(owner, "apply", "[\"session-id\"]")["data"][0]["restarting"].GetBool());
    assert(ota::applies == 1);
    failed(call(owner, "apply", "[\"session-id\"]"));
    assert(call(owner, "begin", files)["errors"].Empty());
    ota::fail = true;
    failed(call(owner, "cancel", "[\"session-id\"]"));
    failed(call(owner, "begin", files));
    ota::fail = false;
    assert(call(owner, "cancel", "[\"session-id\"]")["errors"].Empty());
    assert(ota::active.empty());
    {
        api::modules::Update disconnected;
        assert(call(disconnected, "begin", files)["errors"].Empty());
    }
    assert(ota::active.empty());
    assert(ota::cancels == 3);
    std::cout << "OTA API tests passed: validation, ownership, cleanup and advisory signatures.\n";
}
