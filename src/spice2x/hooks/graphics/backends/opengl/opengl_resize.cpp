#include "opengl_resize.h"

#include <cmath>

namespace graphics::opengl {
    static int texture_extent(int value, int maximum) {
        int extent = 1;
        while (extent < value && extent <= maximum / 2) extent *= 2;
        return extent >= value && extent <= maximum ? extent : 0;
    }

    bool ImageResize::draw(const GlApi &gl, int width, int height,
            const cfg::fullscreen_setting &scene, bool linear_filter, GLenum buffer) {
        if (width <= 0 || height <= 0 || !std::isfinite(scene.scale_x) ||
                !std::isfinite(scene.scale_y) || scene.scale_x < .6f || scene.scale_x > 2.5f ||
                scene.scale_y < .6f || scene.scale_y > 2.5f) return false;

        // Immutable for this private context; do not query the driver every frame.
        if (!maximum_texture_size) gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
        const int tw = texture_extent(width, maximum_texture_size);
        const int th = texture_extent(height, maximum_texture_size);
        if (!tw || !th) return false;

        // This is our context's error state, never the game's.
        for (int i = 0; i < 32 && gl.glGetError() != GL_NO_ERROR; ++i) {}
        if (!texture) gl.glGenTextures(1, &texture);
        gl.glBindTexture(GL_TEXTURE_2D, texture);
        if (texture_width != tw || texture_height != th) {
            gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            if (gl.glGetError() != GL_NO_ERROR) return false;
            texture_width = tw;
            texture_height = th;
        }
        const auto filter = linear_filter ? GL_LINEAR : GL_NEAREST;
        if (texture_filter != filter) {
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            texture_filter = filter;
        }
        gl.glReadBuffer(buffer);
        gl.glDrawBuffer(buffer);
        gl.glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        // Preserve the original image when allocation/readback is unsupported by the driver.
        if (gl.glGetError() != GL_NO_ERROR) return false;

        gl.glViewport(0, 0, width, height);
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glDisable(GL_DEPTH_TEST);
        gl.glDisable(GL_BLEND);
        gl.glDisable(GL_CULL_FACE);
        gl.glClearColor(0, 0, 0, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glMatrixMode(GL_PROJECTION);
        gl.glLoadIdentity();
        gl.glOrtho(0, width, 0, height, -1, 1);
        gl.glMatrixMode(GL_MODELVIEW);
        gl.glLoadIdentity();
        gl.glEnable(GL_TEXTURE_2D);
        gl.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        const float w = width * scene.scale_x;
        const float h = height * scene.scale_y;
        // Match D3D9's source-pixel offsets. API Y is positive down; OpenGL Y is up.
        const float left = (width - w) * .5f + scene.offset_x * scene.scale_x;
        const float bottom = (height - h) * .5f - scene.offset_y * scene.scale_y;
        const auto rectangle = [&](float x0, float y0, float x1, float y1,
                float u0, float v0, float u1, float v1) {
            gl.glTexCoord2f(u0, v0); gl.glVertex2f(x0, y0);
            gl.glTexCoord2f(u1, v0); gl.glVertex2f(x1, y0);
            gl.glTexCoord2f(u1, v1); gl.glVertex2f(x1, y1);
            gl.glTexCoord2f(u0, v1); gl.glVertex2f(x0, y1);
        };
        const auto quad = [&](float x) {
            gl.glBegin(GL_QUADS);
            if (linear_filter) {
                // Clamp only edge strips so interior pixels keep their original sampling alignment.
                const float xs[]{x, x + scene.scale_x * .5f, x + w - scene.scale_x * .5f, x + w};
                const float ys[]{bottom, bottom + scene.scale_y * .5f,
                    bottom + h - scene.scale_y * .5f, bottom + h};
                const float us[]{.5f / tw, .5f / tw, (width - .5f) / tw, (width - .5f) / tw};
                const float vs[]{.5f / th, .5f / th, (height - .5f) / th, (height - .5f) / th};
                for (int row = 0; row < 3; ++row) {
                    for (int column = 0; column < 3; ++column) {
                        rectangle(xs[column], ys[row], xs[column + 1], ys[row + 1],
                            us[column], vs[row], us[column + 1], vs[row + 1]);
                    }
                }
            } else {
                rectangle(x, bottom, x + w, bottom + h,
                    0.f, 0.f, static_cast<float>(width) / tw, static_cast<float>(height) / th);
            }
            gl.glEnd();
        };
        quad(left);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyLeft) quad(left - w);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyRight) quad(left + w);
        gl.glFinish();
        return gl.glGetError() == GL_NO_ERROR;
    }
}
