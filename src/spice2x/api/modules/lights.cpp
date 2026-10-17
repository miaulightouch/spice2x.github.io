#include "lights.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <cfg/configurator.h>

#include "external/rapidjson/document.h"
#include "misc/eamuse.h"
#include "cfg/light.h"
#include "launcher/launcher.h"
#include "games/io.h"
#include "util/scope_guard.h"
#include "util/utils.h"

using namespace std::placeholders;
using namespace rapidjson;


namespace api::modules {

    namespace {
        constexpr unsigned int DEFAULT_PULSE_MS = 180;
        constexpr unsigned int MAX_PULSE_MS = 2000;
        constexpr SizeType MAX_PULSE_NAME = 4096;
        std::mutex LIGHTS_MUTEX;

        bool read_pulse_name(const Value &value, std::string &name) {
            if (!value.IsString() || value.GetStringLength() > MAX_PULSE_NAME) {
                return false;
            }
            name.assign(value.GetString(), value.GetStringLength());
            return name.find('\0') == std::string::npos;
        }
    }

    Lights::Lights() : Module("lights") {
        functions["read"] = std::bind(&Lights::read, this, _1, _2);
        functions["write"] = std::bind(&Lights::write, this, _1, _2);
        functions["write_reset"] = std::bind(&Lights::write_reset, this, _1, _2);
        functions["pulse"] = std::bind(&Lights::pulse, this, _1, _2);

        this->lights = games::get_lights(eamuse_get_game());
        if (this->lights) {
            for (auto &light : *this->lights) {
                this->lights_by_names.emplace(light.getName(), light);
            }
        }
    }

    /**
     * read()
     * read(name: str, ...)
     */
    void Lights::read(api::Request &req, Response &res) {

        std::lock_guard lock(LIGHTS_MUTEX);

        // check light cache
        if (!this->lights) {
            return;
        }

        // all lights for this game
        if (req.params.Size() == 0) {
            // add state for each light
            for (auto &light : *this->lights) {
                get_light(light, res);
            }

            return;
        }

        // specified light names
        for (Value &param : req.params.GetArray()) {
            // check params
            if (!param.IsString()) {
                error_type(res, "name", "string");
                return;
            }
            const auto name = param.GetString();
            if (this->lights_by_names.contains(name)) {
                get_light(this->lights_by_names.at(name).get(), res);
            }
        }
    }

    void Lights::get_light(Light &light, Response &res) {
        Value state(kArrayType);
        Value light_name(light.getName().c_str(), res.doc()->GetAllocator());
        Value light_state(GameAPI::Lights::readLight(RI_MGR, light));
        Value light_enabled(light.override_enabled);
        state.PushBack(light_name, res.doc()->GetAllocator());
        state.PushBack(light_state, res.doc()->GetAllocator());
        state.PushBack(light_enabled, res.doc()->GetAllocator());
        res.add_data(state);
    }

    /**
     * write([name: str, state: float], ...)
     */
    void Lights::write(Request &req, Response &res) {

        std::lock_guard lock(LIGHTS_MUTEX);

        // check light cache
        if (!this->lights) {
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
            if (!param[1].IsFloat() && !param[1].IsInt()) {
                error_type(res, "state", "float");
                continue;
            }

            // get params
            auto light_name = param[0].GetString();
            auto light_state = param[1].GetFloat();

            // write light state
            if (!this->write_light(light_name, light_state)) {
                error_unknown(res, "light", light_name);
                continue;
            }
        }
    }

    /**
     * write_reset()
     * write_reset(name: str, ...)
     * write_reset([name: str], ...)
     */
    void Lights::write_reset(Request &req, Response &res) {

        std::lock_guard lock(LIGHTS_MUTEX);

        // check light cache
        if (!this->lights) {
            return;
        }

        // get params
        auto params = req.params.GetArray();

        // write_reset()
        if (params.Size() == 0) {
            if (lights != nullptr) {
                for (auto &light : *this->lights) {
                    if (light.override_enabled) {
                        light.override_enabled = false;
                        if (cfg::CONFIGURATOR_STANDALONE && RI_MGR) {
                            GameAPI::Lights::writeLight(RI_MGR, light, light.last_state);
                        }
                    }
                }
            }
            return;
        }

        // loop parameters
        for (Value &param : req.params.GetArray()) {
            const char* light_name = nullptr;

            // check params
            if (param.IsArray()) {
                if (param.Size() < 1) {
                    error_params_insufficient(res);
                    continue;
                }
                if (!param[0].IsString()) {
                    error_type(res, "name", "string");
                    continue;
                }
                // get params
                light_name = param[0].GetString();
            } else if (param.IsString()) {
                light_name = param.GetString();
            } else {
                error(res, "parameters must be arrays or strings");
            }

            // write analog state
            if (light_name && !this->write_light_reset(light_name)) {
                error_unknown(res, "analog", light_name);
                continue;
            }
        }
    }

    /**
     * pulse(name: str, value: float, duration_ms: uint = 180)
     * A single request owns the override and its restoration, so disconnects cannot leave it active.
     */
    void Lights::pulse(Request &req, Response &res) {
        if (req.params.Size() < 2 || req.params.Size() > 3) {
            return error(res, "pulse expects [name, value, duration_ms]");
        }

        std::string light_name;
        if (!read_pulse_name(req.params[0], light_name)) {
            return error_type(res, "name", "string without embedded NUL");
        }
        if (!req.params[1].IsNumber()) {
            return error_type(res, "value", "finite number between 0 and 1");
        }
        const auto value = req.params[1].GetDouble();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return error(res, "value must be a finite number between 0 and 1");
        }

        unsigned int duration = DEFAULT_PULSE_MS;
        if (req.params.Size() == 3) {
            if (!req.params[2].IsUint()) {
                return error_type(res, "duration_ms", "uint");
            }
            duration = req.params[2].GetUint();
        }
        if (duration < 1 || duration > MAX_PULSE_MS) {
            return error(res, "duration_ms must be between 1 and 2000");
        }

        std::lock_guard lock(LIGHTS_MUTEX);
        if (!this->lights) {
            return error(res, "lights unavailable");
        }
        if (!this->lights_by_names.contains(light_name)) {
            return error_unknown(res, "light", light_name);
        }

        auto &light = this->lights_by_names.at(light_name).get();
        const auto previous_state = light.override_state;
        const auto previous_enabled = light.override_enabled;
        const auto previous_last_state = light.last_state;
        const auto restore = scope_guard{[&light, previous_state, previous_enabled, previous_last_state] {
            light.override_state = previous_state;
            light.override_enabled = previous_enabled;
            if (cfg::CONFIGURATOR_STANDALONE && RI_MGR) {
                GameAPI::Lights::writeLight(RI_MGR, light,
                    previous_enabled ? previous_state : previous_last_state);
                RI_MGR->devices_flush_output();
                light.last_state = previous_last_state;
            }
        }};
        light.override_state = static_cast<float>(value);
        light.override_enabled = true;
        if (cfg::CONFIGURATOR_STANDALONE && RI_MGR) {
            GameAPI::Lights::writeLight(RI_MGR, light, static_cast<float>(value));
            RI_MGR->devices_flush_output();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    }

    bool Lights::write_light(std::string name, float state) {

        // check light cache
        if (!this->lights) {
            return false;
        }

        // find light
        if (this->lights_by_names.contains(name)) {
            auto &light = this->lights_by_names.at(name).get();
            light.override_state = CLAMP(state, 0.f, 1.f);
            light.override_enabled = true;

            if (cfg::CONFIGURATOR_STANDALONE && RI_MGR) {
                GameAPI::Lights::writeLight(RI_MGR, light, state);
            }

            return true;
        } else {
            // unknown light
            return false;
        }
    }

    bool Lights::write_light_reset(std::string name) {

        // check light cache
        if (!this->lights) {
            return false;
        }

        // find light
        if (this->lights_by_names.contains(name)) {
            auto &light = this->lights_by_names.at(name).get();
            light.override_enabled = false;
            if (cfg::CONFIGURATOR_STANDALONE && RI_MGR) {
                GameAPI::Lights::writeLight(RI_MGR, light, light.last_state);
            }
            return true;
        } else {
            // unknown light
            return false;
        }
    }
}
