#pragma once

#include <windows.h>
#include <GL/gl.h>

#include "cfg/screen_resize.h"

namespace graphics::opengl {
    // Only OpenGL 1.1 entry points: no shaders, FBOs, NPOT textures or newer XP imports.
#define SPICE_GL_FUNCTIONS(X) \
    X(glGetString) X(glGetIntegerv) X(glGetError) X(glGenTextures) X(glBindTexture) \
    X(glTexImage2D) X(glTexParameteri) X(glCopyTexSubImage2D) \
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

    // Owned by a private WGL context. Deleting that context releases its texture.
    struct ImageResize {
        GLuint texture = 0;
        int texture_width = 0;
        int texture_height = 0;

        // Caller has made its private, default-state context current on the game drawable.
        bool draw(const GlApi &gl, int width, int height,
                const cfg::fullscreen_setting &scene, bool linear_filter, GLenum buffer);
    };
}
