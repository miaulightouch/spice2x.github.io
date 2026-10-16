#include "buttons_portable.h"

#include <cassert>
#include <chrono>
#include <iostream>

rawinput::RawInputManager *RI_MGR = nullptr;
static std::vector<Button> BUTTONS {Button("Service"), Button("Test")};
static std::string GAME = "Button Test";
static int POLL_COUNT = 0;

namespace games {
    std::vector<Button> *get_buttons(const std::string &) {
        return &BUTTONS;
    }
}

const std::string &eamuse_get_game() {
    return GAME;
}

void mdxf_poll(bool) {
    ++POLL_COUNT;
}

float GameAPI::Buttons::getVelocity(rawinput::RawInputManager *, Button &button) {
    return button.override_enabled ? button.override_velocity : 0.f;
}

static rapidjson::Document call(api::modules::Buttons &buttons, const char *params) {
    rapidjson::Document request_document;
    const std::string request =
            std::string("{\"id\":1,\"module\":\"buttons\",\"function\":\"pulse\",\"params\":")
            + params + "}";
    request_document.Parse(request.c_str());
    api::Request request_object(request_document);
    api::Response response(1);
    buttons.handle(request_object, response);
    rapidjson::Document result;
    result.Parse(response.get_string().c_str());
    return result;
}

int main() {
    api::modules::Buttons buttons;

    POLL_COUNT = 0;
    auto result = call(buttons, "[\"Service\",1]");
    assert(result["errors"].Empty());
    assert(!BUTTONS[0].override_enabled);
    assert(POLL_COUNT == 2);

    BUTTONS[0].override_enabled = true;
    BUTTONS[0].override_state = GameAPI::Buttons::BUTTON_PRESSED;
    BUTTONS[0].override_velocity = 1.f;
    POLL_COUNT = 0;
    result = call(buttons, "[\"Service\",1]");
    assert(!result["errors"].Empty());
    assert(BUTTONS[0].override_enabled);
    assert(POLL_COUNT == 0);
    BUTTONS[0].override_enabled = false;

    for (const char *params : {"[\"Missing\",1]", "[\"Service\",0]", "[\"Service\",2001]", "[\"Service\",1.5]", "[123,1]", "[\"Service\",1,2]"}) {
        POLL_COUNT = 0;
        result = call(buttons, params);
        assert(!result["errors"].Empty());
        assert(!BUTTONS[0].override_enabled);
        assert(POLL_COUNT == 0);
    }

    const auto started = std::chrono::steady_clock::now();
    result = call(buttons, "[\"Test\"]");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
    assert(result["errors"].Empty());
    assert(!BUTTONS[1].override_enabled);
    assert(elapsed >= 150);
    std::cout << "Buttons pulse tests passed: bounded release, default duration, validation and override protection.\n";
}
