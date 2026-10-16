#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
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

static void wait_for_resize_write() {
    std::unique_lock<std::mutex> lock(resize_test_io_mutex);
    CHECK(resize_test_io_cv.wait_for(lock, std::chrono::seconds(2), [] {
        return resize_test_write_started;
    }));
}

int main() {
    api::modules::Resize module;
    for (const auto *function : {"image_resize_get", "image_resize_set",
            "image_resize_save", "image_resize_enable", "image_resize_set_scene"}) {
        auto result = call(module, function);
        CHECK(result["errors"].Size() == 1);
    }

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

    cfg::SCREENRESIZE->need_surface_clean = false;
    result = call(module, "image_resize_set", R"([{}])");
    CHECK(result["errors"].Empty());
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = true;
    result = call(module, "image_resize_set", R"([{}])");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    result = call(module, "image_resize_set", R"([{"offset_x":9}])");
    CHECK(result["errors"].Empty());
    CHECK(result["data"][0]["scenes"][1]["offset_x"].GetInt() == 9);
    CHECK(near(result["data"][0]["scenes"][1]["scale_y"].GetDouble(), 1.02));
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    result = call(module, "image_resize_set", R"([{"offset_x":9}])");
    CHECK(result["errors"].Empty());
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);

    result = call(module, "image_resize_set", R"([{"linear_filter":true}])");
    CHECK(result["errors"].Empty());
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    result = call(module, "image_resize_set", R"([{"linear_filter":false}])");
    CHECK(result["errors"].Empty());
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    result = call(module, "image_resize_set", R"([{"duplicate":1}])");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    result = call(module, "image_resize_set", R"([{"duplicate":2}])");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    result = call(module, "image_resize_set",
        R"([{"keep_aspect_ratio":true,"scale_x":1.2}])");
    CHECK(result["errors"].Empty());
    CHECK(near(result["data"][0]["scenes"][1]["scale_y"].GetDouble(), 1.2));
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    result = call(module, "image_resize_set", R"([{"scale_y":1.3}])");
    CHECK(near(result["data"][0]["scenes"][1]["scale_x"].GetDouble(), 1.3));
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);

    result = call(module, "image_resize_set", R"([{"scene":3}])");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    result = call(module, "image_resize_set", R"([{"scene":2}])");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;

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
    CHECK(!cfg::SCREENRESIZE->need_surface_clean);
    result = call(module, "image_resize_set", R"([{"scene":3,"offset_x":10}])");
    CHECK(!result["data"][0]["enabled"].GetBool());
    result = call(module, "image_resize_enable", "[true]");
    CHECK(result["errors"].Empty() && result["data"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 2);
    result = call(module, "image_resize_set_scene", "[4]");
    CHECK(result["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    cfg::SCREENRESIZE->need_surface_clean = false;
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 3);
    for (const auto *params : {"[]", "[5]", "[-1]", "[false]"}) {
        CHECK(call(module, "image_resize_set_scene", params)["errors"].Size() == 1);
    }
    CHECK(call(module, "image_resize_enable", "[1]")["errors"].Size() == 1);

    // Exercise actual config serialization, keeping existing window/other-game keys.
    std::ofstream("spicetools_screen_resize.json") << R"({"marker":42,"sp2x_games":{
        "Resize API Test":{"w_width":1600,"w_height":900,"custom":"keep"},
        "Other Game":{"w_width":777}}})";
    cfg::SCREENRESIZE->client_width = 123;
    result = call(module, "image_resize_save");
    CHECK(result["errors"].Empty());
    Document saved;
    saved.Parse(fileutils::text_read("spicetools_screen_resize.json").c_str());
    const auto &saved_game = saved["sp2x_games"]["Resize API Test"];
    CHECK(saved["marker"].GetInt() == 42);
    CHECK(saved_game["w_width"].GetInt() == 1600);
    CHECK(saved_game["w_height"].GetInt() == 900);
    CHECK(std::string(saved_game["custom"].GetString()) == "keep");
    CHECK(saved["sp2x_games"]["Other Game"]["w_width"].GetInt() == 777);
    CHECK(saved_game["scenes"].Size() == 4);
    CHECK(saved_game["screen_resize_current_scene"].GetInt() == 3);
    CHECK(saved_game["scenes"][2]["offset_x"].GetInt() == 10);
    resize_test_write_failure = true;
    CHECK(call(module, "image_resize_save")["errors"].Size() == 1);
    resize_test_write_failure = false;

    // A full save snapshots every window setting while preserving the image state.
    cfg::SCREENRESIZE->window_always_on_top = true;
    cfg::SCREENRESIZE->enable_window_resize = true;
    cfg::SCREENRESIZE->client_keep_aspect_ratio = false;
    cfg::SCREENRESIZE->window_decoration = 2;
    cfg::SCREENRESIZE->client_width = 1600;
    cfg::SCREENRESIZE->client_height = 900;
    cfg::SCREENRESIZE->window_offset_x = -11;
    cfg::SCREENRESIZE->window_offset_y = 22;
    CHECK(cfg::SCREENRESIZE->config_save(false));
    saved.Parse(fileutils::text_read("spicetools_screen_resize.json").c_str());
    const auto &full_saved_game = saved["sp2x_games"]["Resize API Test"];
    CHECK(full_saved_game["w_always_on_top"].GetBool());
    CHECK(full_saved_game["w_enable_resize"].GetBool());
    CHECK(!full_saved_game["w_keep_aspect_ratio"].GetBool());
    CHECK(full_saved_game["w_border_type"].GetInt() == 2);
    CHECK(full_saved_game["w_width"].GetUint() == 1600);
    CHECK(full_saved_game["w_height"].GetUint() == 900);
    CHECK(full_saved_game["w_offset_x"].GetInt() == -11);
    CHECK(full_saved_game["w_offset_y"].GetInt() == 22);

    // Saving must release the drawing lock while its file write is blocked, and
    // concurrent saves must commit in snapshot order.
    call(module, "image_resize_set", R"([{"scene":4,"offset_x":10}])");
    {
        std::lock_guard<std::mutex> lock(resize_test_io_mutex);
        resize_test_block_write = true;
        resize_test_write_started = false;
        resize_test_write_count = 0;
    }
    std::atomic<bool> first_save_ok = false;
    std::thread first_save([&first_save_ok] {
        first_save_ok = cfg::SCREENRESIZE->config_save(true);
    });
    wait_for_resize_write();
    const bool drawing_lock_available = cfg::SCREENRESIZE->image_mutex.try_lock();
    CHECK(drawing_lock_available);
    if (drawing_lock_available) {
        cfg::SCREENRESIZE->image_mutex.unlock();
    }

    call(module, "image_resize_set", R"([{"scene":1,"offset_x":123}])");
    std::atomic<bool> second_save_ok = false;
    std::mutex second_start_mutex;
    std::condition_variable second_start_cv;
    bool second_save_started = false;
    std::thread second_save([&] {
        {
            std::lock_guard<std::mutex> lock(second_start_mutex);
            second_save_started = true;
        }
        second_start_cv.notify_one();
        second_save_ok = cfg::SCREENRESIZE->config_save(true);
    });
    {
        std::unique_lock<std::mutex> lock(second_start_mutex);
        CHECK(second_start_cv.wait_for(lock, std::chrono::seconds(2), [&second_save_started] {
            return second_save_started;
        }));
    }
    {
        std::unique_lock<std::mutex> lock(resize_test_io_mutex);
        const bool second_write_entered = resize_test_io_cv.wait_for(
            lock, std::chrono::milliseconds(500), [] {
                return resize_test_write_count >= 2;
            });
        CHECK(!second_write_entered);
        CHECK(resize_test_write_count == 1);
    }
    {
        std::lock_guard<std::mutex> lock(resize_test_io_mutex);
        resize_test_block_write = false;
        resize_test_write_started = false;
    }
    resize_test_io_cv.notify_all();
    first_save.join();
    second_save.join();
    CHECK(first_save_ok);
    CHECK(second_save_ok);
    CHECK(resize_test_write_count == 2);
    saved.Parse(fileutils::text_read("spicetools_screen_resize.json").c_str());
    CHECK(!saved.HasParseError() && saved.IsObject());
    CHECK(saved["sp2x_games"]["Resize API Test"]["screen_resize_current_scene"].GetInt() == 0);
    CHECK(saved["sp2x_games"]["Resize API Test"]["scenes"][0]["offset_x"].GetInt() == 123);

    // Restore the state used by the following load assertions.
    CHECK(call(module, "image_resize_set", R"([{"scene":4,"offset_x":0}])")["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->config_save(true));

    // A load must wait for a save that already owns the config I/O sequence.
    CHECK(call(module, "image_resize_set", R"([{"scene":1,"offset_x":321}])")["errors"].Empty());
    {
        std::lock_guard<std::mutex> lock(resize_test_io_mutex);
        resize_test_block_write = true;
        resize_test_write_started = false;
        resize_test_write_count = 0;
    }
    std::atomic<bool> blocked_save_ok = false;
    std::thread blocked_save([&blocked_save_ok] {
        blocked_save_ok = cfg::SCREENRESIZE->config_save(true);
    });
    wait_for_resize_write();

    std::mutex load_start_mutex;
    std::condition_variable load_start_cv;
    bool load_started = false;
    std::mutex load_done_mutex;
    std::condition_variable load_done_cv;
    bool load_done = false;
    std::thread load([&] {
        {
            std::lock_guard<std::mutex> lock(load_start_mutex);
            load_started = true;
        }
        load_start_cv.notify_one();
        cfg::SCREENRESIZE->config_load();
        {
            std::lock_guard<std::mutex> lock(load_done_mutex);
            load_done = true;
        }
        load_done_cv.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(load_start_mutex);
        CHECK(load_start_cv.wait_for(lock, std::chrono::seconds(2), [&load_started] {
            return load_started;
        }));
    }
    bool load_finished_while_save_blocked;
    {
        std::unique_lock<std::mutex> lock(load_done_mutex);
        load_finished_while_save_blocked = load_done_cv.wait_for(
            lock, std::chrono::milliseconds(500), [&load_done] {
                return load_done;
            });
    }
    {
        std::lock_guard<std::mutex> lock(resize_test_io_mutex);
        resize_test_block_write = false;
        resize_test_write_started = false;
    }
    resize_test_io_cv.notify_all();
    blocked_save.join();
    load.join();
    CHECK(blocked_save_ok);
    CHECK(!load_finished_while_save_blocked);
    {
        std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
        CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 0);
        CHECK(cfg::SCREENRESIZE->scene_settings[0].offset_x == 321);
    }

    CHECK(call(module, "image_resize_set", R"([{"scene":4,"offset_x":0}])")["errors"].Empty());
    CHECK(cfg::SCREENRESIZE->config_save(true));

    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(cfg::SCREENRESIZE->enable_screen_resize);
    CHECK(cfg::SCREENRESIZE->need_surface_clean);
    CHECK(!cfg::SCREENRESIZE->enable_linear_filter);
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 3);
    CHECK(cfg::SCREENRESIZE->scene_settings[2].offset_x == 10);
    CHECK(cfg::SCREENRESIZE->client_width == 1600);
    CHECK(near(cfg::SCREENRESIZE->scene_settings[1].scale_x, 2.5));

    // Disabled resize still remembers the selected scene and its parameters.
    call(module, "image_resize_set", R"([{"scene":2,"enabled":false}])");
    CHECK(call(module, "image_resize_save")["errors"].Empty());
    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(!cfg::SCREENRESIZE->enable_screen_resize);
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 1);
    CHECK(near(cfg::SCREENRESIZE->scene_settings[1].scale_x, 2.5));

    // Legacy and malformed configs must never produce an invalid renderer index.
    for (const auto *value : {"-1", "4", "256", "4294967296", "1.5", "true", "\"3\"", "null"}) {
        std::ofstream("spicetools_screen_resize.json") <<
            R"({"sp2x_games":{"Resize API Test":{"screen_resize_current_scene":)" << value << "}}}";
        cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
        CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 0);
    }
    std::ofstream("spicetools_screen_resize.json") << R"({"sp2x_games":{"Resize API Test":{}}})";
    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 0);

    // Keep the legacy global fallback, with per-game settings taking precedence.
    std::ofstream("spicetools_screen_resize.json") << R"({"screen_resize_current_scene":2})";
    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 2);
    std::ofstream("spicetools_screen_resize.json") << R"({"screen_resize_current_scene":2,
        "sp2x_games":{"Resize API Test":{"screen_resize_current_scene":1}}})";
    cfg::SCREENRESIZE = std::make_unique<cfg::ScreenResize>();
    CHECK(cfg::SCREENRESIZE->screen_resize_current_scene == 1);

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
