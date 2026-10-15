#pragma once

#include "api/module.h"
#include "api/request.h"

namespace api::modules {

    class Resize : public Module {
    public:
        Resize();

    private:

        // function definitions
        void image_resize_enable(Request &req, Response &res);
        void image_resize_set_scene(Request &req, Response &res);
        void image_resize_get(Request &req, Response &res);
        void image_resize_set(Request &req, Response &res);
        void image_resize_save(Request &req, Response &res);

        bool check_initialized(Response &res);
        void add_image_resize_state(Response &res);
    };
}
