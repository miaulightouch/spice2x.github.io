#include "io_pulse_test_portable.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

#include "analogs.h"
#include "lights.h"

std::unique_ptr<rawinput::RawInputManager> RI_MGR;
int io_pulse_flush_count = 0;
std::vector<float> io_pulse_light_outputs;
std::vector<Analog> ANALOGS {Analog("Axis"), Analog("Other")};
std::vector<Light> LIGHTS {Light("Lamp")};
std::string GAME = "Pulse Test";
int io_pulse_poll_count = 0;

namespace games {
std::vector<Analog> *get_analogs(const std::string &game) {
    return game == "missing" ? nullptr : &ANALOGS;
}

std::vector<Light> *get_lights(const std::string &game) {
    return game == "missing" ? nullptr : &LIGHTS;
}
}

const std::string &eamuse_get_game() {
    return GAME;
}

void mdxf_poll(bool) {
    ++io_pulse_poll_count;
}

static rapidjson::Document call_analogs(api::modules::Analogs &module, const char *params) {
    rapidjson::Document request_document;
    const std::string request_text = std::string(
        "{\"id\":1,\"module\":\"analogs\",\"function\":\"pulse\",\"params\":") + params + "}";
    request_document.Parse(request_text.c_str());
    assert(!request_document.HasParseError());
    api::Request request(request_document);
    api::Response response(1);
    module.handle(request, response);
    rapidjson::Document result;
    result.Parse(response.get_string().c_str());
    assert(!result.HasParseError());
    return result;
}

static rapidjson::Document call_lights(api::modules::Lights &module, const char *function, const char *params) {
    rapidjson::Document request_document;
    const std::string request_text = std::string(
        "{\"id\":1,\"module\":\"lights\",\"function\":\"") + function +
        "\",\"params\":" + params + "}";
    request_document.Parse(request_text.c_str());
    assert(!request_document.HasParseError());
    api::Request request(request_document);
    api::Response response(1);
    module.handle(request, response);
    rapidjson::Document result;
    result.Parse(response.get_string().c_str());
    assert(!result.HasParseError());
    return result;
}

int main() {
    api::modules::Analogs analogs;
    ANALOGS[0].override_state = 0.42f;
    ANALOGS[0].override_enabled = false;
    io_pulse_poll_count = 0;
    auto result = call_analogs(analogs, R"(["Axis",0.8,1])");
    assert(result["errors"].Empty());
    assert(!ANALOGS[0].override_enabled && std::abs(ANALOGS[0].override_state - 0.42f) < 0.0001f);
    assert(io_pulse_poll_count == 2);

    ANALOGS[0].override_state = 0.23f;
    ANALOGS[0].override_enabled = true;
    result = call_analogs(analogs, R"(["Axis",0.8,1])");
    assert(result["errors"].Empty());
    assert(ANALOGS[0].override_enabled && std::abs(ANALOGS[0].override_state - 0.23f) < 0.0001f);

    for (const char *params : {
            R"(["Axis",-0.1,1])", R"(["Axis",1.1,1])", R"(["Axis",true,1])",
            R"(["Axis",0.5,0])", R"(["Axis",0.5,2001])", R"(["Axis",0.5,1.5])",
            R"(["Axis",0.5,1,2])", R"(["Missing",0.5,1])", R"(["a\u0000b",0.5,1])"
        }) {
        io_pulse_poll_count = 0;
        const auto old_state = ANALOGS[0].override_state;
        const auto old_enabled = ANALOGS[0].override_enabled;
        result = call_analogs(analogs, params);
        assert(!result["errors"].Empty());
        assert(ANALOGS[0].override_state == old_state && ANALOGS[0].override_enabled == old_enabled);
        assert(io_pulse_poll_count == 0);
    }

    GAME = "missing";
    api::modules::Analogs missing_analogs;
    result = call_analogs(missing_analogs, R"(["Axis",0.5,1])");
    assert(!result["errors"].Empty());
    GAME = "Pulse Test";

    std::thread analog_pulse([&] {
        auto pulse_result = call_analogs(analogs, R"(["Axis",0.9,50])");
        assert(pulse_result["errors"].Empty());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto before_write = std::chrono::steady_clock::now();
    rapidjson::Document write_request;
    write_request.Parse(R"({"id":1,"module":"analogs","function":"write","params":[["Axis",0.3]]})");
    api::Request write_req(write_request);
    api::Response write_response(1);
    analogs.handle(write_req, write_response);
    const auto write_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before_write).count();
    analog_pulse.join();
    assert(write_elapsed >= 35);
    assert(ANALOGS[0].override_enabled && std::abs(ANALOGS[0].override_state - 0.3f) < 0.0001f);

    api::modules::Lights lights;
    cfg::CONFIGURATOR_STANDALONE = true;
    RI_MGR = std::make_unique<rawinput::RawInputManager>();
    LIGHTS[0].override_state = 0.17f;
    LIGHTS[0].override_enabled = false;
    LIGHTS[0].last_state = 0.31f;
    io_pulse_light_outputs.clear();
    io_pulse_flush_count = 0;
    result = call_lights(lights, "pulse", R"(["Lamp",0.9,1])");
    assert(result["errors"].Empty());
    assert(!LIGHTS[0].override_enabled && std::abs(LIGHTS[0].override_state - 0.17f) < 0.0001f);
    assert(std::abs(LIGHTS[0].last_state - 0.31f) < 0.0001f);
    assert(io_pulse_light_outputs.size() == 2);
    assert(std::abs(io_pulse_light_outputs[0] - 0.9f) < 0.0001f);
    assert(std::abs(io_pulse_light_outputs[1] - 0.31f) < 0.0001f);
    assert(io_pulse_flush_count == 2);

    LIGHTS[0].override_state = 0.22f;
    LIGHTS[0].override_enabled = true;
    LIGHTS[0].last_state = 0.44f;
    io_pulse_light_outputs.clear();
    io_pulse_flush_count = 0;
    result = call_lights(lights, "pulse", R"(["Lamp",0.7,1])");
    assert(result["errors"].Empty());
    assert(LIGHTS[0].override_enabled && std::abs(LIGHTS[0].override_state - 0.22f) < 0.0001f);
    assert(std::abs(LIGHTS[0].last_state - 0.44f) < 0.0001f);
    assert(io_pulse_light_outputs.size() == 2);
    assert(std::abs(io_pulse_light_outputs[1] - 0.22f) < 0.0001f);
    assert(io_pulse_flush_count == 2);

    for (const char *params : {
            R"(["Lamp",-0.1,1])", R"(["Lamp",1.1,1])", R"(["Lamp",true,1])",
            R"(["Lamp",0.5,0])", R"(["Lamp",0.5,2001])", R"(["Lamp",0.5,1.5])",
            R"(["Lamp",0.5,1,2])", R"(["Missing",0.5,1])", R"(["a\u0000b",0.5,1])"
        }) {
        io_pulse_light_outputs.clear();
        io_pulse_flush_count = 0;
        const auto old_state = LIGHTS[0].override_state;
        const auto old_enabled = LIGHTS[0].override_enabled;
        const auto old_last = LIGHTS[0].last_state;
        result = call_lights(lights, "pulse", params);
        assert(!result["errors"].Empty());
        assert(LIGHTS[0].override_state == old_state && LIGHTS[0].override_enabled == old_enabled);
        assert(LIGHTS[0].last_state == old_last && io_pulse_light_outputs.empty());
        assert(io_pulse_flush_count == 0);
    }

    cfg::CONFIGURATOR_STANDALONE = false;
    RI_MGR = std::make_unique<rawinput::RawInputManager>();
    LIGHTS[0].override_enabled = false;
    LIGHTS[0].last_state = 0.31f;
    io_pulse_light_outputs.clear();
    io_pulse_flush_count = 0;
    result = call_lights(lights, "pulse", R"(["Lamp",0.5,1])");
    assert(result["errors"].Empty());
    assert(io_pulse_light_outputs.size() == 2);
    assert(std::abs(io_pulse_light_outputs[0] - 0.5f) < 0.0001f);
    assert(std::abs(io_pulse_light_outputs[1] - 0.31f) < 0.0001f);
    assert(io_pulse_flush_count == 2);

    io_pulse_flush_count = 0;
    LIGHTS[0].override_enabled = false;
    LIGHTS[0].last_state = 0.11f;
    io_pulse_light_outputs.clear();
    std::atomic<bool> game_pulse_entered = false;
    std::thread game_update([&] {
        while (!game_pulse_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::atomic_ref<float>(LIGHTS[0].last_state).store(0.66f, std::memory_order_release);
    });
    game_pulse_entered.store(true, std::memory_order_release);
    result = call_lights(lights, "pulse", R"(["Lamp",0.5,100])");
    game_update.join();
    assert(result["errors"].Empty());
    assert(std::abs(LIGHTS[0].last_state - 0.66f) < 0.0001f);
    assert(io_pulse_light_outputs.size() == 2);
    assert(std::abs(io_pulse_light_outputs[0] - 0.5f) < 0.0001f);
    assert(std::abs(io_pulse_light_outputs[1] - 0.66f) < 0.0001f);
    assert(io_pulse_flush_count == 2);

    LIGHTS[0].override_enabled = false;
    LIGHTS[0].last_state = 0.11f;
    io_pulse_light_outputs.clear();
    io_pulse_flush_count = 0;
    game_pulse_entered.store(false, std::memory_order_release);
    std::thread equal_game_update([&] {
        while (!game_pulse_entered.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::atomic_ref<float>(LIGHTS[0].last_state).store(0.5f, std::memory_order_release);
    });
    game_pulse_entered.store(true, std::memory_order_release);
    result = call_lights(lights, "pulse", R"(["Lamp",0.5,100])");
    equal_game_update.join();
    assert(result["errors"].Empty());
    assert(std::abs(LIGHTS[0].last_state - 0.5f) < 0.0001f);
    assert(io_pulse_light_outputs.size() == 2);
    assert(std::abs(io_pulse_light_outputs[0] - 0.5f) < 0.0001f);
    assert(std::abs(io_pulse_light_outputs[1] - 0.5f) < 0.0001f);
    assert(io_pulse_flush_count == 2);

    RI_MGR.reset();
    io_pulse_flush_count = 0;
    result = call_lights(lights, "pulse", R"(["Lamp",0.5,1])");
    assert(result["errors"].Empty());
    assert(io_pulse_flush_count == 0);

    GAME = "missing";
    api::modules::Lights missing_lights;
    result = call_lights(missing_lights, "pulse", R"(["Lamp",0.5,1])");
    assert(!result["errors"].Empty());

    GAME = "Pulse Test";
    cfg::CONFIGURATOR_STANDALONE = false;
    LIGHTS[0].override_enabled = false;
    std::thread light_pulse([&] {
        auto pulse_result = call_lights(lights, "pulse", R"(["Lamp",0.8,50])");
        assert(pulse_result["errors"].Empty());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto light_write_start = std::chrono::steady_clock::now();
    result = call_lights(lights, "write", R"([["Lamp",0.2]])");
    const auto light_write_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - light_write_start).count();
    light_pulse.join();
    assert(result["errors"].Empty());
    assert(light_write_elapsed >= 35);
    assert(LIGHTS[0].override_enabled && std::abs(LIGHTS[0].override_state - 0.2f) < 0.0001f);

    std::cout << "Analog/light pulse tests passed: bounded validation, serialized writes, exact restoration, "
        "hardware restoration and missing game/manager safety.\n";
}
