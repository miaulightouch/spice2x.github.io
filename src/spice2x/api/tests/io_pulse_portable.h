#pragma once

#include <algorithm>
#include <cassert>
#include <climits>
#include <functional>
#include <string>
#include <vector>

#include "api/module.h"
#include "api/request.h"
#include "api/response.h"
#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"
#include "cfg/analog.h"
#include "cfg/light.h"
#include "util/scope_guard.h"

extern int io_pulse_flush_count;
extern std::vector<float> io_pulse_light_outputs;

namespace rawinput {
    class RawInputManager {
    public:
        void devices_flush_output() {
            ++io_pulse_flush_count;
        }
    };
}

extern rawinput::RawInputManager *RI_MGR;

#ifndef CLAMP
#define CLAMP(value, lower, upper) (std::min((upper), std::max((lower), (value))))
#endif

namespace GameAPI::Analogs {
    inline float getState(rawinput::RawInputManager *, Analog &analog) {
        return analog.override_enabled ? analog.override_state : analog.getLastState();
    }
}

namespace GameAPI::Lights {
    inline float readLight(rawinput::RawInputManager *, Light &light) {
        return light.override_enabled ? light.override_state : light.last_state;
    }

    inline void writeLight(rawinput::RawInputManager *, Light &light, float value) {
        light.last_state = std::clamp(value, 0.f, 1.f);
        io_pulse_light_outputs.push_back(light.last_state);
    }
}

namespace cfg {
    inline bool CONFIGURATOR_STANDALONE = false;
}

namespace games {
    std::vector<Analog> *get_analogs(const std::string &game);
    std::vector<Light> *get_lights(const std::string &game);
}

const std::string &eamuse_get_game();
void mdxf_poll(bool force);

namespace api {
    inline bool LOGGING = false;

    inline Request::Request(rapidjson::Document &document) {
        this->parse_error = false;
        auto id = document.FindMember("id");
        auto module = document.FindMember("module");
        auto function = document.FindMember("function");
        auto params = document.FindMember("params");
        if (id == document.MemberEnd() || !id->value.IsUint64() ||
                module == document.MemberEnd() || !module->value.IsString() ||
                function == document.MemberEnd() || !function->value.IsString() ||
                params == document.MemberEnd() || !params->value.IsArray()) {
            this->parse_error = true;
            return;
        }
        this->id = id->value.GetUint64();
        this->module = module->value.GetString();
        this->function = function->value.GetString();
        this->params = params->value;
    }

    inline Response::Response(uint64_t id) {
        this->document.Parse("{\"id\":0,\"errors\":[],\"data\":[]}");
        this->document["id"].SetUint64(id);
        this->errors = this->document["errors"];
        this->data = this->document["data"];
    }

    inline std::string Response::get_string(bool) {
        this->document["errors"] = this->errors;
        this->document["data"] = this->data;
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        this->document.Accept(writer);
        return buffer.GetString();
    }

    inline Module::Module(std::string name, bool password_force)
        : name(std::move(name)), password_force(password_force) {
    }

    inline void Module::handle(Request &req, Response &res) {
        const auto function = this->functions.find(req.function);
        if (function == this->functions.end()) {
            return error_function_unknown(res);
        }
        function->second(req, res);
    }

    inline void Module::error(Response &res, std::string message) {
        rapidjson::Value value(message.c_str(), res.doc()->GetAllocator());
        res.add_error(value);
    }
}
