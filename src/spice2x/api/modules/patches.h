#pragma once

#include "api/module.h"
#include "api/request.h"

namespace api::modules {

    class Patches : public Module {
    public:
        Patches();

    private:
        void get(Request &req, Response &res);
        void set(Request &req, Response &res);
        void set_group(Request &req, Response &res);
        void disable_all(Request &req, Response &res);
        void set_auto_apply(Request &req, Response &res);
        void save(Request &req, Response &res);
        void reload(Request &req, Response &res);
        void import_url(Request &req, Response &res);
        void remove_recent_url(Request &req, Response &res);
        void clear_recent_urls(Request &req, Response &res);
    };
}
