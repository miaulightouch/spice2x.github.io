#pragma once

// The OpenGL hook is a Win32/WGL entry point, but the state machine around it
// is portable.  This shim supplies only the Win32, WGL and renderer boundary
// needed by backend_test.cpp; it deliberately does not try to render pixels.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using BOOL = int;
using BYTE = unsigned char;
using DWORD = std::uint32_t;
using FARPROC = void *;
using HDC = void *;
using HGLRC = void *;
using HMODULE = void *;
using HMONITOR = void *;
using HWND = void *;
using GLint = int;
using GLenum = unsigned int;
using UINT = unsigned int;
using LONG = std::int32_t;
using GLboolean = unsigned char;
using GLubyte = unsigned char;
using LPVOID = void *;

#ifndef WINAPI
#define WINAPI
#endif

constexpr BOOL FALSE = 0;
constexpr BOOL TRUE = 1;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_TRUE = 1;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_DOUBLEBUFFER = 0x0C32;
constexpr GLenum GL_BACK = 0x0405;
constexpr GLenum GL_FRONT = 0x0404;
constexpr UINT WGL_SWAP_MAIN_PLANE = 0x00000001;
constexpr int MONITOR_DEFAULTTONEAREST = 2;
constexpr int GWL_STYLE = -16;
constexpr int GWL_EXSTYLE = -20;
constexpr LONG WS_OVERLAPPEDWINDOW = 0x00CF0000;
constexpr LONG WS_EX_CLIENTEDGE = 0x00000200;
constexpr LONG WS_EX_WINDOWEDGE = 0x00000100;
constexpr UINT SWP_NOZORDER = 0x0004;
constexpr UINT SWP_NOACTIVATE = 0x0010;
constexpr UINT SWP_FRAMECHANGED = 0x0020;
constexpr UINT SWP_ASYNCWINDOWPOS = 0x4000;

struct RECT {
    LONG left = 0;
    LONG top = 0;
    LONG right = 0;
    LONG bottom = 0;
};

struct MONITORINFO {
    DWORD cbSize = 0;
    RECT rcMonitor {};
};

struct D3DPRESENT_PARAMETERS {};

namespace cfg {
    enum class ScreenDuplicateMode {
        None,
        CopyLeft,
        CopyRight,
    };

    struct fullscreen_setting {
        float scale_x = 1.f;
        float scale_y = 1.f;
        int offset_x = 0;
        int offset_y = 0;
        ScreenDuplicateMode duplicate = ScreenDuplicateMode::None;
    };

    struct ScreenResize {
        std::mutex image_mutex;
        bool enable_screen_resize = false;
        int screen_resize_current_scene = 0;
        fullscreen_setting scene_settings[8] {};
        bool enable_linear_filter = true;
    };

    inline ScreenResize *SCREENRESIZE = nullptr;
}

namespace opengl_test {
    struct State {
        struct DrawRecord {
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
        };

        HGLRC current_context = reinterpret_cast<HGLRC>(std::uintptr_t {0x11});
        HDC current_dc = reinterpret_cast<HDC>(std::uintptr_t {0x22});
        HGLRC private_context = reinterpret_cast<HGLRC>(std::uintptr_t {0x33});
        HWND window = reinterpret_cast<HWND>(std::uintptr_t {0x44});
        std::map<HDC, HWND> dc_windows;

        bool window_from_dc_success = true;
        bool client_rect_success = true;
        bool iconic = false;
        bool create_context_success = true;
        bool read_pixels_success = true;
        bool draw_success = true;
        bool double_buffer = true;
        bool delete_context_success = true;
        bool monitor_success = true;
        bool set_window_pos_success = true;
        bool legacy_game_module = true;
        std::string game = "Jubeat";

        RECT client_rect {0, 0, 640, 480};
        GLint viewport[4] {0, 0, 640, 480};
        RECT monitor_rect {0, 0, 1920, 1080};
        LONG style = WS_OVERLAPPEDWINDOW;
        LONG style_ex = WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE;
        DWORD last_error = 0;

        std::deque<bool> make_current_results;
        std::deque<int> capture_requests;
        std::vector<std::string> events;
        std::vector<HGLRC> deleted_contexts;
        std::vector<cfg::fullscreen_setting> drawn_scenes;
        std::vector<DrawRecord> drawn_sources;
        std::vector<GLenum> drawn_buffers;
        std::vector<GLenum> read_buffers;

        int finish_calls = 0;
        int get_integerv_calls = 0;
        int viewport_queries = 0;
        int create_context_calls = 0;
        int make_current_calls = 0;
        int draw_calls = 0;
        int read_calls = 0;
        int capture_skips = 0;
        int capture_enqueues = 0;
        int screenshot_triggers = 0;
        int screenshot_saves = 0;
        int graphics_hook_calls = 0;
        int monitor_calls = 0;
        int set_window_long_calls = 0;
        int set_window_pos_calls = 0;
        int swap_buffer_calls = 0;
        int swap_layer_calls = 0;
        int get_client_rect_calls = 0;
        bool screenshot_pending = false;
        bool reenter_swap = false;
        bool reenter_layers = false;
        bool reentered_swap = false;
        bool reentered_layers = false;
        int last_set_window_x = 0;
        int last_set_window_y = 0;
        int last_set_window_width = 0;
        int last_set_window_height = 0;
        UINT last_set_window_flags = 0;
        HWND last_hook_window = nullptr;
        int last_capture_screen = -1;
        int last_capture_width = 0;
        int last_capture_height = 0;
    };

    inline State state;
    inline bool graphics_windowed = true;
    inline bool graphics_window_backbuffer_scale = false;

    inline void reset() {
        state = State {};
    }

    inline void event(std::string_view name) {
        state.events.emplace_back(name);
    }

    inline bool next_make_current_result() {
        if (state.make_current_results.empty()) return true;
        const bool result = state.make_current_results.front();
        state.make_current_results.pop_front();
        return result;
    }

    inline HGLRC get_context() {
        return state.current_context;
    }

    inline HDC get_dc() {
        return state.current_dc;
    }

    inline HGLRC create_context(HDC) {
        ++state.create_context_calls;
        event("create_context");
        return state.create_context_success ? state.private_context : nullptr;
    }

    inline BOOL make_current(HDC dc, HGLRC context) {
        ++state.make_current_calls;
        event("make_current");
        if (!next_make_current_result()) {
            // WGL clears the current binding when this call fails.  This is
            // intentional: the hook must still attempt to restore the game.
            state.current_dc = nullptr;
            state.current_context = nullptr;
            return FALSE;
        }
        state.current_dc = dc;
        state.current_context = context;
        return TRUE;
    }

    inline BOOL delete_context(HGLRC context) {
        state.deleted_contexts.push_back(context);
        event("delete_context");
        return state.delete_context_success ? TRUE : FALSE;
    }

    inline BOOL swap_buffers(HDC) {
        ++state.swap_buffer_calls;
        event("swap_buffers");
        return TRUE;
    }

    inline BOOL swap_layers(HDC, UINT) {
        ++state.swap_layer_calls;
        event("swap_layers");
        return TRUE;
    }

    inline const unsigned char *get_string(GLenum value) {
        static const unsigned char version[] = "portable-test-version";
        static const unsigned char renderer[] = "portable-test-renderer";
        return value == GL_RENDERER ? renderer : version;
    }

    inline void get_integerv(GLenum value, GLint *result) {
        ++state.get_integerv_calls;
        if (value == GL_VIEWPORT) {
            ++state.viewport_queries;
            std::memcpy(result, state.viewport, sizeof(state.viewport));
        } else if (value == GL_DOUBLEBUFFER) {
            *result = state.double_buffer ? GL_TRUE : GL_FALSE;
        } else {
            *result = 0;
        }
    }

    inline void finish() {
        ++state.finish_calls;
        event("finish");
    }
}

namespace graphics::opengl {
    struct GlApi {
        std::function<const unsigned char *(GLenum)> glGetString;
        std::function<void(GLenum, GLint *)> glGetIntegerv;
        std::function<void()> glFinish;

        template<class Loader>
        bool load(Loader &&) {
            return true;
        }
    };

    struct ImageRect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    inline bool requires_resize(const cfg::fullscreen_setting &scene) {
        return scene.scale_x != 1.f || scene.scale_y != 1.f || scene.offset_x != 0 ||
            scene.offset_y != 0 || scene.duplicate != cfg::ScreenDuplicateMode::None;
    }

    struct ImageResize {
        bool draw(const GlApi &, const ImageRect &source, int width, int height,
                const cfg::fullscreen_setting &scene, bool, GLenum buffer) {
            (void)source;
            (void)width;
            (void)height;
            ++opengl_test::state.draw_calls;
            opengl_test::state.drawn_sources.push_back({source.x, source.y, source.width, source.height});
            opengl_test::state.drawn_scenes.push_back(scene);
            opengl_test::state.drawn_buffers.push_back(buffer);
            opengl_test::event("draw");
            return opengl_test::state.draw_success;
        }
    };

    inline std::unique_ptr<std::uint8_t[]> read_pixels(const GlApi &, int width, int height, GLenum buffer) {
        ++opengl_test::state.read_calls;
        opengl_test::state.read_buffers.push_back(buffer);
        opengl_test::event("read");
        if (!opengl_test::state.read_pixels_success || width <= 0 || height <= 0) return nullptr;
        const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
        auto pixels = std::make_unique<std::uint8_t[]>(size);
        for (std::size_t i = 0; i < size; ++i) pixels[i] = static_cast<std::uint8_t>(i);
        return pixels;
    }
}

inline HGLRC wglGetCurrentContext() {
    return opengl_test::get_context();
}

inline HDC wglGetCurrentDC() {
    return opengl_test::get_dc();
}

inline HGLRC wglCreateContext(HDC dc) {
    return opengl_test::create_context(dc);
}

inline BOOL wglMakeCurrent(HDC dc, HGLRC context) {
    return opengl_test::make_current(dc, context);
}

inline BOOL wglDeleteContext(HGLRC context) {
    return opengl_test::delete_context(context);
}

inline BOOL WINAPI SwapBuffers(HDC dc) {
    return opengl_test::swap_buffers(dc);
}

inline BOOL WINAPI wglSwapLayerBuffers(HDC dc, UINT planes) {
    return opengl_test::swap_layers(dc, planes);
}

inline HWND WindowFromDC(HDC dc) {
    const auto equivalent_dc = reinterpret_cast<HDC>(std::uintptr_t {0x99});
    if (!opengl_test::state.window_from_dc_success) return nullptr;
    const auto override = opengl_test::state.dc_windows.find(dc);
    if (override != opengl_test::state.dc_windows.end()) return override->second;
    if (dc == opengl_test::state.current_dc || dc == equivalent_dc) return opengl_test::state.window;
    return nullptr;
}

inline BOOL GetClientRect(HWND, RECT *rect) {
    ++opengl_test::state.get_client_rect_calls;
    if (!opengl_test::state.client_rect_success) return FALSE;
    *rect = opengl_test::state.client_rect;
    return TRUE;
}

inline BOOL IsIconic(HWND) {
    return opengl_test::state.iconic ? TRUE : FALSE;
}

inline HMONITOR MonitorFromWindow(HWND, DWORD) {
    return reinterpret_cast<HMONITOR>(std::uintptr_t {0x55});
}

inline BOOL GetMonitorInfo(HMONITOR, MONITORINFO *info) {
    ++opengl_test::state.monitor_calls;
    if (!opengl_test::state.monitor_success) return FALSE;
    info->rcMonitor = opengl_test::state.monitor_rect;
    return TRUE;
}

inline LONG GetWindowLong(HWND, int index) {
    return index == GWL_EXSTYLE ? opengl_test::state.style_ex : opengl_test::state.style;
}

inline LONG SetWindowLong(HWND, int index, LONG value) {
    ++opengl_test::state.set_window_long_calls;
    if (index == GWL_EXSTYLE) opengl_test::state.style_ex = value;
    else opengl_test::state.style = value;
    return value;
}

inline BOOL SetWindowPos(HWND, HWND, int x, int y, int width, int height, UINT flags) {
    ++opengl_test::state.set_window_pos_calls;
    opengl_test::state.last_set_window_x = x;
    opengl_test::state.last_set_window_y = y;
    opengl_test::state.last_set_window_width = width;
    opengl_test::state.last_set_window_height = height;
    opengl_test::state.last_set_window_flags = flags;
    return opengl_test::state.set_window_pos_success ? TRUE : FALSE;
}

inline DWORD GetLastError() {
    return opengl_test::state.last_error;
}

inline HMODULE LoadLibraryW(const wchar_t *) {
    return reinterpret_cast<HMODULE>(std::uintptr_t {0x66});
}

inline HMODULE GetModuleHandleW(const wchar_t *name) {
    if (std::wstring_view(name) == L"gftools.dll" && !opengl_test::state.legacy_game_module) return nullptr;
    return reinterpret_cast<HMODULE>(std::uintptr_t {0x77});
}

inline FARPROC GetProcAddress(HMODULE, const char *) {
    return nullptr;
}

inline void graphics_hook_window(HWND window, D3DPRESENT_PARAMETERS *) {
    ++opengl_test::state.graphics_hook_calls;
    opengl_test::state.last_hook_window = window;
    opengl_test::event("hook_window");
}

inline bool GRAPHICS_WINDOWED = true;
inline bool GRAPHICS_WINDOW_BACKBUFFER_SCALE = false;

inline void graphics_poll_screenshot_hotkey() {
    opengl_test::event("poll_screenshot");
}

inline bool graphics_screenshot_consume() {
    const bool pending = opengl_test::state.screenshot_pending;
    opengl_test::state.screenshot_pending = false;
    return pending;
}

inline void graphics_screenshot_trigger() {
    opengl_test::state.screenshot_pending = true;
    ++opengl_test::state.screenshot_triggers;
    opengl_test::event("screenshot_trigger");
}

inline bool graphics_capture_consume(int *screen) {
    if (opengl_test::state.capture_requests.empty()) return false;
    *screen = opengl_test::state.capture_requests.back();
    opengl_test::state.capture_requests.pop_back();
    opengl_test::state.last_capture_screen = *screen;
    opengl_test::event("capture_consume");
    return true;
}

inline void graphics_capture_skip(int screen) {
    ++opengl_test::state.capture_skips;
    opengl_test::state.last_capture_screen = screen;
    opengl_test::event("capture_skip");
}

inline void graphics_capture_enqueue(int screen, std::shared_ptr<std::uint8_t[]> pixels,
        int width, int height) {
    (void)pixels;
    ++opengl_test::state.capture_enqueues;
    opengl_test::state.last_capture_screen = screen;
    opengl_test::state.last_capture_width = width;
    opengl_test::state.last_capture_height = height;
    opengl_test::event("capture_enqueue");
}

inline std::string graphics_screenshot_genpath() {
    return "portable-screenshot.png";
}

class ThreadPool {
public:
    explicit ThreadPool(std::size_t) {}

    template<class Function>
    void add(Function &&function) {
        std::forward<Function>(function)();
    }
};

namespace fpng {
    inline void fpng_init() {}

    inline bool fpng_encode_image_to_file(const char *, const std::uint8_t *, int, int, int) {
        ++opengl_test::state.screenshot_saves;
        opengl_test::event("png");
        return true;
    }
}

namespace clipboard {
    inline void copy_image(const std::string &) {
        opengl_test::event("clipboard");
    }
}

namespace fileutils {
    inline std::string basename(const std::string &path) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
}

namespace overlay::notifications {
    enum class Severity {
        Success,
        Error,
    };

    inline void add(Severity, const std::string &) {}
}

namespace fmt {
    template<class T>
    inline const void *ptr(T *value) { return value; }

    template<class... Arguments>
    inline std::string format(const char *text, Arguments &&...) {
        return text;
    }
}

template<class... Arguments>
inline void log_info(const char *, Arguments &&...) {}

template<class... Arguments>
inline void log_warning(const char *, Arguments &&...) {}

inline std::string eamuse_get_game() {
    return opengl_test::state.game;
}

namespace detour {
    template<class Hook, class Original>
    inline bool trampoline_try(const char *, const char *, Hook, Original *) {
        return true;
    }
}
