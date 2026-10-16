#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "api/module.h"
#include "api/request.h"
#include "api/response.h"
#include "external/rapidjson/document.h"
#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"

namespace rawinput {
    class RawInputManager {};
}

extern rawinput::RawInputManager *RI_MGR;

class Button;

namespace GameAPI::Buttons {
    enum State {
        BUTTON_PRESSED = true,
        BUTTON_NOT_PRESSED = false,
    };

    float getVelocity(rawinput::RawInputManager *, Button &button);
}

class Button {
private:
    std::string name;

public:
    bool override_enabled = false;
    GameAPI::Buttons::State override_state = GameAPI::Buttons::BUTTON_NOT_PRESSED;
    float override_velocity = 0.f;

    explicit Button(std::string name) : name(std::move(name)) {}

    const std::string &getName() const { return this->name; }
};

namespace games {
    std::vector<Button> *get_buttons(const std::string &game);
}

const std::string &eamuse_get_game();
void mdxf_poll(bool force);

#ifndef CLAMP
#define CLAMP(value, lower, upper) (std::min((upper), std::max((lower), (value))))
#endif

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

    inline std::string Response::get_string(bool pretty) {
        this->document["errors"] = this->errors;
        this->document["data"] = this->data;
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        this->document.Accept(writer);
        return buffer.GetString();
    }

    inline Module::Module(std::string name, bool password_force) : name(std::move(name)), password_force(password_force) {}

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

namespace api::modules {
    class Buttons : public Module {
    public:
        Buttons();

    private:
        std::vector<Button> *buttons;
        void read(Request &req, Response &res);
        void write(Request &req, Response &res);
        void write_reset(Request &req, Response &res);
        void pulse(Request &req, Response &res);
        bool write_button(std::string name, float state);
        bool write_button_reset(std::string name);
    };
}
