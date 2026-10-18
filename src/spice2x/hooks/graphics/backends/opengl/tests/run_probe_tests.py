#!/usr/bin/env python3
"""Exercise the optional same-context shader and PBO probe on Mesa EGL."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / "hooks/graphics/backends/opengl/opengl_probe.cpp"
RESIZE_SOURCE = ROOT / "hooks/graphics/backends/opengl/opengl_resize.cpp"

TEST = r'''
#define SPICE_OPENGL_PROBE 1
#include "hooks/graphics/backends/opengl/opengl_probe.h"

#include <EGL/egl.h>
#include <GL/gl.h>

#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
    using GetPlatformDisplay = EGLDisplay (*)(EGLenum, void *, const EGLint *);
    using UnmapBuffer = GLboolean (*)(GLenum);
    UnmapBuffer real_unmap_buffer = nullptr;

    void check(bool value, const char *what) {
        if (!value) {
            std::cerr << what << "\n";
            std::exit(1);
        }
    }

    GLboolean forced_false_unmap(GLenum target) {
        if (real_unmap_buffer) real_unmap_buffer(target);
        return GL_FALSE;
    }

    bool poll_until_ready(graphics::opengl::ProbeRenderer &renderer,
            graphics::opengl::ProbeFrame &frame, graphics::opengl::ProbeTiming &timing) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            frame = renderer.poll_readback(timing);
            if (frame.pixels) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    std::vector<unsigned char> read_frame(const graphics::opengl::GlApi &gl,
            int width, int height, GLenum buffer) {
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
        gl.glReadBuffer(buffer);
        gl.glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < height / 2; ++y) {
            auto *top = pixels.data() + static_cast<size_t>(y) * width * 3;
            auto *bottom = pixels.data() + static_cast<size_t>(height - y - 1) * width * 3;
            std::swap_ranges(top, top + width * 3, bottom);
        }
        return pixels;
    }

    void paint_pattern(const graphics::opengl::GlApi &gl, int width, int height, GLenum buffer) {
        gl.glDrawBuffer(buffer);
        gl.glViewport(0, 0, width, height);
        gl.glDisable(GL_SCISSOR_TEST);
        gl.glDisable(GL_BLEND);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        gl.glClearColor(0, 0, 0, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, width, height / 2);
        gl.glClearColor(0, 0, 1, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        glScissor(0, height / 2, width, height - height / 2);
        gl.glClearColor(1, 0, 0, 1);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glDisable(GL_SCISSOR_TEST);
    }

    void compare_case(const char *name, graphics::opengl::ProbeRenderer &renderer,
            graphics::opengl::ImageResize &legacy, const graphics::opengl::GlApi &gl,
            const graphics::opengl::ImageRect &source, int canvas_width, int canvas_height,
            int output_width, int output_height, const cfg::fullscreen_setting &scene,
            bool linear, GLenum buffer, int tolerance) {
        paint_pattern(gl, canvas_width, canvas_height, buffer);
        check(legacy.draw(gl, source, output_width, output_height, scene, linear, buffer),
            "legacy pixel reference draw");
        const auto expected = read_frame(gl, output_width, output_height, buffer);
        paint_pattern(gl, canvas_width, canvas_height, buffer);
        graphics::opengl::ProbeTiming timing;
        check(renderer.draw(source, output_width, output_height, scene, linear, buffer, timing),
            renderer.error().c_str());
        const auto actual = read_frame(gl, output_width, output_height, buffer);
        int max_delta = 0;
        size_t changed = 0;
        for (size_t i = 0; i < expected.size(); ++i) {
            const int delta = std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i]));
            max_delta = std::max(max_delta, delta);
            changed += delta != 0;
        }
        std::cerr << name << ": max_delta=" << max_delta << " changed=" << changed << "\n";
        if (changed) {
            std::cerr << " first expected=" << static_cast<int>(expected[0]) << ","
                << static_cast<int>(expected[1]) << "," << static_cast<int>(expected[2])
                << " actual=" << static_cast<int>(actual[0]) << ","
                << static_cast<int>(actual[1]) << "," << static_cast<int>(actual[2]) << "\n";
            for (size_t i = 0; i < expected.size(); ++i) {
                if (expected[i] != actual[i]) {
                    const size_t pixel = i / 3;
                    std::cerr << " first diff pixel=" << (pixel % static_cast<size_t>(output_width))
                        << "," << (pixel / static_cast<size_t>(output_width))
                        << " expected=" << static_cast<int>(expected[i])
                        << " actual=" << static_cast<int>(actual[i]) << "\n";
                    break;
                }
            }
        }
        check(max_delta <= tolerance, name);
    }
}

int main() {
    const auto platform_display = reinterpret_cast<GetPlatformDisplay>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    const auto display = platform_display ?
        platform_display(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr) :
        eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(display != EGL_NO_DISPLAY, "no EGL display");
    check(eglInitialize(display, nullptr, nullptr) == EGL_TRUE, "eglInitialize");
    check(eglBindAPI(EGL_OPENGL_API) == EGL_TRUE, "eglBindAPI");

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    check(eglChooseConfig(display, config_attributes, &config, 1, &count) == EGL_TRUE && count,
        "eglChooseConfig");
    const EGLint surface_attributes[] = {EGL_WIDTH, 1080, EGL_HEIGHT, 1920, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, surface_attributes);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    check(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT, "EGL object creation");
    check(eglMakeCurrent(display, surface, surface, context) == EGL_TRUE, "eglMakeCurrent");

    const auto lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    check(lib != nullptr, "libGL");
    graphics::opengl::GlApi gl;
    check(gl.load([&](const char *name) { return dlsym(lib, name); }), "legacy GL entrypoints");
    auto resolve = [&](const char *name) -> void * {
        if (const auto proc = eglGetProcAddress(name)) return reinterpret_cast<void *>(proc);
        return dlsym(lib, name);
    };
    real_unmap_buffer = reinterpret_cast<UnmapBuffer>(resolve("glUnmapBuffer"));
    check(real_unmap_buffer != nullptr, "glUnmapBuffer resolver");
    graphics::opengl::ProbeRenderer renderer;
    check(renderer.initialize(gl, resolve), renderer.error().c_str());
    const GLenum initialize_error = gl.glGetError();
    if (initialize_error != GL_NO_ERROR) {
        std::cerr << "probe initialization GL error: 0x" << std::hex << initialize_error << "\n";
        return 1;
    }

    GLint double_buffer = GL_FALSE;
    gl.glGetIntegerv(GL_DOUBLEBUFFER, &double_buffer);
    const GLenum buffer = double_buffer ? GL_BACK : GL_FRONT;

    using GenBuffers = void (*)(GLsizei, GLuint *);
    using BindBuffer = void (*)(GLenum, GLuint);
    using BufferData = void (*)(GLenum, GLsizeiptr, const void *, GLenum);
    using GenSamplers = void (*)(GLsizei, GLuint *);
    using BindSampler = void (*)(GLuint, GLuint);
    const auto gen_buffers = reinterpret_cast<GenBuffers>(eglGetProcAddress("glGenBuffers"));
    const auto bind_buffer = reinterpret_cast<BindBuffer>(eglGetProcAddress("glBindBuffer"));
    const auto buffer_data = reinterpret_cast<BufferData>(eglGetProcAddress("glBufferData"));
    const auto gen_samplers = reinterpret_cast<GenSamplers>(eglGetProcAddress("glGenSamplers"));
    const auto bind_sampler = reinterpret_cast<BindSampler>(eglGetProcAddress("glBindSampler"));
    check(gen_buffers && bind_buffer && buffer_data && gen_samplers && bind_sampler,
        "probe state test entrypoints");
    GLuint state_unpack_buffer = 0;
    GLuint state_sampler = 0;
    gen_buffers(1, &state_unpack_buffer);
    bind_buffer(GL_PIXEL_UNPACK_BUFFER, state_unpack_buffer);
    buffer_data(GL_PIXEL_UNPACK_BUFFER, 16, nullptr, GL_STREAM_DRAW);
    gen_samplers(1, &state_sampler);
    bind_sampler(0, state_sampler);
    gl.glEnable(GL_CLIP_DISTANCE0);
    gl.glEnable(GL_SAMPLE_MASK);

    gl.glViewport(3, 4, 51, 49);
    gl.glEnable(GL_SCISSOR_TEST);
    glScissor(2, 3, 20, 21);
    gl.glEnable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    gl.glDisable(GL_SCISSOR_TEST);
    gl.glClearColor(0, 0, 1, 1);
    gl.glClear(GL_COLOR_BUFFER_BIT);
    gl.glEnable(GL_SCISSOR_TEST);
    glScissor(8, 8, 32, 32);
    gl.glClearColor(1, 0, 0, 1);
    gl.glClear(GL_COLOR_BUFFER_BIT);
    const GLenum setup_error = gl.glGetError();
    if (setup_error != GL_NO_ERROR) {
        std::cerr << "test setup GL error: 0x" << std::hex << setup_error << "\n";
        return 1;
    }
    gl.glEnable(0xFFFFFFFFu); // Preserve a pre-existing game GL error.

    cfg::fullscreen_setting scene;
    scene.scale_x = .938f;
    scene.scale_y = .938f;
    scene.offset_x = 3;
    scene.offset_y = -2;
    scene.duplicate = cfg::ScreenDuplicateMode::CopyRight;
    graphics::opengl::ProbeTiming timing;
    check(renderer.draw({0, 0, 64, 64}, 64, 64, scene, true, buffer, timing),
        renderer.error().c_str());

    GLint viewport[4] {};
    GLint polygon[2] {};
    gl.glGetIntegerv(GL_VIEWPORT, viewport);
    gl.glGetIntegerv(GL_POLYGON_MODE, polygon);
    check(viewport[0] == 3 && viewport[1] == 4 && viewport[2] == 51 && viewport[3] == 49,
        "viewport not restored");
    check(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE, "scissor enable not restored");
    check(glIsEnabled(GL_BLEND) == GL_TRUE, "blend enable not restored");
    GLint restored_unpack_buffer = 0;
    GLint restored_sampler = 0;
    gl.glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &restored_unpack_buffer);
    gl.glGetIntegerv(GL_SAMPLER_BINDING, &restored_sampler);
    check(restored_unpack_buffer == static_cast<GLint>(state_unpack_buffer),
        "pixel unpack binding not restored");
    check(restored_sampler == static_cast<GLint>(state_sampler), "sampler binding not restored");
    check(glIsEnabled(GL_CLIP_DISTANCE0) == GL_TRUE, "clip distance state not restored");
    check(glIsEnabled(GL_SAMPLE_MASK) == GL_TRUE, "sample mask state not restored");
    check(polygon[0] == GL_LINE && polygon[1] == GL_LINE, "polygon mode not restored");
    const GLenum draw_error = gl.glGetError();
    const GLenum draw_error_tail = gl.glGetError();
    if (draw_error != GL_INVALID_ENUM || draw_error_tail != GL_NO_ERROR) {
        std::cerr << "game GL error state was consumed or changed: 0x" << std::hex
            << draw_error << ", 0x" << draw_error_tail << "\n";
        return 1;
    }

    bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);
    bind_sampler(0, 0);

    graphics::opengl::ImageResize legacy;
    cfg::fullscreen_setting identity;
    compare_case("identity nearest", renderer, legacy, gl, {0, 0, 64, 64}, 64, 64,
        64, 64, identity, false, buffer, 0);
    compare_case("identity linear", renderer, legacy, gl, {0, 0, 64, 64}, 64, 64,
        64, 64, identity, true, buffer, 1);
    cfg::fullscreen_setting fractional;
    fractional.scale_x = .938f;
    fractional.scale_y = .938f;
    fractional.offset_x = 3;
    fractional.offset_y = -2;
    compare_case("fractional nearest with offsets", renderer, legacy, gl, {0, 0, 64, 64},
        64, 64, 64, 64, fractional, false, buffer, 0);
    compare_case("fractional linear with offsets", renderer, legacy, gl, {0, 0, 64, 64},
        64, 64, 64, 64, fractional, true, buffer, 1);
    cfg::fullscreen_setting duplicate_left = fractional;
    duplicate_left.duplicate = cfg::ScreenDuplicateMode::CopyLeft;
    compare_case("duplicate left", renderer, legacy, gl, {0, 0, 64, 64}, 64, 64,
        64, 64, duplicate_left, false, buffer, 0);
    cfg::fullscreen_setting duplicate_right = fractional;
    duplicate_right.duplicate = cfg::ScreenDuplicateMode::CopyRight;
    compare_case("duplicate right", renderer, legacy, gl, {0, 0, 64, 64}, 64, 64,
        64, 64, duplicate_right, false, buffer, 0);
    cfg::fullscreen_setting upper_menu;
    compare_case("upper menu full canvas nearest", renderer, legacy, gl, {0, 0, 32, 56},
        64, 64, 48, 64, upper_menu, false, buffer, 0);
    compare_case("upper menu full canvas linear", renderer, legacy, gl, {0, 0, 32, 56},
        64, 64, 48, 64, upper_menu, true, buffer, 1);

    cfg::fullscreen_setting cabinet_scene;
    cabinet_scene.scale_x = .938f;
    cabinet_scene.scale_y = .938f;
    cabinet_scene.offset_x = 7;
    cabinet_scene.offset_y = -5;
    compare_case("cabinet 768x1360 nearest", renderer, legacy, gl, {0, 0, 768, 1360},
        768, 1360, 1080, 1920, cabinet_scene, false, buffer, 0);
    compare_case("cabinet 768x1360 linear", renderer, legacy, gl, {0, 0, 768, 1360},
        768, 1360, 1080, 1920, cabinet_scene, true, buffer, 1);

    auto frame = renderer.read_sync(64, 64, buffer, timing);
    check(frame.pixels && frame.width == 64 && frame.height == 64, "sync readback");

    gl.glEnable(0xFFFFFFFFu);
    check(renderer.issue_readback(64, 64, buffer, timing), renderer.error().c_str());
    check(poll_until_ready(renderer, frame, timing), "PBO readback did not become ready");
    check(frame.width == 64 && frame.height == 64, "PBO dimensions");
    check(gl.glGetError() == GL_INVALID_ENUM && gl.glGetError() == GL_NO_ERROR,
        "PBO path changed game GL error state");

    graphics::opengl::ProbeRenderer bad_unmap_renderer;
    auto bad_resolve = [&](const char *name) -> void * {
        if (std::strcmp(name, "glUnmapBuffer") == 0) {
            return reinterpret_cast<void *>(&forced_false_unmap);
        }
        return resolve(name);
    };
    check(bad_unmap_renderer.initialize(gl, bad_resolve), bad_unmap_renderer.error().c_str());
    graphics::opengl::ProbeTiming bad_timing;
    check(bad_unmap_renderer.issue_readback(64, 64, buffer, bad_timing),
        bad_unmap_renderer.error().c_str());
    graphics::opengl::ProbeFrame rejected;
    const auto bad_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (bad_unmap_renderer.readback_pending() &&
            std::chrono::steady_clock::now() < bad_deadline) {
        rejected = bad_unmap_renderer.poll_readback(bad_timing);
        if (bad_unmap_renderer.readback_pending()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    check(!rejected.pixels && !bad_unmap_renderer.readback_pending() &&
            !bad_unmap_renderer.error().empty(),
        "GL_FALSE unmap was not rejected");
    bad_unmap_renderer.release();

    renderer.release();
    check(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE,
        "eglMakeCurrent cleanup");
    check(eglDestroyContext(display, context) == EGL_TRUE, "eglDestroyContext");
    check(eglDestroySurface(display, surface) == EGL_TRUE, "eglDestroySurface");
    eglTerminate(display);
    dlclose(lib);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="spice-opengl-probe-test-") as directory:
    temp = Path(directory)
    (temp / "windows.h").write_text("#pragma once\n")
    test = temp / "probe_test.cpp"
    test.write_text(TEST)
    binary = temp / "probe_test"
    subprocess.run([
        "g++", "-std=c++20", "-DSPICE_OPENGL_PROBE", "-Wall", "-Wextra",
        "-Werror", "-Wno-deprecated-declarations", "-I" + str(temp),
        "-I" + str(ROOT), str(SOURCE), str(RESIZE_SOURCE), str(test), "-ldl", "-lEGL", "-lGL",
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
