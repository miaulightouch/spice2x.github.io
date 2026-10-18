#include "opengl_backend.h"

#ifdef SPICE_OPENGL_TEST
#include "tests/backend_portable.h"
#else
#include "opengl_resize.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "external/fpng/fpng.h"
#include "hooks/graphics/graphics.h"
#include "misc/clipboard.h"
#include "misc/eamuse.h"
#include "overlay/notifications.h"
#include "util/detour.h"
#include "util/fileutils.h"
#include "util/logging.h"
#include "util/threadpool.h"
#endif

#ifdef SPICE_OPENGL_PROBE
#include "opengl_probe.h"
#include "opengl_probe_metrics.h"
#include <array>
#include <chrono>
#include "external/rapidjson/stringbuffer.h"
#include "external/rapidjson/writer.h"
#endif

namespace {
    graphics::opengl::GlApi gl;
    decltype(wglGetCurrentContext) *get_context = nullptr;
    decltype(wglGetCurrentDC) *get_dc = nullptr;
    decltype(wglCreateContext) *create_context = nullptr;
    decltype(wglMakeCurrent) *make_current = nullptr;
    decltype(wglDeleteContext) *delete_context = nullptr;
    decltype(SwapBuffers) *swap_buffers = nullptr;
    decltype(wglSwapLayerBuffers) *swap_layers = nullptr;
    std::mutex contexts_mutex;
    thread_local bool in_swap = false;
    std::atomic<bool> screenshot_busy {false};

#ifdef SPICE_OPENGL_PROBE
    decltype(wglGetProcAddress) *get_proc_address = nullptr;
    using ProbeClock = std::chrono::steady_clock;
    constexpr const char *probe_modes[] {"legacy", "shader", "shader-pbo"};
    constexpr const char *probe_metric_names[] {
        "hook_cpu_ms", "swap_cpu_ms", "frame_interval_ms", "finish_cpu_ms", "context_cpu_ms",
        "state_cpu_ms", "copy_cpu_ms", "draw_cpu_ms", "read_issue_cpu_ms", "read_map_cpu_ms",
        "pbo_latency_frames"};
    struct ProbeStats {
        std::array<graphics::opengl::ProbeMetric, std::size(probe_metric_names)> metrics;
        uint64_t frames = 0, capture_issued = 0, capture_completed = 0, capture_dropped = 0;
        int active_mode = 0;
        bool available = false;
        graphics::opengl::ImageRect source;
        int width = 0, height = 0;
        cfg::fullscreen_setting scene;
        bool linear = false, resize = false;
        std::string description = "shader capability probe runs when selected";
        std::string error;
    };
    std::mutex probe_mutex;
    ProbeStats probe_stats;
    std::atomic<int> probe_requested {0};
    std::atomic<uint64_t> probe_epoch {0};
    struct ProbeFrameStats {
        bool tracked = false;
        uint64_t epoch = 0;
        double finish_ms = 0, context_ms = 0;
        unsigned capture_issued = 0, capture_completed = 0, capture_dropped = 0;
        graphics::opengl::ImageRect source;
        int width = 0, height = 0;
        cfg::fullscreen_setting scene;
        bool linear = false, resize = false;
        graphics::opengl::ProbeTiming timings;
    };
    thread_local ProbeFrameStats probe_frame;

    struct ProbeCaptureGuard {
        int screen;
        uint64_t request;
        bool pending;

        ProbeCaptureGuard(int screen, uint64_t request, bool pending)
            : screen(screen), request(request), pending(pending) {
            probe_frame.capture_issued += pending;
        }

        void drop() noexcept {
            if (!pending) return;
            pending = false;
            try { graphics_capture_skip_request(screen, request); } catch (...) {}
            ++probe_frame.capture_dropped;
        }

        void deliver(std::shared_ptr<uint8_t[]> pixels, int width, int height) {
            const bool accepted = graphics_capture_enqueue_request(screen, request, std::move(pixels), width, height);
            pending = false;
            if (accepted) ++probe_frame.capture_completed;
            else ++probe_frame.capture_dropped;
        }

        ~ProbeCaptureGuard() { drop(); }
    };

    double probe_elapsed(ProbeClock::time_point start) {
        return std::chrono::duration<double, std::milli>(ProbeClock::now() - start).count();
    }

    void probe_active(int mode, bool available, const std::string &description, const std::string &error) {
        std::lock_guard lock(probe_mutex);
        if (probe_stats.active_mode != mode || probe_stats.available != available || probe_stats.error != error) {
            log_info("graphics::opengl", "probe active={}, available={}, detail={}, error={}",
                    probe_modes[mode], available, description, error);
        }
        probe_stats.active_mode = mode;
        probe_stats.available = available;
        probe_stats.description = description;
        probe_stats.error = error;
    }

    void probe_record_swap(double hook_ms, double swap_ms, ProbeClock::time_point start) noexcept try {
        if (!probe_frame.tracked) return;
        static thread_local auto previous = ProbeClock::time_point {};
        static thread_local uint64_t previous_epoch = ~uint64_t {0};
        static auto last_log = ProbeClock::now();
        bool write_log = false;
        uint64_t frames = 0, issued = 0, completed = 0, dropped = 0;
        int mode = 0;
        std::array<double, std::size(probe_metric_names)> means {};
        double hook_max = 0, interval_max = 0;
        {
            std::lock_guard lock(probe_mutex);
            if (probe_frame.epoch != probe_epoch.load()) return;
            auto &m = probe_stats.metrics;
            m[0].add(hook_ms); m[1].add(swap_ms);
            if (previous_epoch == probe_frame.epoch && previous != ProbeClock::time_point {}) {
                m[2].add(std::chrono::duration<double, std::milli>(start - previous).count());
            }
            previous = start; previous_epoch = probe_frame.epoch;
            m[3].add(probe_frame.finish_ms); m[4].add(probe_frame.context_ms);
            m[5].add(probe_frame.timings.state_ms); m[6].add(probe_frame.timings.copy_ms);
            m[7].add(probe_frame.timings.draw_ms);
            if (probe_frame.timings.read_issue_ms > 0) m[8].add(probe_frame.timings.read_issue_ms);
            if (probe_frame.timings.read_map_ms > 0) m[9].add(probe_frame.timings.read_map_ms);
            if (probe_frame.timings.pbo_latency_frames) m[10].add(probe_frame.timings.pbo_latency_frames);
            ++probe_stats.frames;
            probe_stats.capture_issued += probe_frame.capture_issued;
            probe_stats.capture_completed += probe_frame.capture_completed;
            probe_stats.capture_dropped += probe_frame.capture_dropped;
            probe_stats.source = probe_frame.source;
            probe_stats.width = probe_frame.width; probe_stats.height = probe_frame.height;
            probe_stats.scene = probe_frame.scene;
            probe_stats.linear = probe_frame.linear; probe_stats.resize = probe_frame.resize;
            if (start - last_log >= std::chrono::seconds(5)) {
                last_log = start; write_log = true;
                frames = probe_stats.frames;
                issued = probe_stats.capture_issued;
                completed = probe_stats.capture_completed;
                dropped = probe_stats.capture_dropped;
                mode = probe_stats.active_mode;
                for (size_t index = 0; index < m.size(); ++index) {
                    means[index] = m[index].count ? m[index].total / m[index].count : 0;
                }
                hook_max = m[0].maximum;
                interval_max = m[2].maximum;
            }
        }
        // Percentiles are computed on the requesting WS thread, outside the render loop.
        if (write_log) log_info("graphics::opengl",
                "probe mode={} frames={} cpu mean ms: hook={:.3f} swap={:.3f} interval={:.3f} finish={:.3f} context={:.3f} state={:.3f} copy={:.3f} draw={:.3f} read={:.3f} map={:.3f}; max hook={:.3f} interval={:.3f}; capture issued={} completed={} dropped={} pbo frames={:.2f}",
                probe_modes[mode], frames, means[0], means[1], means[2], means[3], means[4],
                means[5], means[6], means[7], means[8], means[9], hook_max, interval_max,
                issued, completed, dropped, means[10]);
    } catch (...) {}
#endif

    struct Context {
        HGLRC resize_context = nullptr;
        HWND owner_window = nullptr;
        HWND window = nullptr;
        graphics::opengl::ImageResize image;
        GLenum buffer = 0;
        bool warned = false;
        bool logged = false;
#ifdef SPICE_OPENGL_PROBE
        graphics::opengl::ProbeRenderer probe;
        bool probe_attempted = false;
        bool probe_available = false;
        bool probe_capture_pending = false;
        uint64_t probe_capture_request = 0;
        uint64_t probe_capture_epoch = 0;
        int probe_mode = 0;
        int probe_request_seen = -1;
        std::string probe_error_seen;
#endif
    };
    // Game contexts may move between equivalent DC handles; all resources are context-owned.
    std::map<HGLRC, Context> contexts;

    void save_screenshot(std::shared_ptr<uint8_t[]> pixels, int width, int height) {
        // One pending PNG bounds memory and serializes filename allocation and clipboard writes.
        if (screenshot_busy.exchange(true)) {
            graphics_screenshot_trigger();
            return;
        }
        try {
            static auto *pool = new ThreadPool(1);
            pool->add([pixels = std::move(pixels), width, height] {
                try {
                    static std::once_flag fpng_ready;
                    std::call_once(fpng_ready, [] { fpng::fpng_init(); });
                    const auto path = graphics_screenshot_genpath();
                    if (!path.empty() && fpng::fpng_encode_image_to_file(
                            path.c_str(), pixels.get(), width, height, 3)) {
                        log_info("graphics::opengl", "screenshot saved: {}", path);
                        clipboard::copy_image(path);
                        overlay::notifications::add(overlay::notifications::Severity::Success,
                                fmt::format("Screenshot saved: {}", fileutils::basename(path)));
                    } else {
                        log_warning("graphics::opengl", "failed to save screenshot");
                        overlay::notifications::add(overlay::notifications::Severity::Error,
                                "Screenshot failed to save");
                    }
                } catch (const std::exception &error) {
                    log_warning("graphics::opengl", "screenshot save failed: {}", error.what());
                } catch (...) {
                    log_warning("graphics::opengl", "screenshot save failed");
                }
                screenshot_busy = false;
            });
        } catch (...) {
            screenshot_busy = false;
            throw;
        }
    }

    void prepare_window(HWND window) {
        graphics_hook_window(window, nullptr);
        if (GRAPHICS_WINDOWED) return;

        // Legacy OpenGL has no D3D presentation parameters; use a borderless monitor-sized window.
        MONITORINFO monitor {};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const auto style = GetWindowLong(window, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW;
        const auto style_ex = GetWindowLong(window, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE);
        SetWindowLong(window, GWL_STYLE, style);
        SetWindowLong(window, GWL_EXSTYLE, style_ex);
        const auto &rect = monitor.rcMonitor;
        if (!SetWindowPos(window, nullptr, rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS)) {
            log_warning("graphics::opengl", "unable to size fullscreen window, error={}", GetLastError());
        }
    }

    void present(HDC dc) {
        const auto game_context = get_context();
        const auto game_dc = get_dc();
        const HWND window = WindowFromDC(dc);
        const HWND current_window = game_dc ? WindowFromDC(game_dc) : nullptr;
        static std::once_flag swapped;
        std::call_once(swapped, [&] {
            log_info("graphics::opengl", "swap received: context={}, current dc={} (window={}), dc={} (window={})",
                    fmt::ptr(game_context), fmt::ptr(game_dc), fmt::ptr(current_window),
                    fmt::ptr(dc), fmt::ptr(window));
        });
        if (!game_context || !game_dc || !cfg::SCREENRESIZE || !window) return;
        std::unique_lock lock(contexts_mutex);
        auto &state = contexts[game_context];
        // gftools releases its GetDC handle while the context stays current, then swaps a paint DC.
        if (state.owner_window) {
            if (state.owner_window != window) return;
        } else if (game_dc != dc && current_window != window) {
            return;
        }
        const HDC restore_dc = current_window == window ? game_dc : dc;
        static std::once_flag identified;
        std::call_once(identified, [] {
            log_info("graphics::opengl", "game context active: version={}, renderer={}",
                    reinterpret_cast<const char *>(gl.glGetString(GL_VERSION)),
                    reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER)));
        });
        if (state.window != window) {
            state.window = window;
            lock.unlock();
            prepare_window(window);
            // Resizing may replace the drawable. Let the game render its next frame first.
            return;
        }

#ifdef SPICE_OPENGL_PROBE
        probe_frame.tracked = true;
        probe_frame.epoch = probe_epoch.load();
#endif
        graphics_poll_screenshot_hotkey();
        bool screenshot = !screenshot_busy && graphics_screenshot_consume();
        int screen = 0;
#ifdef SPICE_OPENGL_PROBE
        uint64_t capture_request = 0;
        bool capture = graphics_capture_consume_request(&screen, &capture_request);
        ProbeCaptureGuard capture_guard(screen, capture_request, capture);
#else
        bool capture = graphics_capture_consume(&screen);
#endif
        if (capture && screen != 0) {
            graphics_capture_skip(screen);
            capture = false;
        }
        RECT rect {};
        if (!GetClientRect(window, &rect) || IsIconic(window) || rect.right <= 0 || rect.bottom <= 0) {
            if (capture) graphics_capture_skip(screen);
            if (screenshot) graphics_screenshot_trigger();
            return;
        }
        const int width = rect.right;
        const int height = rect.bottom;
        cfg::fullscreen_setting scene;
        bool linear_filter;
        {
            std::lock_guard image_lock(cfg::SCREENRESIZE->image_mutex);
            if (cfg::SCREENRESIZE->enable_screen_resize) {
                scene = cfg::SCREENRESIZE->scene_settings[cfg::SCREENRESIZE->screen_resize_current_scene];
            }
            linear_filter = cfg::SCREENRESIZE->enable_linear_filter;
        }
        const bool fit = !GRAPHICS_WINDOWED || GRAPHICS_WINDOW_BACKBUFFER_SCALE;
        graphics::opengl::ImageRect source {0, 0, width, height};
        if (fit) {
            // The last viewport can be only the test menu's upper display, not the full canvas.
            if (cfg::SCREENRESIZE->init_client_width && cfg::SCREENRESIZE->init_client_height) {
                source = {0, 0, static_cast<int>(cfg::SCREENRESIZE->init_client_width),
                    static_cast<int>(cfg::SCREENRESIZE->init_client_height)};
            } else {
                GLint viewport[4] {};
                gl.glGetIntegerv(GL_VIEWPORT, viewport);
                source = {viewport[0], viewport[1], viewport[2], viewport[3]};
            }
        }
        const bool resize = graphics::opengl::requires_resize(scene) ||
            (fit && (source.x != 0 || source.y != 0 || source.width != width || source.height != height));
        // No extra copy, context switch or GPU synchronization for an identity scene.
#ifdef SPICE_OPENGL_PROBE
        probe_frame.source = source;
        probe_frame.width = width; probe_frame.height = height;
        probe_frame.scene = scene;
        probe_frame.linear = linear_filter; probe_frame.resize = resize;
        const int requested = probe_requested.load();
        if (requested && !state.probe_attempted) {
            state.probe_attempted = true;
            state.probe_available = state.probe.initialize(gl, [](const char *name) -> void * {
                auto address = get_proc_address(name);
                const auto value = reinterpret_cast<uintptr_t>(address);
                if (!address || value <= 3 || value == ~uintptr_t {0}) {
                    address = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
                }
                return reinterpret_cast<void *>(address);
            });
        }
        const bool async_available = state.probe_available && state.probe.async_supported();
        const int selected = requested && state.probe_available && (requested != 2 || async_available) ? requested : 0;
        if (state.probe_capture_pending && (selected != 2 ||
                state.probe_capture_epoch != probe_frame.epoch ||
                !graphics_capture_request_active(0, state.probe_capture_request))) {
            state.probe.abandon_readback();
            state.probe_capture_pending = false;
            graphics_capture_skip_request(0, state.probe_capture_request);
            if (state.probe_capture_epoch == probe_frame.epoch) ++probe_frame.capture_dropped;
        }
        if (state.probe_request_seen != requested || state.probe_mode != selected) {
            state.probe_request_seen = requested;
            state.probe_mode = selected;
            probe_active(selected, state.probe_available,
                    state.probe_attempted ? state.probe.description() : "legacy baseline; shader not initialized",
                    requested == 2 && state.probe_available && !async_available ? "PBO/sync entrypoints unavailable" :
                        state.probe_attempted ? state.probe.error() : "");
        }
        if (selected) {
            if (!state.buffer) {
                GLint double_buffer = GL_FALSE;
                gl.glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
                state.buffer = double_buffer ? GL_BACK : GL_FRONT;
            }
            auto &timing = probe_frame.timings;
            auto ready = state.probe.poll_readback(timing);
            if (ready.pixels && state.probe_capture_pending) {
                const bool accepted = graphics_capture_enqueue_request(0, state.probe_capture_request,
                        std::shared_ptr<uint8_t[]>(std::move(ready.pixels)), ready.width, ready.height);
                state.probe_capture_pending = false;
                if (accepted) ++probe_frame.capture_completed;
                else ++probe_frame.capture_dropped;
            } else if (state.probe_capture_pending && !state.probe.readback_pending()) {
                state.probe_capture_pending = false;
                graphics_capture_skip_request(0, state.probe_capture_request);
                ++probe_frame.capture_dropped;
            }
            const bool drawn = !resize || state.probe.draw(source, width, height, scene,
                    linear_filter, state.buffer, timing);
            if (drawn) {
                if (screenshot || (capture && selected == 1)) {
                    auto frame = state.probe.read_sync(width, height, state.buffer, timing);
                    if (frame.pixels) {
                        auto shared = std::shared_ptr<uint8_t[]>(std::move(frame.pixels));
                        if (capture) {
                            capture_guard.deliver(shared, frame.width, frame.height);
                        }
                        if (screenshot) save_screenshot(std::move(shared), frame.width, frame.height);
                    } else {
                        if (capture && (selected == 1 || screenshot)) {
                            capture_guard.drop();
                        }
                        if (screenshot) graphics_screenshot_trigger();
                    }
                }
                if (capture && selected == 2 && !screenshot) {
                    if (!state.probe.readback_pending() && graphics_capture_request_active(screen, capture_request)) {
                        state.probe_capture_pending = true;
                        state.probe_capture_request = capture_request;
                        state.probe_capture_epoch = probe_frame.epoch;
                        if (state.probe.issue_readback(width, height, state.buffer, timing)) {
                            capture_guard.pending = false;
                        } else {
                            state.probe_capture_pending = false;
                            capture_guard.drop();
                        }
                    } else {
                        capture_guard.drop();
                    }
                }
                if (state.probe_error_seen != state.probe.error()) {
                    state.probe_error_seen = state.probe.error();
                    probe_active(selected, true, state.probe.description(), state.probe_error_seen);
                }
                return;
            }
            state.probe_available = false;
            state.probe_mode = 0;
            probe_active(0, false, state.probe.description(), state.probe.error());
        }
#endif
        if (!resize && !screenshot && !capture) return;
        if (!state.resize_context) state.resize_context = create_context(dc);
        if (!state.resize_context) {
            if (!state.warned) log_warning("graphics::opengl", "unable to create resize context, error={}", GetLastError());
            state.warned = true;
            if (capture) graphics_capture_skip(screen);
            return;
        }
        // Complete game rendering before accessing its default framebuffer from our context.
        // The isolated compatibility context preserves shaders, FBOs, matrices, texture units,
        // GL error state and all other game state without relying on partial push/pop lists.
#ifdef SPICE_OPENGL_PROBE
        auto stage_start = ProbeClock::now();
#endif
        gl.glFinish();
#ifdef SPICE_OPENGL_PROBE
        probe_frame.finish_ms += probe_elapsed(stage_start);
        stage_start = ProbeClock::now();
#endif
        const bool switched = make_current(dc, state.resize_context);
#ifdef SPICE_OPENGL_PROBE
        probe_frame.context_ms += probe_elapsed(stage_start);
#endif
        bool drawn = false;
        std::unique_ptr<uint8_t[]> pixels;
        if (switched) {
            if (!state.buffer) {
                GLint double_buffer = GL_FALSE;
                gl.glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
                state.buffer = double_buffer ? GL_BACK : GL_FRONT;
            }
            if (resize) {
#ifdef SPICE_OPENGL_PROBE
                stage_start = ProbeClock::now();
#endif
                drawn = state.image.draw(gl, source, width, height, scene, linear_filter, state.buffer);
#ifdef SPICE_OPENGL_PROBE
                probe_frame.timings.draw_ms += probe_elapsed(stage_start);
#endif
            }
            if (screenshot || capture) {
#ifdef SPICE_OPENGL_PROBE
                stage_start = ProbeClock::now();
#endif
                pixels = graphics::opengl::read_pixels(gl, width, height, state.buffer);
#ifdef SPICE_OPENGL_PROBE
                probe_frame.timings.read_issue_ms += probe_elapsed(stage_start);
#endif
            }
            // A successful synchronous readback already completed the final draw.
            if (drawn && !pixels) {
#ifdef SPICE_OPENGL_PROBE
                stage_start = ProbeClock::now();
#endif
                gl.glFinish();
#ifdef SPICE_OPENGL_PROBE
                probe_frame.finish_ms += probe_elapsed(stage_start);
#endif
            }
        }
        // wglMakeCurrent failure unbinds the old context, so restore it on BOTH paths.
#ifdef SPICE_OPENGL_PROBE
        stage_start = ProbeClock::now();
#endif
        const bool restored = make_current(restore_dc, game_context);
#ifdef SPICE_OPENGL_PROBE
        probe_frame.context_ms += probe_elapsed(stage_start);
#endif
        if ((!switched || (resize && !drawn) || ((screenshot || capture) && !pixels) || !restored) && !state.warned) {
            log_warning("graphics::opengl", "frame processing failed (switched={}, drawn={}, restored={}, error={})",
                    switched, drawn, restored, GetLastError());
            state.warned = true;
        }
        if (drawn && restored && !state.logged) {
            log_info("graphics::opengl", "image resize active: {}x{} -> {}x{}",
                    source.width, source.height, width, height);
            state.logged = true;
        }
        lock.unlock();
        if (pixels && restored) {
            auto shared = std::shared_ptr<uint8_t[]>(std::move(pixels));
            if (capture) {
#ifdef SPICE_OPENGL_PROBE
                capture_guard.deliver(shared, width, height);
#else
                graphics_capture_enqueue(screen, shared, width, height);
#endif
            }
            if (screenshot) save_screenshot(std::move(shared), width, height);
        } else if (capture) {
#ifdef SPICE_OPENGL_PROBE
            capture_guard.drop();
#else
            graphics_capture_skip(screen);
#endif
        }
    }

    void process_frame(HDC dc) {
        try {
            present(dc);
        } catch (const std::exception &error) {
            // Allocation/worker failures must never unwind through the game's swap call.
            log_warning("graphics::opengl", "frame processing failed: {}", error.what());
        } catch (...) {
            log_warning("graphics::opengl", "frame processing failed");
        }
    }

    BOOL WINAPI swap_buffers_hook(HDC dc) {
        if (in_swap) return swap_buffers(dc);
        in_swap = true;
#ifdef SPICE_OPENGL_PROBE
        probe_frame = {};
        const auto start = ProbeClock::now();
#endif
        process_frame(dc);
#ifdef SPICE_OPENGL_PROBE
        const double hook_ms = probe_elapsed(start);
        const auto swap_start = ProbeClock::now();
#endif
        const auto result = swap_buffers(dc);
#ifdef SPICE_OPENGL_PROBE
        probe_record_swap(hook_ms, probe_elapsed(swap_start), start);
#endif
        in_swap = false;
        return result;
    }

    BOOL WINAPI swap_layers_hook(HDC dc, UINT planes) {
        if (in_swap) return swap_layers(dc, planes);
        in_swap = true;
#ifdef SPICE_OPENGL_PROBE
        probe_frame = {};
        const auto start = ProbeClock::now();
#endif
        if (planes & WGL_SWAP_MAIN_PLANE) process_frame(dc);
#ifdef SPICE_OPENGL_PROBE
        const double hook_ms = probe_elapsed(start);
        const auto swap_start = ProbeClock::now();
#endif
        const auto result = swap_layers(dc, planes);
#ifdef SPICE_OPENGL_PROBE
        probe_record_swap(hook_ms, probe_elapsed(swap_start), start);
#endif
        in_swap = false;
        return result;
    }

    BOOL WINAPI make_current_hook(HDC dc, HGLRC context) {
        const auto result = make_current(dc, context);
        if (result && context) {
            const auto window = dc ? WindowFromDC(dc) : nullptr;
            try {
                std::lock_guard lock(contexts_mutex);
                contexts[context].owner_window = window;
            } catch (...) {
                log_warning("graphics::opengl", "unable to record context window");
            }
        }
        return result;
    }

    BOOL WINAPI delete_context_hook(HGLRC context) {
        // A failed deletion (e.g. a context current on another thread) must retain its resources.
        std::lock_guard lock(contexts_mutex);
#ifdef SPICE_OPENGL_PROBE
        const auto probe_entry = contexts.find(context);
        if (probe_entry != contexts.end()) {
            auto &state = probe_entry->second;
            if (state.probe_capture_pending) {
                graphics_capture_skip_request(0, state.probe_capture_request);
                state.probe_capture_pending = false;
                if (state.probe_capture_epoch == probe_epoch.load()) {
                    std::lock_guard stats_lock(probe_mutex);
                    ++probe_stats.capture_dropped;
                }
            }
            if (get_context() == context) {
                state.probe.release();
                state.probe_attempted = false;
                state.probe_available = false;
                state.probe_request_seen = -1;
            }
        }
#endif
        const auto result = delete_context(context);
        if (result) {
            const auto entry = contexts.find(context);
            if (entry != contexts.end()) {
                if (entry->second.resize_context) delete_context(entry->second.resize_context);
                contexts.erase(entry);
            }
        }
        return result;
    }
}

#ifdef SPICE_OPENGL_PROBE
std::string graphics_opengl_probe(std::string_view mode, bool reset) {
    auto snapshot = std::make_unique<ProbeStats>();
    {
        std::lock_guard lock(probe_mutex);
        if (!mode.empty()) {
            const int selected = mode == "shader" ? 1 : mode == "shader-pbo" ? 2 : 0;
            probe_requested = selected;
        }
        if (reset) {
            for (auto &metric : probe_stats.metrics) metric = {};
            probe_stats.frames = 0;
            probe_stats.capture_issued = 0;
            probe_stats.capture_completed = 0;
            probe_stats.capture_dropped = 0;
            ++probe_epoch;
        }
        *snapshot = probe_stats;
    }
    rapidjson::Document doc(rapidjson::kObjectType);
    auto &alloc = doc.GetAllocator();
    doc.AddMember("build", rapidjson::Value(SPICE_OPENGL_PROBE_REVISION, alloc), alloc);
    doc.AddMember("requested_mode", rapidjson::Value(probe_modes[probe_requested.load()], alloc), alloc);
    doc.AddMember("active_mode", rapidjson::Value(probe_modes[snapshot->active_mode], alloc), alloc);
    doc.AddMember("available", snapshot->available, alloc);
    doc.AddMember("description", rapidjson::Value(snapshot->description.c_str(), alloc), alloc);
    doc.AddMember("error", rapidjson::Value(snapshot->error.c_str(), alloc), alloc);
    doc.AddMember("frames", snapshot->frames, alloc);
    doc.AddMember("capture_issued", snapshot->capture_issued, alloc);
    doc.AddMember("capture_completed", snapshot->capture_completed, alloc);
    doc.AddMember("capture_dropped", snapshot->capture_dropped, alloc);
    rapidjson::Value source(rapidjson::kArrayType), output(rapidjson::kArrayType);
    source.PushBack(snapshot->source.x, alloc).PushBack(snapshot->source.y, alloc)
            .PushBack(snapshot->source.width, alloc).PushBack(snapshot->source.height, alloc);
    output.PushBack(snapshot->width, alloc).PushBack(snapshot->height, alloc);
    doc.AddMember("source", source, alloc);
    doc.AddMember("output", output, alloc);
    doc.AddMember("resize_active", snapshot->resize, alloc);
    doc.AddMember("linear_filter", snapshot->linear, alloc);
    rapidjson::Value scene(rapidjson::kObjectType);
    scene.AddMember("scale_x", snapshot->scene.scale_x, alloc);
    scene.AddMember("scale_y", snapshot->scene.scale_y, alloc);
    scene.AddMember("offset_x", snapshot->scene.offset_x, alloc);
    scene.AddMember("offset_y", snapshot->scene.offset_y, alloc);
    scene.AddMember("duplicate", static_cast<int>(snapshot->scene.duplicate), alloc);
    doc.AddMember("scene", scene, alloc);
    doc.AddMember("gpu_timing_measured", false, alloc);
    rapidjson::Value metrics(rapidjson::kObjectType);
    for (size_t index = 0; index < snapshot->metrics.size(); ++index) {
        const auto metric = snapshot->metrics[index].summarize();
        rapidjson::Value value(rapidjson::kObjectType);
        value.AddMember("count", metric.count, alloc);
        value.AddMember("samples", static_cast<uint64_t>(metric.samples), alloc);
        value.AddMember("mean", metric.mean, alloc);
        value.AddMember("p50", metric.p50, alloc);
        value.AddMember("p95", metric.p95, alloc);
        value.AddMember("p99", metric.p99, alloc);
        value.AddMember("max", metric.maximum, alloc);
        metrics.AddMember(rapidjson::Value(probe_metric_names[index], alloc), value, alloc);
    }
    doc.AddMember("metrics", metrics, alloc);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return {buffer.GetString(), buffer.GetSize()};
}
#endif

void graphics_opengl_window_created(HWND window, std::string_view title) {
    // Preserve the game's canvas before launcher settings resize the main window.
    if (window && eamuse_get_game() == "Jubeat" &&
            GetModuleHandleW(L"gftools.dll") && title.ends_with(" - jubeat")) {
        if (!GRAPHICS_WINDOWED) {
            RECT rect {};
            if (cfg::SCREENRESIZE && GetClientRect(window, &rect) && rect.right > 0 && rect.bottom > 0) {
                cfg::SCREENRESIZE->init_client_width = rect.right;
                cfg::SCREENRESIZE->init_client_height = rect.bottom;
            }
            return;
        }
        graphics_hook_window(window, nullptr);
        RECT rect {};
        if (GetClientRect(window, &rect)) {
            log_info("graphics::opengl", "main window client after initialization: {}x{}",
                    rect.right - rect.left, rect.bottom - rect.top);
        }
    }
}

void graphics_opengl_init() {
    // This legacy path is for jubeat. Keep hook and context behavior unchanged for other games.
    if (eamuse_get_game() != "Jubeat") return;
    const auto module = LoadLibraryW(L"opengl32.dll");
    if (!module) return;
    if (!gl.load([&](const char *name) { return GetProcAddress(module, name); })) return;
#define LOAD_WGL(name, target) target = reinterpret_cast<decltype(target)>(GetProcAddress(module, #name)); if (!target) return;
    LOAD_WGL(wglGetCurrentContext, get_context)
    LOAD_WGL(wglGetCurrentDC, get_dc)
    LOAD_WGL(wglCreateContext, create_context)
    LOAD_WGL(wglMakeCurrent, make_current)
    LOAD_WGL(wglDeleteContext, delete_context)
#ifdef SPICE_OPENGL_PROBE
    LOAD_WGL(wglGetProcAddress, get_proc_address)
#endif
#undef LOAD_WGL
    swap_buffers = reinterpret_cast<decltype(swap_buffers)>(
            GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "SwapBuffers"));
    swap_layers = reinterpret_cast<decltype(swap_layers)>(GetProcAddress(module, "wglSwapLayerBuffers"));
    if (!swap_buffers) return;
    if (!detour::trampoline_try("gdi32.dll", "SwapBuffers", swap_buffers_hook, &swap_buffers)) {
        log_warning("graphics::opengl", "unable to hook SwapBuffers; image resize unavailable");
        return;
    }
    // Some engines use wglSwapLayerBuffers rather than GDI SwapBuffers.
    if (swap_layers) detour::trampoline_try("opengl32.dll", "wglSwapLayerBuffers", swap_layers_hook, &swap_layers);
    if (!detour::trampoline_try("opengl32.dll", "wglMakeCurrent", make_current_hook, &make_current)) {
        log_warning("graphics::opengl", "unable to track context windows; released DC scaling may be unavailable");
    }
    detour::trampoline_try("opengl32.dll", "wglDeleteContext", delete_context_hook, &delete_context);
    log_info("graphics::opengl", "legacy image resize hooks initialized");
#ifdef SPICE_OPENGL_PROBE
    log_info("graphics::opengl", "cabinet comparison build={}, WS capture.opengl_probe enabled, initial mode=legacy; metrics are CPU elapsed times",
            SPICE_OPENGL_PROBE_REVISION);
#endif
}
