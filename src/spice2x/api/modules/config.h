#pragma once

#include "api/module.h"
#include "api/request.h"

namespace api::modules {

    class Config : public Module {
    public:
        Config();

    private:
        void get_games(Request &req, Response &res);
        void get_options(Request &req, Response &res);
        void set_options(Request &req, Response &res);
    };
}
