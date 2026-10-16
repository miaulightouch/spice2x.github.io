#include "info_portable.h"
#include "api/modules/info.h"
#include "external/rapidjson/document.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace rapidjson;

static int checks = 0;

static void check(bool passed, int line) {
    ++checks;
    if (!passed) {
        std::cerr << "Info API check failed at line " << line << '\n';
        std::exit(1);
    }
}

#define CHECK(condition) check((condition), __LINE__)

static Document call(api::modules::Info &module, const std::string &function,
        const std::string &params = "[]") {
    Document input;
    const auto json = "{\"id\":7,\"module\":\"info\",\"function\":\"" + function +
        "\",\"params\":" + params + "}";
    input.Parse(json.c_str());
    CHECK(!input.HasParseError());
    api::Request request(input);
    api::Response response(request.id);
    module.handle(request, response);
    Document output;
    output.Parse(response.get_string().c_str());
    CHECK(!output.HasParseError());
    return output;
}

static void check_error(const Document &result) {
    CHECK(result["errors"].Size() == 1);
    CHECK(result["data"].Empty());
}

static void check_chunk(const Document &result, uint64_t offset, uint64_t next,
        uint64_t size, const char *encoded) {
    CHECK(result["errors"].Empty());
    CHECK(result["data"].Size() == 1);
    const auto &data = result["data"][0];
    CHECK(data["offset"].GetUint64() == offset);
    CHECK(data["next"].GetUint64() == next);
    CHECK(data["size"].GetUint64() == size);
    CHECK(std::string(data["data"].GetString()) == encoded);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const auto fixture = std::filesystem::path(argv[1]);
    const auto active = fixture / "active.log";
    const auto missing = fixture / "missing.log";
    std::filesystem::create_directories(fixture);

    api::modules::Info module;
    CHECK(module.requires_password("log"));
    CHECK(!module.requires_password("avs"));
    CHECK(!module.requires_password("launcher"));
    CHECK(!module.requires_password("memory"));
    check_error(call(module, "unknown"));

    {
        std::ofstream output(active, std::ios::binary);
        output << "0123456789";
    }
    LOG_FILE_PATH = active.string();
    info_open_calls = info_close_calls = info_seek_calls = info_read_calls = 0;
    info_fail_size = info_fail_seek = info_fail_read = false;
    info_partial_read = static_cast<std::size_t>(-1);

    check_chunk(call(module, "log"), 0, 10, 10, "MDEyMzQ1Njc4OQ==");
    check_chunk(call(module, "log", "[null,4]"), 6, 10, 10, "Njc4OQ==");
    check_chunk(call(module, "log", "[2,4]"), 2, 6, 10, "MjM0NQ==");
    check_chunk(call(module, "log", "[0,1]"), 0, 1, 10, "MA==");
    check_chunk(call(module, "log", "[10,4]"), 10, 10, 10, "");
    check_chunk(call(module, "log", "[0,262144]"), 0, 10, 10, "MDEyMzQ1Njc4OQ==");
    CHECK(info_open_calls == info_close_calls);

    info_partial_read = 3;
    check_chunk(call(module, "log", "[2,8]"), 2, 5, 10, "MjM0");
    CHECK(info_open_calls == info_close_calls);
    info_partial_read = static_cast<std::size_t>(-1);

    const auto reads_before_bounds = info_read_calls;
    for (const auto *params : {
            "[0,0]", "[0,262145]", "[0,1.5]", "[0,-1]", "[0,true]",
            "[\"/tmp/secret\",1]", "[0,1,2]", "[1.5]", "[-1]",
            "[18446744073709551615,1]", "[11,1]"}) {
        check_error(call(module, "log", params));
    }
    CHECK(info_read_calls == reads_before_bounds);
    CHECK(info_open_calls == info_close_calls);

    info_fail_size = true;
    check_error(call(module, "log", "[0,4]"));
    CHECK(info_open_calls == info_close_calls);
    info_fail_size = false;
    info_fail_seek = true;
    check_error(call(module, "log", "[2,4]"));
    CHECK(info_open_calls == info_close_calls);
    info_fail_seek = false;
    info_fail_read = true;
    check_error(call(module, "log", "[2,4]"));
    CHECK(info_open_calls == info_close_calls);
    info_fail_read = false;

    {
        std::ofstream output(active, std::ios::binary | std::ios::trunc);
        const char bytes[] = "alpha\0\xff\nomega";
        output.write(bytes, sizeof(bytes) - 1);
    }
    check_chunk(call(module, "log"), 0, 13, 13, "YWxwaGEA/wpvbWVnYQ==");

    {
        std::ofstream output(active, std::ios::binary | std::ios::trunc);
    }
    check_chunk(call(module, "log"), 0, 0, 0, "");

    LOG_FILE_PATH = missing.string();
    check_error(call(module, "log"));
    LOG_FILE_PATH.clear();
    check_error(call(module, "log"));

    std::cout << "Native info tests passed: password gate, bounded log reads, tail/offset/limit, empty and binary data, partial reads, path safety, failure cleanup (" << checks << " checks).\n";
}
