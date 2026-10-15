#include "opengl_backend.h"
#include "opengl_resize.h"

#include <map>
#include <mutex>

#include "misc/eamuse.h"
#include "util/detour.h"
#include "util/logging.h"

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

    struct Context {
        HGLRC resize_context = nullptr;
        graphics::opengl::ImageResize image;
        bool warned = false;
        bool logged = false;
    };
    // Game contexts may move between equivalent DC handles; all resources are context-owned.
    std::map<HGLRC, Context> contexts;

    void transform(HDC dc) {
        const auto game_context = get_context();
        const auto game_dc = get_dc();
        if (!game_context || !game_dc || game_dc != dc || !cfg::SCREENRESIZE) return;
        static std::once_flag identified;
        std::call_once(identified, [] {
            log_info("graphics::opengl", "game context active: version={}, renderer={}",
                    reinterpret_cast<const char *>(gl.glGetString(GL_VERSION)),
                    reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER)));
        });
        cfg::fullscreen_setting scene;
        bool linear_filter;
        {
            std::lock_guard lock(cfg::SCREENRESIZE->image_mutex);
            if (!cfg::SCREENRESIZE->enable_screen_resize) return;
            scene = cfg::SCREENRESIZE->scene_settings[cfg::SCREENRESIZE->screen_resize_current_scene];
            linear_filter = cfg::SCREENRESIZE->enable_linear_filter;
        }
        RECT rect {};
        const HWND window = WindowFromDC(dc);
        if (!window || !GetClientRect(window, &rect)) return;
        const auto width = rect.right - rect.left;
        const auto height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return;

        std::lock_guard lock(contexts_mutex);
        auto &state = contexts[game_context];
        if (!state.resize_context) state.resize_context = create_context(dc);
        if (!state.resize_context) {
            if (!state.warned) log_warning("graphics::opengl", "unable to create resize context, error={}", GetLastError());
            state.warned = true;
            return;
        }
        // Complete game rendering before accessing its default framebuffer from our context.
        // The isolated compatibility context preserves shaders, FBOs, matrices, texture units,
        // GL error state and all other game state without relying on partial push/pop lists.
        gl.glFinish();
        const bool switched = make_current(dc, state.resize_context);
        bool drawn = false;
        if (switched) {
            GLint double_buffer = GL_FALSE;
            gl.glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
            drawn = state.image.draw(gl, width, height, scene, linear_filter,
                    double_buffer ? GL_BACK : GL_FRONT);
        }
        // wglMakeCurrent failure unbinds the old context, so restore it on BOTH paths.
        const bool restored = make_current(game_dc, game_context);
        if ((!drawn || !restored) && !state.warned) {
            log_warning("graphics::opengl", "image resize failed (switched={}, drawn={}, restored={}, error={})",
                    switched, drawn, restored, GetLastError());
            state.warned = true;
        }
        if (drawn && restored && !state.logged) {
            log_info("graphics::opengl", "image resize active: {}x{}", width, height);
            state.logged = true;
        }
    }

    BOOL WINAPI swap_buffers_hook(HDC dc) {
        if (in_swap) return swap_buffers(dc);
        in_swap = true;
        transform(dc);
        const auto result = swap_buffers(dc);
        in_swap = false;
        return result;
    }

    BOOL WINAPI swap_layers_hook(HDC dc, UINT planes) {
        if (in_swap) return swap_layers(dc, planes);
        in_swap = true;
        if (planes & WGL_SWAP_MAIN_PLANE) transform(dc);
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
