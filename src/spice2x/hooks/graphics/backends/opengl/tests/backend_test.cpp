#include "backend_portable.h"

#define SPICE_OPENGL_TEST 1
#include "../opengl_backend.cpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    cfg::ScreenResize screen_resize;

    void check(bool condition, const char *expression, int line) {
        if (!condition) {
            throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
        }
    }

#define CHECK(expression) check((expression), #expression, __LINE__)

    void reset_backend() {
        opengl_test::reset();
        contexts.clear();
        get_context = opengl_test::get_context;
        get_dc = opengl_test::get_dc;
        create_context = opengl_test::create_context;
        make_current = opengl_test::make_current;
        delete_context = opengl_test::delete_context;
        swap_buffers = opengl_test::swap_buffers;
        swap_layers = opengl_test::swap_layers;
        gl.glGetString = opengl_test::get_string;
        gl.glGetIntegerv = opengl_test::get_integerv;
        gl.glFinish = opengl_test::finish;
        screenshot_busy.store(false);
        in_swap = false;

        screen_resize.enable_screen_resize = false;
        screen_resize.screen_resize_current_scene = 0;
        screen_resize.enable_linear_filter = true;
        for (auto &scene : screen_resize.scene_settings) scene = cfg::fullscreen_setting {};
        cfg::SCREENRESIZE = &screen_resize;
        GRAPHICS_WINDOWED = true;
        GRAPHICS_WINDOW_BACKBUFFER_SCALE = false;
    }

    void prime_window() {
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 1);
        CHECK(contexts.size() == 1);
    }

    void clear_observations() {
        opengl_test::state.events.clear();
        opengl_test::state.deleted_contexts.clear();
        opengl_test::state.drawn_scenes.clear();
        opengl_test::state.drawn_sources.clear();
        opengl_test::state.drawn_buffers.clear();
        opengl_test::state.read_buffers.clear();
        opengl_test::state.finish_calls = 0;
        opengl_test::state.get_integerv_calls = 0;
        opengl_test::state.viewport_queries = 0;
        opengl_test::state.create_context_calls = 0;
        opengl_test::state.make_current_calls = 0;
        opengl_test::state.draw_calls = 0;
        opengl_test::state.read_calls = 0;
        opengl_test::state.capture_skips = 0;
        opengl_test::state.capture_enqueues = 0;
        opengl_test::state.screenshot_triggers = 0;
        opengl_test::state.screenshot_saves = 0;
        opengl_test::state.monitor_calls = 0;
        opengl_test::state.set_window_long_calls = 0;
        opengl_test::state.set_window_pos_calls = 0;
        opengl_test::state.get_client_rect_calls = 0;
        opengl_test::state.last_capture_screen = -1;
        opengl_test::state.last_capture_width = 0;
        opengl_test::state.last_capture_height = 0;
        opengl_test::state.capture_requests.clear();
        opengl_test::state.screenshot_pending = false;
    }

    std::size_t event_index(std::string_view name) {
        const auto it = std::find(opengl_test::state.events.begin(), opengl_test::state.events.end(), name);
        return it == opengl_test::state.events.end() ? opengl_test::state.events.size() :
            static_cast<std::size_t>(it - opengl_test::state.events.begin());
    }

    void test_identity_has_no_work() {
        reset_backend();
        prime_window();
        clear_observations();
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.finish_calls == 0);
        CHECK(opengl_test::state.create_context_calls == 0);
        CHECK(opengl_test::state.make_current_calls == 0);
        CHECK(opengl_test::state.draw_calls == 0);
        CHECK(opengl_test::state.read_calls == 0);
    }

    void test_window_created_before_first_context() {
        reset_backend();
        const auto context = opengl_test::state.current_context;
        opengl_test::state.current_context = nullptr;
        const auto window = opengl_test::state.window;
        graphics_opengl_window_created(window, "L44 - jubeat");
        CHECK(opengl_test::state.graphics_hook_calls == 1);
        CHECK(opengl_test::state.last_hook_window == window);
        CHECK(opengl_test::state.create_context_calls == 0);
        CHECK(opengl_test::state.draw_calls == 0);

        opengl_test::state.current_context = context;
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 2);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 2);
    }

    void test_window_created_ignores_auxiliary_and_other_games() {
        reset_backend();
        const auto window = opengl_test::state.window;
        for (const auto *title : {"GDI+ Window", "JBFSUBWINDOW", "CicMarshalWndEIF", ""}) {
            graphics_opengl_window_created(window, title);
        }
        graphics_opengl_window_created(nullptr, "L44 - jubeat");
        CHECK(opengl_test::state.graphics_hook_calls == 0);

        opengl_test::state.game = "Beatmania IIDX";
        graphics_opengl_window_created(window, "L44 - jubeat");
        CHECK(opengl_test::state.graphics_hook_calls == 0);

        opengl_test::state.game = "Jubeat";
        opengl_test::state.legacy_game_module = false;
        graphics_opengl_window_created(window, "L44 - jubeat");
        CHECK(opengl_test::state.graphics_hook_calls == 0);

        opengl_test::state.legacy_game_module = true;
        GRAPHICS_WINDOWED = false;
        graphics_opengl_window_created(window, "L44 - jubeat");
        CHECK(opengl_test::state.graphics_hook_calls == 0);
    }

    void test_source_destination_fit_draws_once() {
        reset_backend();
        GRAPHICS_WINDOW_BACKBUFFER_SCALE = true;
        opengl_test::state.viewport[2] = 320;
        opengl_test::state.viewport[3] = 240;
        prime_window();
        clear_observations();
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.create_context_calls == 1);
        CHECK(opengl_test::state.make_current_calls == 2);
        CHECK(opengl_test::state.draw_calls == 1);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.drawn_buffers.back() == GL_BACK);
    }

    void test_disabled_scene_uses_base_fit_only() {
        reset_backend();
        GRAPHICS_WINDOWED = false;
        GRAPHICS_WINDOW_BACKBUFFER_SCALE = false;
        opengl_test::state.viewport[2] = 320;
        opengl_test::state.viewport[3] = 240;
        screen_resize.scene_settings[0].scale_x = 2.f;
        screen_resize.scene_settings[0].scale_y = 2.f;
        screen_resize.scene_settings[0].offset_x = 7;
        screen_resize.scene_settings[0].duplicate = cfg::ScreenDuplicateMode::CopyRight;
        screen_resize.enable_screen_resize = false;
        prime_window();
        clear_observations();
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.draw_calls == 1);
        CHECK(opengl_test::state.drawn_scenes.size() == 1);
        const auto &scene = opengl_test::state.drawn_scenes.front();
        CHECK(scene.scale_x == 1.f);
        CHECK(scene.scale_y == 1.f);
        CHECK(scene.offset_x == 0);
        CHECK(scene.offset_y == 0);
        CHECK(scene.duplicate == cfg::ScreenDuplicateMode::None);
    }

    void test_capture_and_resize_finish_and_read_once() {
        reset_backend();
        GRAPHICS_WINDOW_BACKBUFFER_SCALE = true;
        opengl_test::state.viewport[2] = 320;
        opengl_test::state.viewport[3] = 240;
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.draw_calls == 1);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.finish_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
    }

    void test_windowed_without_backbuffer_scale_uses_client_source() {
        reset_backend();
        GRAPHICS_WINDOWED = true;
        GRAPHICS_WINDOW_BACKBUFFER_SCALE = false;
        opengl_test::state.viewport[2] = 320;
        opengl_test::state.viewport[3] = 240;
        screen_resize.enable_screen_resize = true;
        screen_resize.scene_settings[0].scale_x = 1.2f;
        screen_resize.scene_settings[0].scale_y = 1.2f;
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.draw_calls == 1);
        CHECK(opengl_test::state.viewport_queries == 0);
        CHECK(opengl_test::state.drawn_sources.size() == 1);
        const auto &source = opengl_test::state.drawn_sources.front();
        CHECK(source.x == 0);
        CHECK(source.y == 0);
        CHECK(source.width == opengl_test::state.client_rect.right);
        CHECK(source.height == opengl_test::state.client_rect.bottom);
    }

    void test_capture_is_the_only_read_request() {
        reset_backend();
        prime_window();
        clear_observations();
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 0);

        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
        CHECK(opengl_test::state.capture_skips == 0);
    }

    void test_png_and_capture_share_one_read_and_enqueue_first() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        opengl_test::state.screenshot_pending = true;
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
        CHECK(opengl_test::state.screenshot_saves == 1);
        CHECK(event_index("capture_enqueue") < event_index("png"));
    }

    void test_front_and_back_read_buffers() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.double_buffer = true;
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_buffers.back() == GL_BACK);

        reset_backend();
        opengl_test::state.double_buffer = false;
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_buffers.back() == GL_FRONT);
    }

    void test_bad_dc_and_context_are_noops() {
        reset_backend();
        prime_window();
        clear_observations();
        const auto dc = opengl_test::state.current_dc;
        const auto other_dc = reinterpret_cast<HDC>(std::uintptr_t {0x88});
        opengl_test::state.capture_requests.push_back(0);
        present(other_dc);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 0);
        present(dc);
        CHECK(opengl_test::state.read_calls == 1);

        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.current_context = nullptr;
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 0);
    }

    void test_minimized_requeues_screenshot_and_skips_capture() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.iconic = true;
        opengl_test::state.capture_requests.push_back(0);
        opengl_test::state.screenshot_pending = true;
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 1);
        CHECK(opengl_test::state.screenshot_triggers == 1);
        CHECK(opengl_test::state.screenshot_pending);
    }

    void test_context_creation_failure_skips_capture() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.create_context_success = false;
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.create_context_calls == 1);
        CHECK(opengl_test::state.make_current_calls == 0);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 1);
    }

    void test_make_current_failure_restores_game_context() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.make_current_results.push_back(false);
        opengl_test::state.make_current_results.push_back(true);
        const auto game_context = opengl_test::state.current_context;
        const auto game_dc = opengl_test::state.current_dc;
        opengl_test::state.capture_requests.push_back(0);
        present(game_dc);
        CHECK(opengl_test::state.make_current_calls == 2);
        CHECK(opengl_test::state.current_context == game_context);
        CHECK(opengl_test::state.current_dc == game_dc);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 1);
    }

    void test_read_failure_skips_capture() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.read_pixels_success = false;
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 0);
        CHECK(opengl_test::state.capture_skips == 1);
    }

    void test_nonzero_screen_is_skipped() {
        reset_backend();
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(1);
        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 0);
        CHECK(opengl_test::state.capture_skips == 1);
        CHECK(opengl_test::state.last_capture_screen == 1);
    }

    void test_context_delete_retains_state_on_failure() {
        reset_backend();
        CHECK(make_current_hook(opengl_test::state.current_dc, opengl_test::state.current_context) == TRUE);
        prime_window();
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        present(opengl_test::state.current_dc);
        CHECK(contexts.size() == 1);
        const auto game_context = opengl_test::state.current_context;
        const auto private_context = opengl_test::state.private_context;

        clear_observations();
        opengl_test::state.delete_context_success = false;
        CHECK(delete_context_hook(game_context) == FALSE);
        CHECK(contexts.size() == 1);
        CHECK(contexts.at(game_context).owner_window == opengl_test::state.window);
        CHECK(opengl_test::state.deleted_contexts.size() == 1);
        CHECK(opengl_test::state.deleted_contexts.front() == game_context);

        opengl_test::state.delete_context_success = true;
        CHECK(delete_context_hook(game_context) == TRUE);
        CHECK(contexts.empty());
        CHECK(opengl_test::state.deleted_contexts.size() == 3);
        CHECK(opengl_test::state.deleted_contexts[1] == game_context);
        CHECK(opengl_test::state.deleted_contexts[2] == private_context);
    }

    void test_same_context_equivalent_dc_is_processed() {
        reset_backend();
        prime_window();
        clear_observations();
        const auto equivalent_dc = reinterpret_cast<HDC>(std::uintptr_t {0x99});
        // The WGL context stays current on the original DC while the game
        // presents through an equivalent DC handle.
        opengl_test::state.capture_requests.push_back(0);
        present(equivalent_dc);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
    }

    void test_released_dc_uses_recorded_window_and_live_paint_dc() {
        const auto other_window = reinterpret_cast<HWND>(std::uintptr_t {0x77});
        for (const auto stale_window : {static_cast<HWND>(nullptr), other_window}) {
            reset_backend();
            const auto context = opengl_test::state.current_context;
            const auto original_dc = opengl_test::state.current_dc;
            const auto paint_dc = reinterpret_cast<HDC>(std::uintptr_t {0x99});
            const auto window = opengl_test::state.window;
            CHECK(make_current_hook(original_dc, context) == TRUE);
            CHECK(contexts.at(context).owner_window == window);
            opengl_test::state.dc_windows[original_dc] = stale_window;
            opengl_test::state.dc_windows[paint_dc] = window;
            opengl_test::state.client_rect = {0, 0, 1080, 1920};
            opengl_test::state.viewport[2] = 768;
            opengl_test::state.viewport[3] = 1360;
            GRAPHICS_WINDOW_BACKBUFFER_SCALE = true;

            present(paint_dc);
            clear_observations();
            opengl_test::state.capture_requests.push_back(0);
            present(paint_dc);
            CHECK(opengl_test::state.draw_calls == 1);
            CHECK(opengl_test::state.capture_enqueues == 1);
            CHECK(opengl_test::state.last_capture_width == 1080);
            CHECK(opengl_test::state.last_capture_height == 1920);
            CHECK(opengl_test::state.drawn_sources.back().width == 768);
            CHECK(opengl_test::state.drawn_sources.back().height == 1360);
            CHECK(opengl_test::state.current_context == context);
            CHECK(opengl_test::state.current_dc == paint_dc);
            CHECK(contexts.at(context).owner_window == window);

            opengl_test::state.dc_windows[paint_dc] = nullptr;
            const auto next_paint_dc = reinterpret_cast<HDC>(std::uintptr_t {0x100});
            opengl_test::state.dc_windows[next_paint_dc] = window;
            opengl_test::state.capture_requests.push_back(0);
            present(next_paint_dc);
            CHECK(opengl_test::state.draw_calls == 2);
            CHECK(opengl_test::state.capture_enqueues == 2);
            CHECK(opengl_test::state.current_dc == next_paint_dc);
        }
    }

    void test_tracked_context_follows_successful_window_binding() {
        reset_backend();
        const auto context = opengl_test::state.current_context;
        const auto original_dc = opengl_test::state.current_dc;
        const auto window = opengl_test::state.window;
        const auto other_dc = reinterpret_cast<HDC>(std::uintptr_t {0x88});
        const auto other_window = reinterpret_cast<HWND>(std::uintptr_t {0x77});
        opengl_test::state.dc_windows[original_dc] = window;
        opengl_test::state.dc_windows[other_dc] = other_window;
        CHECK(make_current_hook(original_dc, context) == TRUE);
        CHECK(make_current_hook(other_dc, context) == TRUE);
        CHECK(contexts.at(context).owner_window == other_window);
        clear_observations();
        opengl_test::state.capture_requests.push_back(0);
        present(original_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 0);
        CHECK(opengl_test::state.capture_enqueues == 0);
        CHECK(opengl_test::state.capture_requests.size() == 1);
        CHECK(opengl_test::state.current_dc == other_dc);
    }

    void test_failed_bind_and_unbind_do_not_change_owner() {
        reset_backend();
        const auto context = opengl_test::state.current_context;
        const auto dc = opengl_test::state.current_dc;
        const auto window = opengl_test::state.window;
        CHECK(make_current_hook(dc, context) == TRUE);
        opengl_test::state.make_current_results.push_back(false);
        CHECK(make_current_hook(nullptr, context) == FALSE);
        CHECK(contexts.at(context).owner_window == window);
        CHECK(make_current_hook(nullptr, nullptr) == TRUE);
        CHECK(contexts.size() == 1);
        CHECK(contexts.at(context).owner_window == window);
    }

    void test_same_handle_reused_for_another_window_is_rejected() {
        reset_backend();
        const auto context = opengl_test::state.current_context;
        const auto dc = opengl_test::state.current_dc;
        CHECK(make_current_hook(dc, context) == TRUE);
        opengl_test::state.dc_windows[dc] = reinterpret_cast<HWND>(std::uintptr_t {0x77});
        opengl_test::state.capture_requests.push_back(0);
        present(dc);
        CHECK(opengl_test::state.graphics_hook_calls == 0);
        CHECK(opengl_test::state.capture_enqueues == 0);
        CHECK(opengl_test::state.capture_requests.size() == 1);
    }

    void test_untracked_released_dc_is_not_assumed_to_match() {
        reset_backend();
        const auto paint_dc = reinterpret_cast<HDC>(std::uintptr_t {0x99});
        opengl_test::state.dc_windows[opengl_test::state.current_dc] = nullptr;
        opengl_test::state.dc_windows[paint_dc] = opengl_test::state.window;
        opengl_test::state.capture_requests.push_back(0);
        present(paint_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 0);
        CHECK(opengl_test::state.capture_enqueues == 0);
        CHECK(opengl_test::state.capture_requests.size() == 1);
    }

    BOOL WINAPI recursive_swap_buffers(HDC dc) {
        ++opengl_test::state.swap_buffer_calls;
        opengl_test::event("swap_buffers");
        if (opengl_test::state.reenter_swap && !opengl_test::state.reentered_swap) {
            opengl_test::state.reentered_swap = true;
            swap_buffers_hook(dc);
        }
        return TRUE;
    }

    BOOL WINAPI recursive_swap_layers(HDC dc, UINT planes) {
        ++opengl_test::state.swap_layer_calls;
        opengl_test::event("swap_layers");
        if (opengl_test::state.reenter_layers && !opengl_test::state.reentered_layers) {
            opengl_test::state.reentered_layers = true;
            swap_layers_hook(dc, planes);
        }
        return TRUE;
    }

    void test_swap_layer_main_plane_and_reentrancy() {
        reset_backend();
        prime_window();
        clear_observations();
        swap_layers = recursive_swap_layers;
        swap_layers_hook(opengl_test::state.current_dc, 0);
        CHECK(opengl_test::state.read_calls == 0);

        opengl_test::state.reentered_layers = false;
        opengl_test::state.reenter_layers = true;
        opengl_test::state.capture_requests.push_back(0);
        swap_layers_hook(opengl_test::state.current_dc, WGL_SWAP_MAIN_PLANE);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
        CHECK(opengl_test::state.reentered_layers);
    }

    void test_swap_buffers_reentrancy() {
        reset_backend();
        prime_window();
        clear_observations();
        swap_buffers = recursive_swap_buffers;
        opengl_test::state.reenter_swap = true;
        opengl_test::state.capture_requests.push_back(0);
        swap_buffers_hook(opengl_test::state.current_dc);
        CHECK(opengl_test::state.read_calls == 1);
        CHECK(opengl_test::state.capture_enqueues == 1);
        CHECK(opengl_test::state.reentered_swap);
    }

    void test_window_init_shared_hook_and_fullscreen_bounds() {
        reset_backend();
        GRAPHICS_WINDOWED = false;
        opengl_test::state.monitor_rect = {100, 200, 1380, 920};
        prime_window();
        CHECK(opengl_test::state.graphics_hook_calls == 1);
        CHECK(opengl_test::state.monitor_calls == 1);
        CHECK(opengl_test::state.set_window_long_calls == 2);
        CHECK(opengl_test::state.set_window_pos_calls == 1);
        CHECK(opengl_test::state.last_set_window_x == 100);
        CHECK(opengl_test::state.last_set_window_y == 200);
        CHECK(opengl_test::state.last_set_window_width == 1280);
        CHECK(opengl_test::state.last_set_window_height == 720);
        CHECK(opengl_test::state.last_set_window_flags ==
            (SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS));

        present(opengl_test::state.current_dc);
        CHECK(opengl_test::state.graphics_hook_calls == 1);
        CHECK(opengl_test::state.monitor_calls == 1);
    }

    using Test = std::pair<const char *, void (*)()>;

    const Test tests[] = {
        {"identity_has_no_work", test_identity_has_no_work},
        {"window_created_before_first_context", test_window_created_before_first_context},
        {"window_created_ignores_auxiliary_and_other_games", test_window_created_ignores_auxiliary_and_other_games},
        {"source_destination_fit_draws_once", test_source_destination_fit_draws_once},
        {"disabled_scene_uses_base_fit_only", test_disabled_scene_uses_base_fit_only},
        {"capture_and_resize_finish_and_read_once", test_capture_and_resize_finish_and_read_once},
        {"windowed_without_backbuffer_scale_uses_client_source", test_windowed_without_backbuffer_scale_uses_client_source},
        {"capture_is_the_only_read_request", test_capture_is_the_only_read_request},
        {"png_and_capture_share_one_read_and_enqueue_first", test_png_and_capture_share_one_read_and_enqueue_first},
        {"front_and_back_read_buffers", test_front_and_back_read_buffers},
        {"bad_dc_and_context_are_noops", test_bad_dc_and_context_are_noops},
        {"minimized_requeues_screenshot_and_skips_capture", test_minimized_requeues_screenshot_and_skips_capture},
        {"context_creation_failure_skips_capture", test_context_creation_failure_skips_capture},
        {"make_current_failure_restores_game_context", test_make_current_failure_restores_game_context},
        {"read_failure_skips_capture", test_read_failure_skips_capture},
        {"nonzero_screen_is_skipped", test_nonzero_screen_is_skipped},
        {"context_delete_retains_state_on_failure", test_context_delete_retains_state_on_failure},
        {"same_context_equivalent_dc_is_processed", test_same_context_equivalent_dc_is_processed},
        {"released_dc_uses_recorded_window_and_live_paint_dc", test_released_dc_uses_recorded_window_and_live_paint_dc},
        {"tracked_context_follows_successful_window_binding", test_tracked_context_follows_successful_window_binding},
        {"failed_bind_and_unbind_do_not_change_owner", test_failed_bind_and_unbind_do_not_change_owner},
        {"same_handle_reused_for_another_window_is_rejected", test_same_handle_reused_for_another_window_is_rejected},
        {"untracked_released_dc_is_not_assumed_to_match", test_untracked_released_dc_is_not_assumed_to_match},
        {"swap_layer_main_plane_and_reentrancy", test_swap_layer_main_plane_and_reentrancy},
        {"swap_buffers_reentrancy", test_swap_buffers_reentrancy},
        {"window_init_shared_hook_and_fullscreen_bounds", test_window_init_shared_hook_and_fullscreen_bounds},
    };
}

int main() {
    int failures = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
