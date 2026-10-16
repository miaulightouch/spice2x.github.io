#include "buttons.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include "external/rapidjson/document.h"
#include "misc/eamuse.h"
#include "cfg/button.h"
#include "launcher/launcher.h"
#include "games/io.h"
#include "util/utils.h"
#include "util/scope_guard.h"
#include "acio/mdxf/mdxf_poll.h"

using namespace std::placeholders;
using namespace rapidjson;


namespace api::modules {

    namespace {
        constexpr unsigned int DEFAULT_PULSE_MS = 180;
        constexpr unsigned int MAX_PULSE_MS = 2000;
        std::mutex BUTTONS_MUTEX;
    }

    Buttons::Buttons() : Module("buttons") {
        functions["read"] = std::bind(&Buttons::read, this, _1, _2);
        functions["write"] = std::bind(&Buttons::write, this, _1, _2);
        functions["write_reset"] = std::bind(&Buttons::write_reset, this, _1, _2);
        functions["pulse"] = std::bind(&Buttons::pulse, this, _1, _2);
        buttons = games::get_buttons(eamuse_get_game());
    }

    /**
     * read()
     */
    void Buttons::read(api::Request &req, Response &res) {

        std::lock_guard lock(BUTTONS_MUTEX);

        // check button cache
        if (!this->buttons) {
            return;
        }

        // add state for each button
        for (auto &button : *this->buttons) {
            Value state(kArrayType);
            Value button_name(button.getName().c_str(), res.doc()->GetAllocator());
            Value button_state(GameAPI::Buttons::getVelocity(RI_MGR, button));
            Value button_enabled(button.override_enabled);
            state.PushBack(button_name, res.doc()->GetAllocator());
            state.PushBack(button_state, res.doc()->GetAllocator());
            state.PushBack(button_enabled, res.doc()->GetAllocator());
            res.add_data(state);
        }
    }

    /**
     * write([name: str, state: bool/float], ...)
     */
    void Buttons::write(Request &req, Response &res) {

        std::lock_guard lock(BUTTONS_MUTEX);

        // check button cache
        if (!buttons) {
            return;
        }

        // loop parameters
        for (Value &param : req.params.GetArray()) {

            // check params
            if (!param.IsArray()) {
                error(res, "parameters must be arrays");
                return;
            }
            if (param.Size() < 2) {
                error_params_insufficient(res);
                continue;
            }
            if (!param[0].IsString()) {
                error_type(res, "name", "string");
                continue;
            }
            if (!param[1].IsBool() && !param[1].IsFloat() && !param[1].IsInt()) {
                error_type(res, "state", "bool or float");
                continue;
            }

            // get params
            auto button_name = param[0].GetString();
            auto button_state = param[1].IsBool() ? param[1].GetBool() : param[1].GetFloat() > 0;
            auto button_velocity = param[1].IsFloat() ? param[1].GetFloat() : (button_state ? 1.f : 0.f);

            // write button state
            if (!this->write_button(button_name, button_velocity)) {
                error_unknown(res, "button", button_name);
                continue;
            }
        }
    }

    /**
     * write_reset()
     * write_reset([name: str], ...)
     */
    void Buttons::write_reset(Request &req, Response &res) {

        std::lock_guard lock(BUTTONS_MUTEX);

        // check button cache
        if (!this->buttons) {
            return;
        }

        // get params
        auto params = req.params.GetArray();

        // write_reset()
        if (params.Size() == 0) {
            if (buttons != nullptr) {
                for (auto &button : *this->buttons) {
                    button.override_enabled = false;
                }
                mdxf_poll(true);
            }
            return;
        }

        // loop parameters
        for (Value &param : req.params.GetArray()) {

            // check params
            if (!param.IsArray()) {
                error(res, "parameters must be arrays");
                return;
            }
            if (param.Size() < 1) {
                error_params_insufficient(res);
                continue;
            }
            if (!param[0].IsString()) {
                error_type(res, "name", "string");
                continue;
            }

            // get params
            auto button_name = param[0].GetString();

            // write button state
            if (!this->write_button_reset(button_name)) {
                error_unknown(res, "button", button_name);
                continue;
            }
        }
    }

    /**
     * pulse(name: str, duration_ms: uint = 180)
     * A single request owns the press and release, so a disconnected client cannot leave it held.
     */
    void Buttons::pulse(Request &req, Response &res) {

        if (!this->buttons) {
            error(res, "buttons unavailable");
            return;
        }
        if (req.params.Size() < 1) {
            error_params_insufficient(res);
            return;
        }
        if (req.params.Size() > 2) {
            error(res, "pulse expects [name, duration_ms]");
            return;
        }
        if (!req.params[0].IsString()) {
            error_type(res, "name", "string");
            return;
        }

        unsigned int duration = DEFAULT_PULSE_MS;
        if (req.params.Size() == 2) {
            const auto &duration_param = req.params[1];
            if (!duration_param.IsUint() && !duration_param.IsInt()) {
                error_type(res, "duration_ms", "uint");
                return;
            }
            if (duration_param.IsUint()) {
                duration = duration_param.GetUint();
            } else if (duration_param.GetInt() > 0) {
                duration = static_cast<unsigned int>(duration_param.GetInt());
            } else {
                duration = 0;
            }
        }
        if (duration == 0 || duration > MAX_PULSE_MS) {
            error(res, "duration_ms must be between 1 and 2000");
            return;
        }

        const std::string button_name = req.params[0].GetString();
        std::lock_guard lock(BUTTONS_MUTEX);
        for (auto &button : *this->buttons) {
            if (button.getName() != button_name) {
                continue;
            }
            if (button.override_enabled) {
                error(res, "button is already overridden: " + button_name);
                return;
            }
            const auto release = scope_guard{[&button] {
                button.override_enabled = false;
                mdxf_poll(true);
            }};
            button.override_state = GameAPI::Buttons::BUTTON_PRESSED;
            button.override_velocity = 1.f;
            button.override_enabled = true;
            mdxf_poll(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            return;
        }
        error_unknown(res, "button", button_name);
    }

    bool Buttons::write_button(std::string name, float state) {

        // check button cache
        if (!this->buttons) {
            return false;
        }

        // find button
        for (auto &button : *this->buttons) {
            if (button.getName() == name) {
                button.override_state = state > 0.f ?
                        GameAPI::Buttons::BUTTON_PRESSED : GameAPI::Buttons::BUTTON_NOT_PRESSED;
                button.override_velocity = CLAMP(state, 0.f, 1.f);
                button.override_enabled = true;
                mdxf_poll(true);
                return true;
            }
        }

        // unknown button
        return false;
    }

    bool Buttons::write_button_reset(std::string name) {

        // check button cache
        if (!this->buttons) {
            return false;
        }

        // find button
        for (auto &button : *this->buttons) {
            if (button.getName() == name) {
                button.override_enabled = false;
                mdxf_poll(true);
                return true;
            }
        }

        // unknown button
        return false;
    }
}
