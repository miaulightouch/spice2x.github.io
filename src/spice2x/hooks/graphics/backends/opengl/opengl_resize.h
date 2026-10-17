#pragma once

#include <cstdint>
#include <memory>

#include <windows.h>
#include <GL/gl.h>

#include "cfg/screen_resize.h"

namespace graphics::opengl {
    // Only OpenGL 1.1 entry points: no shaders, FBOs, NPOT textures or newer XP imports.
#define SPICE_GL_FUNCTIONS(X) \
    X(glGetString) X(glGetIntegerv) X(glGetError) X(glGenTextures) X(glBindTexture) \
    X(glTexImage2D) X(glTexParameteri) X(glCopyTexSubImage2D) \
    X(glReadPixels) X(glPixelStorei) \
    X(glReadBuffer) X(glDrawBuffer) X(glViewport) X(glClearColor) X(glClear) \
    X(glMatrixMode) X(glLoadIdentity) X(glOrtho) X(glEnable) X(glDisable) \
    X(glTexEnvi) X(glBegin) X(glEnd) X(glTexCoord2f) X(glVertex2f) X(glFinish)

    struct GlApi {
#define SPICE_GL_MEMBER(name) decltype(::name) *name = nullptr;
        SPICE_GL_FUNCTIONS(SPICE_GL_MEMBER)
#undef SPICE_GL_MEMBER

        template<typename Resolver> bool load(Resolver resolve) {
#define SPICE_GL_LOAD(name) name = reinterpret_cast<decltype(name)>(resolve(#name)); if (!name) return false;
            SPICE_GL_FUNCTIONS(SPICE_GL_LOAD)
#undef SPICE_GL_LOAD
            return true;
        }
    };

    struct ImageRect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    // Filtering/aspect flags alone do not change a pixel-aligned identity image.
    inline bool requires_resize(const cfg::fullscreen_setting &scene) {
        return scene.scale_x != 1.f || scene.scale_y != 1.f ||
            scene.offset_x != 0 || scene.offset_y != 0 ||
            scene.duplicate != cfg::ScreenDuplicateMode::None;
    }

    // Owned by a private WGL context. Deleting that context releases its texture.
    struct ImageResize {
        GLuint texture = 0;
        int texture_width = 0;
        int texture_height = 0;
        GLint maximum_texture_size = 0;
        GLint maximum_viewport_width = 0;
        GLint maximum_viewport_height = 0;
        GLint texture_filter = 0;

        // Caller has made its private, default-state context current on the game drawable.
        // Caller synchronizes before switching contexts when no readback will follow.
        bool draw(const GlApi &gl, const ImageRect &source, int output_width, int output_height,
                const cfg::fullscreen_setting &scene, bool linear_filter, GLenum buffer);
    };

    // Caller must have made the private/default context current; this never touches a game context.
    // The returned packed RGB rows are ordered top-down.
    std::unique_ptr<uint8_t[]> read_pixels(const GlApi &gl, int width, int height, GLenum buffer);
}
