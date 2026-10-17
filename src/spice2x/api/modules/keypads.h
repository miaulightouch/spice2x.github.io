#pragma once

#include "api/module.h"
#include "api/request.h"

namespace api::modules {

    class Keypads : public Module {
    public:
        Keypads();

    private:

        // function definitions
        void capabilities(Request &req, Response &res);
        void insert_card(Request &req, Response &res);
        void write(Request &req, Response &res);
        void set(Request &req, Response &res);
        void get(Request &req, Response &res);
    };
}
