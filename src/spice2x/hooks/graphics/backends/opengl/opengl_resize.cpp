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

        GLint maximum = 0;
        gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        const int tw = texture_extent(width, maximum);
        const int th = texture_extent(height, maximum);
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
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
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
        const float u = static_cast<float>(width) / tw;
        const float v = static_cast<float>(height) / th;
        const auto quad = [&](float x) {
            gl.glBegin(GL_QUADS);
            gl.glTexCoord2f(0, 0); gl.glVertex2f(x, bottom);
            gl.glTexCoord2f(u, 0); gl.glVertex2f(x + w, bottom);
            gl.glTexCoord2f(u, v); gl.glVertex2f(x + w, bottom + h);
            gl.glTexCoord2f(0, v); gl.glVertex2f(x, bottom + h);
            gl.glEnd();
        };
        quad(left);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyLeft) quad(left - w);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyRight) quad(left + w);
        gl.glFinish();
        return gl.glGetError() == GL_NO_ERROR;
    }
}
