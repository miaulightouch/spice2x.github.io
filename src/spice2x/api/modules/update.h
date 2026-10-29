#pragma once

#include "api/module.h"

namespace api::modules {

    class Update : public Module {
    public:
        Update();
        ~Update() override;

    private:
        std::string upload_id;
        void status(Request &req, Response &res);
        void begin(Request &req, Response &res);
        void chunk(Request &req, Response &res);
        void finish(Request &req, Response &res);
        void apply(Request &req, Response &res);
        void cancel(Request &req, Response &res);
        bool owns_upload(Request &req, Response &res, size_t count);
    };
}
