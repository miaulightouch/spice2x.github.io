#include "resize.h"
#include <cmath>
#include <mutex>
#include "external/rapidjson/document.h"
#include "cfg/screen_resize.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    Resize::Resize() : Module("resize") {
        functions["image_resize_enable"] = std::bind(&Resize::image_resize_enable, this, _1, _2);
        functions["image_resize_set_scene"] = std::bind(&Resize::image_resize_set_scene, this, _1, _2);
        functions["image_resize_get"] = std::bind(&Resize::image_resize_get, this, _1, _2);
        functions["image_resize_set"] = std::bind(&Resize::image_resize_set, this, _1, _2);
        functions["image_resize_save"] = std::bind(&Resize::image_resize_save, this, _1, _2);
        require_password("image_resize_save");
    }

    bool Resize::check_initialized(Response &res) {
        if (!cfg::SCREENRESIZE) {
            error(res, "Image resize is not initialized.");
            return false;
        }
        return true;
    }

    /**
     * image_resize_enable(enable: bool)
     */
    void Resize::image_resize_enable(Request &req, Response &res) {
        if (!check_initialized(res)) {
            return;
        }
        if (req.params.Size() < 1) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsBool()) {
            return error_type(res, "enable", "bool");
        }

        std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
        const bool enabled = req.params[0].GetBool();
        if (!cfg::SCREENRESIZE->enable_screen_resize && enabled) {
            cfg::SCREENRESIZE->need_surface_clean = true;
        }
        cfg::SCREENRESIZE->enable_screen_resize = enabled;
    }

    /**
     * image_resize_set_scene(scene: int)
     */
    void Resize::image_resize_set_scene(Request &req, Response &res) {
        if (!check_initialized(res)) {
            return;
        }
        if (req.params.Size() < 1) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsInt()) {
            return error_type(res, "scene", "int");
        }

        std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
        const auto scene = req.params[0].GetInt();
        if (scene < 0 || (int)std::size(cfg::SCREENRESIZE->scene_settings) < scene) {
            return error(res, "invalid scene number");
        }
        if (scene == 0) {
            cfg::SCREENRESIZE->enable_screen_resize = false;
        } else {
            const bool scene_changed = cfg::SCREENRESIZE->screen_resize_current_scene != scene - 1;
            const bool was_disabled = !cfg::SCREENRESIZE->enable_screen_resize;
            if (scene_changed || was_disabled) {
                cfg::SCREENRESIZE->need_surface_clean = true;
            }
            cfg::SCREENRESIZE->enable_screen_resize = true;
            cfg::SCREENRESIZE->screen_resize_current_scene = scene - 1;
        }
    }

    // Caller holds image_mutex so the response describes a single consistent state.
    void Resize::add_image_resize_state(Response &res) {
        auto &resize = *cfg::SCREENRESIZE;
        auto &alloc = res.doc()->GetAllocator();
        Value state(kObjectType);
        state.AddMember("enabled", resize.enable_screen_resize, alloc);
        state.AddMember("scene", resize.screen_resize_current_scene + 1, alloc);
        state.AddMember("linear_filter", resize.enable_linear_filter, alloc);
        Value scenes(kArrayType);
        for (const auto &scene : resize.scene_settings) {
            Value settings(kObjectType);
            settings.AddMember("offset_x", scene.offset_x, alloc);
            settings.AddMember("offset_y", scene.offset_y, alloc);
            settings.AddMember("scale_x", scene.scale_x, alloc);
            settings.AddMember("scale_y", scene.scale_y, alloc);
            settings.AddMember("keep_aspect_ratio", scene.keep_aspect_ratio, alloc);
            settings.AddMember("duplicate", static_cast<int32_t>(scene.duplicate), alloc);
            scenes.PushBack(settings, alloc);
        }
        state.AddMember("scenes", scenes, alloc);
        res.add_data(state);
    }

    void Resize::image_resize_get(Request &req, Response &res) {
        if (!check_initialized(res)) {
            return;
        }
        std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
        add_image_resize_state(res);
    }

    /**
     * image_resize_set(settings: object)
     * Validate the entire partial update before changing any live settings.
     */
    void Resize::image_resize_set(Request &req, Response &res) {
        if (!check_initialized(res)) {
            return;
        }
        if (req.params.Size() < 1) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsObject()) {
            return error_type(res, "settings", "object");
        }

        std::lock_guard<std::mutex> lock(cfg::SCREENRESIZE->image_mutex);
        auto &resize = *cfg::SCREENRESIZE;
        const auto &settings = req.params[0];
        int32_t scene_index = resize.screen_resize_current_scene;
        const auto old_scene_index = scene_index;
        const bool old_enabled = resize.enable_screen_resize;
        const auto old_duplicate = resize.scene_settings[scene_index].duplicate;
        if (settings.HasMember("scene")) {
            if (!settings["scene"].IsInt()) {
                return error_type(res, "scene", "int");
            }
            scene_index = settings["scene"].GetInt();
            if (scene_index < 1 || static_cast<size_t>(scene_index) >
                    std::size(resize.scene_settings)) {
                return error(res, "scene must be between 1 and 4");
            }
            --scene_index;
        }

        auto scene = resize.scene_settings[scene_index];
        bool enabled = resize.enable_screen_resize;
        bool linear_filter = resize.enable_linear_filter;
        for (auto it = settings.MemberBegin(); it != settings.MemberEnd(); ++it) {
            const std::string name(it->name.GetString(), it->name.GetStringLength());
            const auto &value = it->value;
            if (name == "scene") {
                continue;
            } else if (name == "enabled" || name == "linear_filter" ||
                    name == "keep_aspect_ratio") {
                if (!value.IsBool()) {
                    return error_type(res, name, "bool");
                }
                if (name == "enabled") {
                    enabled = value.GetBool();
                } else if (name == "linear_filter") {
                    linear_filter = value.GetBool();
                } else {
                    scene.keep_aspect_ratio = value.GetBool();
                }
            } else if (name == "offset_x" || name == "offset_y") {
                if (!value.IsInt()) {
                    return error_type(res, name, "int");
                }
                const auto offset = value.GetInt();
                if (offset < -32768 || offset > 32767) {
                    return error(res, name + " must be between -32768 and 32767");
                }
                (name == "offset_x" ? scene.offset_x : scene.offset_y) = offset;
            } else if (name == "scale_x" || name == "scale_y") {
                if (!value.IsNumber()) {
                    return error_type(res, name, "number");
                }
                const double scale = value.GetDouble();
                if (!std::isfinite(scale) || scale < 0.6 || scale > 2.5) {
                    return error(res, name + " must be between 0.6 and 2.5");
                }
                (name == "scale_x" ? scene.scale_x : scene.scale_y) =
                    static_cast<float>(scale);
            } else if (name == "duplicate") {
                if (!value.IsInt()) {
                    return error_type(res, name, "int");
                }
                const auto duplicate = value.GetInt();
                if (duplicate < 0 || duplicate > 2) {
                    return error(res, "duplicate must be between 0 and 2");
                }
                scene.duplicate = static_cast<cfg::ScreenDuplicateMode>(duplicate);
            } else {
                return error_unknown(res, "setting", name);
            }
        }

        if (scene.keep_aspect_ratio) {
            const bool has_x = settings.HasMember("scale_x");
            const bool has_y = settings.HasMember("scale_y");
            if (has_x && has_y && scene.scale_x != scene.scale_y) {
                return error(res, "scale_x and scale_y must match when keeping aspect ratio");
            }
            if (has_y && !has_x) {
                scene.scale_x = scene.scale_y;
            } else {
                scene.scale_y = scene.scale_x;
            }
        }

        const bool needs_surface_clean = scene_index != old_scene_index ||
            scene.duplicate != old_duplicate || (!old_enabled && enabled);
        resize.scene_settings[scene_index] = scene;
        resize.screen_resize_current_scene = static_cast<int8_t>(scene_index);
        resize.enable_screen_resize = enabled;
        resize.enable_linear_filter = linear_filter;
        if (needs_surface_clean) {
            resize.need_surface_clean = true;
        }
        add_image_resize_state(res);
    }

    void Resize::image_resize_save(Request &req, Response &res) {
        if (!check_initialized(res)) {
            return;
        }
        // Window settings belong to the window thread and are not changed by this API.
        if (!cfg::SCREENRESIZE->config_save(true)) {
            error(res, "Unable to save image resize settings.");
        }
    }
}
