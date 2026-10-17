#include "opengl_resize.h"

#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace graphics::opengl {
    static int texture_extent(int value, int maximum) {
        if (value <= 0 || maximum <= 0) return 0;
        int extent = 1;
        while (extent < value && extent <= maximum / 2) extent *= 2;
        return extent >= value && extent <= maximum ? extent : 0;
    }

    bool ImageResize::draw(const GlApi &gl, const ImageRect &source,
            int output_width, int output_height,
            const cfg::fullscreen_setting &scene, bool linear_filter, GLenum buffer) {
        if (output_width <= 0 || output_height <= 0 || source.x < 0 || source.y < 0 ||
                source.width <= 0 || source.height <= 0 || source.x > output_width ||
                source.y > output_height || source.width > output_width - source.x ||
                source.height > output_height - source.y || !std::isfinite(scene.scale_x) ||
                !std::isfinite(scene.scale_y) || scene.scale_x < .6f || scene.scale_x > 2.5f ||
                scene.scale_y < .6f || scene.scale_y > 2.5f) return false;

        if (!maximum_viewport_width || !maximum_viewport_height) {
            for (int i = 0; i < 32 && gl.glGetError() != GL_NO_ERROR; ++i) {}
            GLint viewport_limits[2] {};
            gl.glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_limits);
            if (gl.glGetError() != GL_NO_ERROR || viewport_limits[0] <= 0 || viewport_limits[1] <= 0) {
                return false;
            }
            maximum_viewport_width = viewport_limits[0];
            maximum_viewport_height = viewport_limits[1];
        }
        if (output_width > maximum_viewport_width || output_height > maximum_viewport_height) {
            return false;
        }

        // Immutable for this private context; do not query the driver every frame.
        if (!maximum_texture_size) gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum_texture_size);
        const int tw = texture_extent(source.width, maximum_texture_size);
        const int th = texture_extent(source.height, maximum_texture_size);
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
        gl.glCopyTexSubImage2D(
                GL_TEXTURE_2D, 0, 0, 0,
                source.x, source.y, source.width, source.height);
        // Preserve the original image when allocation/readback is unsupported by the driver.
        if (gl.glGetError() != GL_NO_ERROR) return false;

        gl.glViewport(0, 0, output_width, output_height);
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glDisable(GL_DEPTH_TEST);
        gl.glDisable(GL_BLEND);
        gl.glDisable(GL_CULL_FACE);
        gl.glClearColor(0, 0, 0, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glMatrixMode(GL_PROJECTION);
        gl.glLoadIdentity();
        gl.glOrtho(0, output_width, 0, output_height, -1, 1);
        gl.glMatrixMode(GL_MODELVIEW);
        gl.glLoadIdentity();
        gl.glEnable(GL_TEXTURE_2D);
        gl.glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        const float base_scale_x = static_cast<float>(output_width) / source.width;
        const float base_scale_y = static_cast<float>(output_height) / source.height;
        const float scale_x = base_scale_x * scene.scale_x;
        const float scale_y = base_scale_y * scene.scale_y;
        const float w = source.width * scale_x;
        const float h = source.height * scale_y;
        // Match D3D9's source-pixel offsets. API Y is positive down; OpenGL Y is up.
        const float left = (output_width - w) * .5f + scene.offset_x * scale_x;
        const float bottom = (output_height - h) * .5f - scene.offset_y * scale_y;
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
                const float xs[]{x, x + scale_x * .5f, x + w - scale_x * .5f, x + w};
                const float ys[]{bottom, bottom + scale_y * .5f,
                    bottom + h - scale_y * .5f, bottom + h};
                const float us[]{.5f / tw, .5f / tw,
                    (source.width - .5f) / tw, (source.width - .5f) / tw};
                const float vs[]{.5f / th, .5f / th,
                    (source.height - .5f) / th, (source.height - .5f) / th};
                for (int row = 0; row < 3; ++row) {
                    for (int column = 0; column < 3; ++column) {
                        rectangle(xs[column], ys[row], xs[column + 1], ys[row + 1],
                            us[column], vs[row], us[column + 1], vs[row + 1]);
                    }
                }
            } else {
                rectangle(x, bottom, x + w, bottom + h,
                    0.f, 0.f,
                    static_cast<float>(source.width) / tw,
                    static_cast<float>(source.height) / th);
            }
            gl.glEnd();
        };
        quad(left);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyLeft) quad(left - w);
        if (scene.duplicate == cfg::ScreenDuplicateMode::CopyRight) quad(left + w);
        return gl.glGetError() == GL_NO_ERROR;
    }

    std::unique_ptr<uint8_t[]> read_pixels(const GlApi &gl, int width, int height, GLenum buffer) {
        if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return nullptr;

        constexpr size_t channels = 3;
        const auto max_size = std::numeric_limits<size_t>::max();
        const auto row_size = static_cast<size_t>(width) * channels;
        if (row_size == 0 || static_cast<size_t>(height) > max_size / row_size) return nullptr;

        const auto size = row_size * static_cast<size_t>(height);
        auto pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[size]);
        if (!pixels) return nullptr;

        for (int i = 0; i < 32 && gl.glGetError() != GL_NO_ERROR; ++i) {}
        gl.glReadBuffer(buffer);
        gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (gl.glGetError() != GL_NO_ERROR) return nullptr;

        gl.glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.get());
        if (gl.glGetError() != GL_NO_ERROR) return nullptr;

        for (int y = 0; y < height / 2; ++y) {
            auto *top = pixels.get() + static_cast<size_t>(y) * row_size;
            auto *bottom = pixels.get() + static_cast<size_t>(height - 1 - y) * row_size;
            for (size_t x = 0; x < row_size; ++x) {
                std::swap(top[x], bottom[x]);
            }
        }
        return pixels;
    }
}
