#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "api/modules/resize.h"
#include "cfg/screen_resize.h"
#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"
#include "portable.h"

using namespace rapidjson;

static int32_t checks = 0;

static void check(bool passed, int32_t line) {
    ++checks;
    if (!passed) {
        std::cerr << "Resize API check failed at line " << line << '\n';
        std::exit(1);
    }
}

#define CHECK(condition) check((condition), __LINE__)

static Document call(api::modules::Resize &module, const std::string &function,
        const std::string &params = "[]") {
    Document input;
    const auto json = "{\"id\":7,\"module\":\"resize\",\"function\":\"" + function +
        "\",\"params\":" + params + "}";
    input.Parse(json.c_str());
    if (input.HasParseError()) {
        std::abort();
    }
    api::Request request(input);
    api::Response response(request.id);
    module.handle(request, response);
    Document output;
    output.Parse(response.get_string().c_str());
    return output;
}

static std::string state_json(api::modules::Resize &module) {
    auto state = call(module, "image_resize_get");
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    state["data"][0].Accept(writer);
    return buffer.GetString();
}

static bool near(double actual, double expected) {
    return std::abs(actual - expected) < 0.00001;
}

int main() {
    api::modules::Resize module;
    for (const auto *function : {"image_resize_get", "image_resize_set",
            "image_resize_save", "image_resize_enable", "image_resize_set_scene"}) {
        auto result = call(module, function);
        CHECK(result["errors"].Size() == 1);
    }

    cfg::SCREEN_RESIZE_CFG_PATH_OVERRIDE = "resize-test.json";
    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    auto result = call(module, "image_resize_get");
    CHECK(result["id"].GetInt() == 7);
    CHECK(result["errors"].Empty());
    CHECK(result["data"][0]["scenes"].Size() == 4);
    CHECK(result["data"][0]["scene"].GetInt() == 1);
    CHECK(!result["data"][0]["enabled"].GetBool());
    CHECK(module.requires_password("image_resize_save"));
    CHECK(!module.requires_password("image_resize_get"));
    CHECK(!module.requires_password("image_resize_set"));

    result = call(module, "image_resize_set", R"([{
        "scene":2,"enabled":true,"linear_filter":false,"offset_x":8,"offset_y":-4,
        "keep_aspect_ratio":false,"scale_x":0.97,"scale_y":1.02,"duplicate":2
    }])");
    CHECK(result["errors"].Empty());
    CHECK(result["data"][0]["scene"].GetInt() == 2);
    CHECK(result["data"][0]["enabled"].GetBool());
    CHECK(!result["data"][0]["linear_filter"].GetBool());
    const auto &scene = result["data"][0]["scenes"][1];
    CHECK(scene["offset_x"].GetInt() == 8);
    CHECK(scene["offset_y"].GetInt() == -4);
    CHECK(near(scene["scale_x"].GetDouble(), 0.97));
    CHECK(near(scene["scale_y"].GetDouble(), 1.02));
    CHECK(scene["duplicate"].GetInt() == 2);
    CHECK(!scene["keep_aspect_ratio"].GetBool());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    CHECK(result["data"][0]["scenes"][0]["offset_x"].GetInt() == 0);

    result = call(module, "image_resize_set", R"([{"offset_x":9}])");
    CHECK(result["errors"].Empty());
    CHECK(result["data"][0]["scenes"][1]["offset_x"].GetInt() == 9);
    CHECK(near(result["data"][0]["scenes"][1]["scale_y"].GetDouble(), 1.02));

    result = call(module, "image_resize_set",
        R"([{"keep_aspect_ratio":true,"scale_x":1.2}])");
    CHECK(result["errors"].Empty());
    CHECK(near(result["data"][0]["scenes"][1]["scale_y"].GetDouble(), 1.2));
    result = call(module, "image_resize_set", R"([{"scale_y":1.3}])");
    CHECK(near(result["data"][0]["scenes"][1]["scale_x"].GetDouble(), 1.3));

    const auto before = state_json(module);
    const std::vector<std::string> invalid = {
        "[]", "[null]", "[true]", "[[]]", "[1]",
        R"([{"scene":0}])", R"([{"scene":5}])", R"([{"scene":-1}])",
        R"([{"scene":1.5}])", R"([{"enabled":1}])", R"([{"linear_filter":0}])",
        R"([{"keep_aspect_ratio":"true"}])", R"([{"offset_x":0.5}])",
        R"([{"offset_x":32768}])", R"([{"offset_y":-32769}])",
        R"([{"offset_x":4294967295}])", R"([{"scale_x":0}])",
        R"([{"scale_x":-1}])", R"([{"scale_x":0.599}])",
        R"([{"scale_y":2.501}])", R"([{"scale_x":1e300}])",
        R"([{"scale_y":false}])", R"([{"duplicate":3}])",
        R"([{"duplicate":-1}])", R"([{"duplicate":1.5}])",
        R"([{"scale_x":1.1,"scale_y":1.2}])", R"([{"unknown":1}])",
        R"([{"scene":4,"enabled":false,"offset_x":200,"scale_y":0}])",
    };
    for (const auto &params : invalid) {
        result = call(module, "image_resize_set", params);
        CHECK(result["errors"].Size() == 1);
        CHECK(result["data"].Empty());
        CHECK(state_json(module) == before);
    }

    result = call(module, "image_resize_set",
        R"([{"scale_x":0.6,"offset_x":-32768,"offset_y":32767}])");
    CHECK(result["errors"].Empty());
    result = call(module, "image_resize_set", R"([{"scale_y":2.5}])");
    CHECK(result["errors"].Empty());

    result = call(module, "image_resize_set_scene", "[0]");
    CHECK(result["errors"].Empty() && result["data"].Empty());
    CHECK(!cfg::SCREENRESIZE->enable_screen_resize);
    result = call(module, "image_resize_set", R"([{"scene":3,"offset_x":10}])");
    CHECK(!result["data"][0]["enabled"].GetBool());
    result = call(module, "image_resize_enable", "[true]");
    CHECK(result["errors"].Empty() && result["data"].Empty());
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 2);
    result = call(module, "image_resize_set_scene", "[4]");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 3);
    for (const auto *params : {"[]", "[5]", "[-1]", "[false]"}) {
        CHECK(call(module, "image_resize_set_scene", params)["errors"].Size() == 1);
    }
    CHECK(call(module, "image_resize_enable", "[1]")["errors"].Size() == 1);

    // Exercise actual config serialization, keeping existing window/other-game keys.
    std::ofstream("resize-test.json") << R"({"marker":42,"sp2x_games":{
        "Resize API Test":{"w_width":1600,"w_height":900,"custom":"keep"},
        "Other Game":{"w_width":777}}})";
    cfg::SCREENRESIZE->client_width = 123;
    result = call(module, "image_resize_save");
    CHECK(result["errors"].Empty());
    Document saved;
    saved.Parse(fileutils::text_read("resize-test.json").c_str());
    const auto &saved_game = saved["sp2x_games"]["Resize API Test"];
    CHECK(saved["marker"].GetInt() == 42);
    CHECK(saved_game["w_width"].GetInt() == 1600);
    CHECK(saved_game["w_height"].GetInt() == 900);
    CHECK(std::string(saved_game["custom"].GetString()) == "keep");
    CHECK(saved["sp2x_games"]["Other Game"]["w_width"].GetInt() == 777);
    CHECK(saved_game["scenes"].Size() == 4);
    CHECK(saved_game["scenes"][2]["offset_x"].GetInt() == 10);
    resize_test_write_failure = true;
    CHECK(call(module, "image_resize_save")["errors"].Size() == 1);
    resize_test_write_failure = false;

    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(cfg::SCREENRESIZE->enable_screen_resize);
    CHECK(!cfg::SCREENRESIZE->enable_linear_filter);
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 0);
    CHECK(cfg::SCREENRESIZE->scene_settings[2].offset_x == 10);
    CHECK(cfg::SCREENRESIZE->client_width == 1600);
    CHECK(near(cfg::SCREENRESIZE->scene_settings[1].scale_x, 2.5));

    // Concurrent API clients and the renderer's lock must observe complete updates.
    std::atomic<bool> failed = false;
    std::vector<std::thread> clients;
    for (int32_t i = 0; i < 4; ++i) {
        clients.emplace_back([&failed] {
            api::modules::Resize client;
            for (int32_t n = 0; n < 100; ++n) {
                auto update = call(client, "image_resize_set",
                    n % 2 ? R"([{"scene":1,"scale_x":1.1}])" :
                            R"([{"scene":1,"scale_y":1.2}])");
                if (!update["errors"].Empty()) {
                    failed = true;
                }
                std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
                const auto &current = cfg::SCREENRESIZE->scene_settings[0];
                if (current.scale_x != current.scale_y) {
                    failed = true;
                }
            }
        });
    }
    for (auto &client : clients) {
        client.join();
    }
    CHECK(!failed);
    std::cout << checks << " resize API checks passed; 400 concurrent updates passed.\n";
}
