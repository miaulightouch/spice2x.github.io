#include "analogs.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include "external/rapidjson/document.h"
#include "misc/eamuse.h"
#include "cfg/analog.h"
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
        constexpr SizeType MAX_PULSE_NAME = 4096;
        std::mutex ANALOGS_MUTEX;

        bool read_pulse_name(const Value &value, std::string &name) {
            if (!value.IsString() || value.GetStringLength() > MAX_PULSE_NAME) {
                return false;
            }
            name.assign(value.GetString(), value.GetStringLength());
            return name.find('\0') == std::string::npos;
        }
    }

    Analogs::Analogs() : Module("analogs") {
        functions["read"] = std::bind(&Analogs::read, this, _1, _2);
        functions["write"] = std::bind(&Analogs::write, this, _1, _2);
        functions["write_reset"] = std::bind(&Analogs::write_reset, this, _1, _2);
        functions["pulse"] = std::bind(&Analogs::pulse, this, _1, _2);
        analogs = games::get_analogs(eamuse_get_game());
    }

    /**
     * read()
     */
    void Analogs::read(api::Request &req, Response &res) {

        std::lock_guard lock(ANALOGS_MUTEX);

        // check analog cache
        if (!analogs) {
            return;
        }

        // add state for each analog
        for (auto &analog : *this->analogs) {
            Value state(kArrayType);
            Value analog_name(analog.getName().c_str(), res.doc()->GetAllocator());
            Value analog_state(GameAPI::Analogs::getState(RI_MGR, analog));
            Value analog_enabled(analog.override_enabled);
            state.PushBack(analog_name, res.doc()->GetAllocator());
            state.PushBack(analog_state, res.doc()->GetAllocator());
            state.PushBack(analog_enabled, res.doc()->GetAllocator());
            res.add_data(state);
        }
    }

    /**
     * write([name: str, state: float], ...)
     */
    void Analogs::write(Request &req, Response &res) {

        std::lock_guard lock(ANALOGS_MUTEX);

        // check analog cache
        if (!analogs)
            return;

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
            auto analog_name = param[0].GetString();
            auto analog_state = param[1].GetFloat();

            // write analog state
            if (!this->write_analog(analog_name, analog_state)) {
                error_unknown(res, "analog", analog_name);
                continue;
            }
        }
    }

    /**
     * write_reset()
     * write_reset([name: str], ...)
     */
    void Analogs::write_reset(Request &req, Response &res) {

        std::lock_guard lock(ANALOGS_MUTEX);

        // check analog cache
        if (!analogs)
            return;

        // get params
        auto params = req.params.GetArray();

        // write_reset()
        if (params.Size() == 0) {
            if (analogs != nullptr) {
                for (auto &analog : *this->analogs) {
                    analog.override_enabled = false;
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
            auto analog_name = param[0].GetString();

            // write analog state
            if (!this->write_analog_reset(analog_name)) {
                error_unknown(res, "analog", analog_name);
                continue;
            }
        }
    }

    /**
     * pulse(name: str, value: float, duration_ms: uint = 180)
     * A single request owns the override and its restoration, so disconnects cannot leave it active.
     */
    void Analogs::pulse(Request &req, Response &res) {
        if (req.params.Size() < 2 || req.params.Size() > 3) {
            return error(res, "pulse expects [name, value, duration_ms]");
        }

        std::string analog_name;
        if (!read_pulse_name(req.params[0], analog_name)) {
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

        std::lock_guard lock(ANALOGS_MUTEX);
        if (!this->analogs) {
            return error(res, "analogs unavailable");
        }

        Analog *target = nullptr;
        for (auto &analog : *this->analogs) {
            if (analog.getName() == analog_name) {
                target = &analog;
                break;
            }
        }
        if (!target) {
            return error_unknown(res, "analog", analog_name);
        }

        const auto previous_state = target->override_state;
        const auto previous_enabled = target->override_enabled;
        const auto restore = scope_guard{[target, previous_state, previous_enabled] {
            target->override_state = previous_state;
            target->override_enabled = previous_enabled;
            mdxf_poll(true);
        }};
        target->override_state = static_cast<float>(value);
        target->override_enabled = true;
        mdxf_poll(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    }

    bool Analogs::write_analog(std::string name, float state) {

        // check analog cache
        if (!this->analogs) {
            return false;
        }

        // find analog
        for (auto &analog : *this->analogs) {
            if (analog.getName() == name) {
                analog.override_state = CLAMP(state, 0.f, 1.f);
                analog.override_enabled = true;
                mdxf_poll(true);
                return true;
            }
        }

        // unknown analog
        return false;
    }

    bool Analogs::write_analog_reset(std::string name) {

        // check analog cache
        if (!analogs) {
            return false;
        }

        // find analog
        for (auto &analog : *this->analogs) {
            if (analog.getName() == name) {
                analog.override_enabled = false;
                mdxf_poll(true);
                return true;
            }
        }

        // unknown analog
        return false;
    }
}
