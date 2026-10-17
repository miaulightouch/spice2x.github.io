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

    struct Context {
        HGLRC resize_context = nullptr;
        HWND window = nullptr;
        graphics::opengl::ImageResize image;
        GLenum buffer = 0;
        bool warned = false;
        bool logged = false;
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
        if (!game_context || !game_dc || !cfg::SCREENRESIZE) return;
        if (game_dc != dc && WindowFromDC(game_dc) != WindowFromDC(dc)) return;
        static std::once_flag identified;
        std::call_once(identified, [] {
            log_info("graphics::opengl", "game context active: version={}, renderer={}",
                    reinterpret_cast<const char *>(gl.glGetString(GL_VERSION)),
                    reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER)));
        });
        const HWND window = WindowFromDC(dc);
        if (!window) return;
        std::unique_lock lock(contexts_mutex);
        auto &state = contexts[game_context];
        if (state.window != window) {
            state.window = window;
            lock.unlock();
            prepare_window(window);
            // Resizing may replace the drawable. Let the game render its next frame first.
            return;
        }

        graphics_poll_screenshot_hotkey();
        bool screenshot = !screenshot_busy && graphics_screenshot_consume();
        int screen = 0;
        bool capture = graphics_capture_consume(&screen);
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
            GLint viewport[4] {};
            gl.glGetIntegerv(GL_VIEWPORT, viewport);
            source = {viewport[0], viewport[1], viewport[2], viewport[3]};
        }
        const bool resize = graphics::opengl::requires_resize(scene) ||
            (fit && (source.x != 0 || source.y != 0 || source.width != width || source.height != height));
        // No extra copy, context switch or GPU synchronization for an identity scene.
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
        gl.glFinish();
        const bool switched = make_current(dc, state.resize_context);
        bool drawn = false;
        std::unique_ptr<uint8_t[]> pixels;
        if (switched) {
            if (!state.buffer) {
                GLint double_buffer = GL_FALSE;
                gl.glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
                state.buffer = double_buffer ? GL_BACK : GL_FRONT;
            }
            if (resize) drawn = state.image.draw(gl, source, width, height, scene, linear_filter, state.buffer);
            if (screenshot || capture) pixels = graphics::opengl::read_pixels(gl, width, height, state.buffer);
            // A successful synchronous readback already completed the final draw.
            if (drawn && !pixels) gl.glFinish();
        }
        // wglMakeCurrent failure unbinds the old context, so restore it on BOTH paths.
        const bool restored = make_current(game_dc, game_context);
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
            if (capture) graphics_capture_enqueue(screen, shared, width, height);
            if (screenshot) save_screenshot(std::move(shared), width, height);
        } else if (capture) {
            graphics_capture_skip(screen);
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
        process_frame(dc);
        const auto result = swap_buffers(dc);
        in_swap = false;
        return result;
    }

    BOOL WINAPI swap_layers_hook(HDC dc, UINT planes) {
        if (in_swap) return swap_layers(dc, planes);
        in_swap = true;
        if (planes & WGL_SWAP_MAIN_PLANE) process_frame(dc);
        const auto result = swap_layers(dc, planes);
        in_swap = false;
        return result;
    }

    BOOL WINAPI delete_context_hook(HGLRC context) {
        // A failed deletion (e.g. a context current on another thread) must retain its resources.
        std::lock_guard lock(contexts_mutex);
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
    detour::trampoline_try("opengl32.dll", "wglDeleteContext", delete_context_hook, &delete_context);
    log_info("graphics::opengl", "legacy image resize hooks initialized");
}
