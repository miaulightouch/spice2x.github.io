#include "screen_resize.h"

#include "avs/game.h"
#include "external/rapidjson/document.h"
#include "external/rapidjson/pointer.h"
#include "external/rapidjson/prettywriter.h"
#include "misc/eamuse.h"
#include "util/deferlog.h"
#include "util/utils.h"
#include "util/fileutils.h"
#include "hooks/graphics/graphics.h"

namespace cfg {

    // globals
    std::unique_ptr<cfg::ScreenResize> SCREENRESIZE;

    ScreenResize::ScreenResize() {
        bool file_exists = false;
        this->config_path =
            fileutils::get_config_file_path("ScreenResize", "spicetools_screen_resize.json", &file_exists);
        if (file_exists) {
            this->config_load();
        }
    }

    ScreenResize::~ScreenResize() {
    }

    void ScreenResize::config_load() {
        std::lock_guard<std::mutex> config_lock(this->config_mutex);
        std::lock_guard<std::mutex> image_lock(this->image_mutex);
        std::string config = fileutils::text_read(this->config_path);
        if (config.empty()) {
            log_info("ScreenResize", "config is empty");
            return;
        }

        // parse document
        rapidjson::Document doc;
        doc.Parse(config.c_str());

        // check parse error
        auto error = doc.GetParseError();
        if (error) {
            log_warning("ScreenResize", "config parse error: {}", static_cast<uint32_t>(error));
            return;
        }

        // verify root is a dict
        if (!doc.IsObject()) {
            log_warning("ScreenResize", "config not found");
            return;
        }

        bool use_game_setting = false;

        std::string root("/");
        // try to find game-specific setting, if one exists
        {
            const auto game = rapidjson::Pointer("/sp2x_games/" + eamuse_get_game()).Get(doc);
            if (game && game->IsObject()) {
                use_game_setting = true;
                root = "/sp2x_games/" + eamuse_get_game() + "/";
            }
        }

        log_misc(
            "ScreenResize",
            "Loading fullscreen image settings. Game = {}, is_global = {}, JSON path: {}",
            eamuse_get_game(),
            use_game_setting,
            root);

        load_bool_value(doc, root + "enable_screen_resize", this->enable_screen_resize);
        if (this->enable_screen_resize) {
            log_misc("ScreenResize", "enabled by config file");
        }

        load_bool_value(doc, root + "enable_linear_filter", this->enable_linear_filter);
        // Older configs did not store the selected scene and start with Scene 1.
        // Validate before narrowing: the renderer uses this value as an array index.
        int current_scene = 0;
        load_int_value(doc, root + "screen_resize_current_scene", current_scene);
        if (current_scene < 0 || current_scene >= static_cast<int>(std::size(this->scene_settings))) {
            log_warning("ScreenResize", "invalid saved scene {}; using Scene 1", current_scene);
            current_scene = 0;
        }
        this->screen_resize_current_scene = static_cast<int8_t>(current_scene);
        for (size_t i = 0; i < std::size(this->scene_settings); i++) {
            auto& scene = this->scene_settings[i];
            const std::string prefix = fmt::format("scenes/{}/", i);
            load_int_value(doc, root + prefix + "offset_x", scene.offset_x);
            load_int_value(doc, root + prefix + "offset_y", scene.offset_y);
            load_float_value(doc, root + prefix + "scale_x", scene.scale_x);
            load_float_value(doc, root + prefix + "scale_y", scene.scale_y);
            load_bool_value(doc, root + prefix + "keep_aspect_ratio", scene.keep_aspect_ratio);

            int duplicate = 0;
            load_int_value(doc, root + prefix + "duplicate", duplicate);
            scene.duplicate = static_cast<cfg::ScreenDuplicateMode>(duplicate);
        }

        // windowed settings are always under game settings
        root = "/sp2x_games/" + eamuse_get_game() + "/";
        log_misc(
            "ScreenResize",
            "Loading window settings. Game = {}, JSON path: {}",
            eamuse_get_game(),
            root);
        load_bool_value(doc, root + "w_always_on_top", this->window_always_on_top);
        load_bool_value(doc, root + "w_enable_resize", this->enable_window_resize);
        load_bool_value(doc, root + "w_keep_aspect_ratio", this->client_keep_aspect_ratio);
        load_int_value(doc, root + "w_border_type", this->window_decoration);
        load_uint32_value(doc, root + "w_width", this->client_width);
        load_uint32_value(doc, root + "w_height", this->client_height);
        load_int_value(doc, root + "w_offset_x", this->window_offset_x);
        load_int_value(doc, root + "w_offset_y", this->window_offset_y);
        if (this->enable_screen_resize) {
            this->need_surface_clean = true;
        }
    }

    bool ScreenResize::load_bool_value(rapidjson::Document& doc, std::string path, bool& value) {
        const auto v = rapidjson::Pointer(path).Get(doc);
        if (!v) {
            log_misc("ScreenResize", "{} not found", path);
            return false;
        }
        if (!v->IsBool()) {
            log_warning("ScreenResize", "{} is invalid type", path);
            return false;
        }
        value = v->GetBool();
        return true;
    }

    bool ScreenResize::load_int_value(rapidjson::Document& doc, std::string path, int& value) {
        const auto v = rapidjson::Pointer(path).Get(doc);
        if (!v) {
            log_misc("ScreenResize", "{} not found", path);
            return false;
        }
        if (!v->IsInt()) {
            log_warning("ScreenResize", "{} is invalid type", path);
            return false;
        }
        value = v->GetInt();
        return true;
    }

    bool ScreenResize::load_uint32_value(rapidjson::Document& doc, std::string path, uint32_t& value) {
        const auto v = rapidjson::Pointer(path).Get(doc);
        if (!v) {
            log_misc("ScreenResize", "{} not found", path);
            return false;
        }
        if (!v->IsUint()) {
            log_warning("ScreenResize", "{} is invalid type", path);
            return false;
        }
        value = v->GetUint();
        return true;
    }

    bool ScreenResize::load_float_value(rapidjson::Document& doc, std::string path, float& value) {
        const auto v = rapidjson::Pointer(path).Get(doc);
        if (!v) {
            log_misc("ScreenResize", "{} not found", path);
            return false;
        }
        if (v->IsInt()) {
            value = v->GetInt();
            return true;
        }
        if (v->IsDouble()) {
            value = v->GetDouble();
            return true;
        }
        if (v->IsFloat()) {
            value = v->GetFloat();
            return true;
        }
        return false;
    }

    bool ScreenResize::config_save(bool image_only) {
        // Keep config I/O ordered while leaving the drawing lock free during save I/O.
        std::lock_guard<std::mutex> config_lock(this->config_mutex);
        bool enable_screen_resize;
        bool enable_linear_filter;
        int8_t screen_resize_current_scene;
        fullscreen_setting scene_settings[4];
        bool window_always_on_top;
        bool enable_window_resize;
        bool client_keep_aspect_ratio;
        int window_decoration;
        uint32_t client_width;
        uint32_t client_height;
        int32_t window_offset_x;
        int32_t window_offset_y;
        std::string game;
        {
            std::lock_guard<std::mutex> lock(this->image_mutex);
            enable_screen_resize = this->enable_screen_resize;
            enable_linear_filter = this->enable_linear_filter;
            screen_resize_current_scene = this->screen_resize_current_scene;
            for (size_t i = 0; i < std::size(scene_settings); ++i) {
                scene_settings[i] = this->scene_settings[i];
            }
            if (!image_only) {
                window_always_on_top = this->window_always_on_top;
                enable_window_resize = this->enable_window_resize;
                client_keep_aspect_ratio = this->client_keep_aspect_ratio;
                window_decoration = this->window_decoration;
                client_width = this->client_width;
                client_height = this->client_height;
                window_offset_x = this->window_offset_x;
                window_offset_y = this->window_offset_y;
            }
            game = eamuse_get_game();
        }

        rapidjson::Document doc;
        std::string config = fileutils::text_read(this->config_path);
        if (!config.empty()) {
            doc.Parse(config.c_str());
            log_misc("ScreenResize", "existing config file found");
        }
        if (!doc.IsObject()) {
            log_misc("ScreenResize", "clearing out config file");
            doc.SetObject();
        }

        // always save under per-game settings
        std::string root("/sp2x_games/" + game + "/");

        log_misc(
            "ScreenResize",
            "Game = {}, JSON path = {}",
            game,
            root);

        // full screen image settings
        rapidjson::Pointer(root + "enable_screen_resize").Set(doc, enable_screen_resize);
        rapidjson::Pointer(root + "enable_linear_filter").Set(doc, enable_linear_filter);
        rapidjson::Pointer(root + "screen_resize_current_scene").Set(
            doc, static_cast<int>(screen_resize_current_scene));
        for (size_t i = 0; i < std::size(scene_settings); i++) {
            auto& scene = scene_settings[i];
            const std::string prefix = fmt::format("scenes/{}/", i);
            rapidjson::Pointer(root + prefix + "offset_x").Set(doc, scene.offset_x);
            rapidjson::Pointer(root + prefix + "offset_y").Set(doc, scene.offset_y);
            rapidjson::Pointer(root + prefix + "scale_x").Set(doc, scene.scale_x);
            rapidjson::Pointer(root + prefix + "scale_y").Set(doc, scene.scale_y);
            rapidjson::Pointer(root + prefix + "keep_aspect_ratio").Set(doc, scene.keep_aspect_ratio);
            rapidjson::Pointer(root + prefix + "duplicate").Set(doc, scene.duplicate);
        }

        // The API only saves image settings; preserve existing window settings on disk.
        if (!image_only) {
            // windowed mode settings
            rapidjson::Pointer(root + "w_always_on_top").Set(doc, window_always_on_top);
            rapidjson::Pointer(root + "w_enable_resize").Set(doc, enable_window_resize);
            rapidjson::Pointer(root + "w_keep_aspect_ratio").Set(doc, client_keep_aspect_ratio);
            rapidjson::Pointer(root + "w_border_type").Set(doc, window_decoration);
            rapidjson::Pointer(root + "w_width").Set(doc, client_width);
            rapidjson::Pointer(root + "w_height").Set(doc, client_height);
            rapidjson::Pointer(root + "w_offset_x").Set(doc, window_offset_x);
            rapidjson::Pointer(root + "w_offset_y").Set(doc, window_offset_y);
        }

        // build JSON
        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        // save to file
        if (fileutils::write_config_file("ScreenResize", this->config_path, buffer.GetString())) {
            // this->config_dirty = false;
            return true;
        } else {
            log_warning("ScreenResize", "unable to save config file");
            return false;
        }
    }
}
