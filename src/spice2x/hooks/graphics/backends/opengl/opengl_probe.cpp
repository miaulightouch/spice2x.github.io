#include "opengl_probe.h"

#ifdef SPICE_OPENGL_PROBE

#include <GL/glext.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <utility>

namespace graphics::opengl {
    namespace {
        using Clock = std::chrono::steady_clock;

        double elapsed_ms(const Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

        template<typename Function, typename Resolver>
        Function load_function(Resolver &resolver, const char *name) {
            return reinterpret_cast<Function>(resolver(name));
        }

        template<typename Function, typename Resolver>
        bool load_required(Function &function, Resolver &resolver, const char *name) {
            function = load_function<Function>(resolver, name);
            return function != nullptr;
        }

        bool valid_size(int width, int height, size_t &bytes) {
            if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return false;
            constexpr size_t channels = 3;
            const auto row = static_cast<size_t>(width) * channels;
            if (!row || static_cast<size_t>(height) > std::numeric_limits<size_t>::max() / row) return false;
            bytes = row * static_cast<size_t>(height);
            return true;
        }

        std::string gl_string(const GLubyte *value) {
            return value ? reinterpret_cast<const char *>(value) : "unknown";
        }

        constexpr std::array<GLenum, 17> kProbeCaps {
            GL_SCISSOR_TEST,
            GL_BLEND,
            GL_DEPTH_TEST,
            GL_STENCIL_TEST,
            GL_CULL_FACE,
            GL_RASTERIZER_DISCARD,
            GL_FRAMEBUFFER_SRGB,
            GL_ALPHA_TEST,
            GL_SAMPLE_ALPHA_TO_COVERAGE,
            GL_SAMPLE_ALPHA_TO_ONE,
            GL_SAMPLE_COVERAGE,
            GL_DITHER,
            GL_COLOR_LOGIC_OP,
            GL_POLYGON_SMOOTH,
            GL_LINE_SMOOTH,
            GL_POINT_SMOOTH,
            GL_PRIMITIVE_RESTART,
        };

        constexpr std::array<GLenum, 8> kPackParams {
            GL_PACK_ALIGNMENT,
            GL_PACK_SWAP_BYTES,
            GL_PACK_LSB_FIRST,
            GL_PACK_ROW_LENGTH,
            GL_PACK_SKIP_ROWS,
            GL_PACK_SKIP_PIXELS,
            GL_PACK_IMAGE_HEIGHT,
            GL_PACK_SKIP_IMAGES,
        };

        constexpr const char *kVertexShader = R"GLSL(#version 130
void main() {
    vec2 position;
    if (gl_VertexID == 0) position = vec2(-1.0, -1.0);
    else if (gl_VertexID == 1) position = vec2(3.0, -1.0);
    else position = vec2(-1.0, 3.0);
    gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL";

        constexpr const char *kFragmentShader = R"GLSL(#version 130
uniform sampler2D source_texture;
uniform vec2 source_size;
uniform vec2 destination_origin;
uniform vec2 destination_size;
uniform vec2 pixel_scale;
uniform int duplicate_mode;

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec2 local = pixel - destination_origin;
    bool inside = local.x >= 0.0 && local.y >= 0.0 &&
        local.x < destination_size.x && local.y < destination_size.y;

    if (!inside && duplicate_mode == 1) {
        local.x = pixel.x - (destination_origin.x - destination_size.x);
        local.y = pixel.y - destination_origin.y;
        inside = local.x >= 0.0 && local.y >= 0.0 &&
            local.x < destination_size.x && local.y < destination_size.y;
    } else if (!inside && duplicate_mode == 2) {
        local.x = pixel.x - (destination_origin.x + destination_size.x);
        local.y = pixel.y - destination_origin.y;
        inside = local.x >= 0.0 && local.y >= 0.0 &&
            local.x < destination_size.x && local.y < destination_size.y;
    }

    if (!inside) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec2 source_pixel = local / pixel_scale;
    source_pixel = clamp(source_pixel, vec2(0.5), source_size - vec2(0.5));
    gl_FragColor = texture2D(source_texture, source_pixel / source_size);
}
)GLSL";
    }

    struct ProbeRenderer::Impl {
        struct Dynamic {
            decltype(::glGetBooleanv) *get_booleanv = nullptr;
            decltype(::glIsEnabled) *is_enabled = nullptr;
            decltype(::glColorMask) *color_mask = nullptr;
            decltype(::glPolygonMode) *polygon_mode = nullptr;
            decltype(::glFlush) *flush = nullptr;

            PFNGLACTIVETEXTUREPROC active_texture = nullptr;
            PFNGLBINDSAMPLERPROC bind_sampler = nullptr;
            PFNGLUSEPROGRAMPROC use_program = nullptr;
            decltype(::glDrawArrays) *draw_arrays = nullptr;
            PFNGLGENVERTEXARRAYSPROC gen_vertex_arrays = nullptr;
            PFNGLBINDVERTEXARRAYPROC bind_vertex_array = nullptr;
            PFNGLDELETEVERTEXARRAYSPROC delete_vertex_arrays = nullptr;

            PFNGLCREATESHADERPROC create_shader = nullptr;
            PFNGLSHADERSOURCEPROC shader_source = nullptr;
            PFNGLCOMPILESHADERPROC compile_shader = nullptr;
            PFNGLGETSHADERIVPROC get_shader_iv = nullptr;
            PFNGLGETSHADERINFOLOGPROC get_shader_info_log = nullptr;
            PFNGLDELETESHADERPROC delete_shader = nullptr;
            PFNGLCREATEPROGRAMPROC create_program = nullptr;
            PFNGLATTACHSHADERPROC attach_shader = nullptr;
            PFNGLLINKPROGRAMPROC link_program = nullptr;
            PFNGLGETPROGRAMIVPROC get_program_iv = nullptr;
            PFNGLGETPROGRAMINFOLOGPROC get_program_info_log = nullptr;
            PFNGLDELETEPROGRAMPROC delete_program = nullptr;
            PFNGLGETUNIFORMLOCATIONPROC get_uniform_location = nullptr;
            PFNGLUNIFORM1IPROC uniform_1i = nullptr;
            PFNGLUNIFORM2FPROC uniform_2f = nullptr;
            decltype(::glDeleteTextures) *delete_textures = nullptr;
            decltype(::glGetTexLevelParameteriv) *get_texture_level = nullptr;

            PFNGLBINDFRAMEBUFFERPROC bind_framebuffer = nullptr;
            PFNGLDRAWBUFFERSPROC draw_buffers = nullptr;

            PFNGLGENBUFFERSPROC gen_buffers = nullptr;
            PFNGLDELETEBUFFERSPROC delete_buffers = nullptr;
            PFNGLBINDBUFFERPROC bind_buffer = nullptr;
            PFNGLBUFFERDATAPROC buffer_data = nullptr;
            PFNGLGETBUFFERPARAMETERIVPROC get_buffer_parameter = nullptr;
            PFNGLMAPBUFFERPROC map_buffer = nullptr;
            PFNGLUNMAPBUFFERPROC unmap_buffer = nullptr;
            PFNGLFENCESYNCPROC fence_sync = nullptr;
            PFNGLDELETESYNCPROC delete_sync = nullptr;
            PFNGLCLIENTWAITSYNCPROC client_wait_sync = nullptr;

            bool pbo_ready() const {
                return gen_buffers && delete_buffers && bind_buffer && buffer_data && get_buffer_parameter &&
                    map_buffer && unmap_buffer;
            }

            bool sync_ready() const {
                return fence_sync && delete_sync && client_wait_sync && flush;
            }
        } fn;

        struct State {
            GLint draw_framebuffer = 0;
            GLint read_framebuffer = 0;
            GLint read_buffer = GL_BACK;
            GLint draw_buffer_count = 0;
            std::array<GLenum, 32> draw_buffers {};
            std::array<GLenum, 32> default_draw_buffers {};
            GLenum default_read_buffer = GL_BACK;
            GLint viewport[4] {};
            GLint current_program = 0;
            GLint vertex_array = 0;
            GLint active_texture = GL_TEXTURE0;
            GLint texture_0 = 0;
            GLint sampler_0 = 0;
            GLint pixel_pack_buffer = 0;
            GLint pixel_unpack_buffer = 0;
            GLint pack_params[kPackParams.size()] {};
            GLboolean color_mask[4] {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
            GLint polygon_mode[2] {GL_FILL, GL_FILL};
            std::array<GLboolean, kProbeCaps.size()> caps {};
            std::array<GLboolean, 8> clip_distances {};
            GLboolean sample_mask = GL_FALSE;
        };

        const GlApi *gl = nullptr;
        std::function<void *(const char *)> resolver;
        std::string description_text;
        std::string error_text;
        bool initialized = false;

        GLuint program = 0;
        GLuint vertex_array = 0;
        GLuint source_texture = 0;
        int source_texture_width = 0;
        int source_texture_height = 0;
        GLint source_filter = 0;
        GLint source_sampler = -1;
        GLint source_size_uniform = -1;
        GLint destination_origin_uniform = -1;
        GLint destination_size_uniform = -1;
        GLint pixel_scale_uniform = -1;
        GLint duplicate_uniform = -1;

        GLuint pbo = 0;
        int pbo_width = 0;
        int pbo_height = 0;
        GLsync pending_sync = nullptr;
        unsigned pending_frames = 0;

        bool set_error(std::string message) {
            error_text = std::move(message);
            return false;
        }

        bool capture_state(State &state) {
            if (!gl || !fn.get_booleanv || !fn.is_enabled ||
                    !fn.color_mask || !fn.polygon_mode || !fn.active_texture ||
                    !fn.bind_sampler || !fn.bind_framebuffer || !fn.draw_buffers ||
                    !fn.bind_buffer || !fn.bind_vertex_array) {
                return false;
            }

            gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state.draw_framebuffer);
            gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state.read_framebuffer);
            gl->glGetIntegerv(GL_READ_BUFFER, &state.read_buffer);
            GLint max_draw_buffers = 0;
            gl->glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_draw_buffers);
            if (max_draw_buffers <= 0 || max_draw_buffers > 32) return false;
            state.draw_buffer_count = max_draw_buffers;
            for (GLint i = 0; i < max_draw_buffers; ++i) {
                GLint value = GL_NONE;
                gl->glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
                state.draw_buffers[static_cast<size_t>(i)] = static_cast<GLenum>(value);
            }
            // The probe temporarily uses the default framebuffer. Capture all of its
            // draw-buffer slots before doing so, including MRT selections on the game FBO.
            fn.bind_framebuffer(GL_DRAW_FRAMEBUFFER, 0);
            for (GLint i = 0; i < max_draw_buffers; ++i) {
                GLint value = GL_NONE;
                gl->glGetIntegerv(GL_DRAW_BUFFER0 + i, &value);
                state.default_draw_buffers[static_cast<size_t>(i)] = static_cast<GLenum>(value);
            }
            fn.bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
            GLint default_read_buffer = GL_NONE;
            gl->glGetIntegerv(GL_READ_BUFFER, &default_read_buffer);
            state.default_read_buffer = static_cast<GLenum>(default_read_buffer);
            fn.bind_framebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(state.draw_framebuffer));
            fn.bind_framebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(state.read_framebuffer));
            gl->glGetIntegerv(GL_VIEWPORT, state.viewport);
            gl->glGetIntegerv(GL_CURRENT_PROGRAM, &state.current_program);
            gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state.vertex_array);
            gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
            gl->glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &state.pixel_pack_buffer);
            gl->glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state.pixel_unpack_buffer);
            fn.get_booleanv(GL_COLOR_WRITEMASK, state.color_mask);
            gl->glGetIntegerv(GL_POLYGON_MODE, state.polygon_mode);

            for (size_t i = 0; i < kPackParams.size(); ++i) {
                gl->glGetIntegerv(kPackParams[i], &state.pack_params[i]);
            }
            fn.active_texture(GL_TEXTURE0);
            gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture_0);
            gl->glGetIntegerv(GL_SAMPLER_BINDING, &state.sampler_0);
            fn.active_texture(static_cast<GLenum>(state.active_texture));
            for (size_t i = 0; i < kProbeCaps.size(); ++i) {
                state.caps[i] = fn.is_enabled(kProbeCaps[i]);
            }
            for (size_t i = 0; i < state.clip_distances.size(); ++i) {
                state.clip_distances[i] = fn.is_enabled(GL_CLIP_DISTANCE0 + static_cast<GLenum>(i));
            }
            state.sample_mask = fn.is_enabled(GL_SAMPLE_MASK);
            return true;
        }

        void restore_state(const State &state) {
            for (size_t i = 0; i < kProbeCaps.size(); ++i) {
                if (state.caps[i]) gl->glEnable(kProbeCaps[i]);
                else gl->glDisable(kProbeCaps[i]);
            }
            fn.color_mask(state.color_mask[0], state.color_mask[1],
                    state.color_mask[2], state.color_mask[3]);
            fn.polygon_mode(GL_FRONT, static_cast<GLenum>(state.polygon_mode[0]));
            fn.polygon_mode(GL_BACK, static_cast<GLenum>(state.polygon_mode[1]));
            gl->glViewport(state.viewport[0], state.viewport[1],
                    state.viewport[2], state.viewport[3]);

            for (size_t i = 0; i < state.clip_distances.size(); ++i) {
                if (state.clip_distances[i]) gl->glEnable(GL_CLIP_DISTANCE0 + static_cast<GLenum>(i));
                else gl->glDisable(GL_CLIP_DISTANCE0 + static_cast<GLenum>(i));
            }
            if (state.sample_mask) gl->glEnable(GL_SAMPLE_MASK);
            else gl->glDisable(GL_SAMPLE_MASK);

            fn.bind_framebuffer(GL_DRAW_FRAMEBUFFER, 0);
            if (state.draw_buffer_count > 0) {
                gl->glDrawBuffer(state.default_draw_buffers.front());
            }
            fn.bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
            gl->glReadBuffer(state.default_read_buffer);

            fn.bind_framebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(state.draw_framebuffer));
            if (state.draw_framebuffer == 0) {
                if (state.draw_buffer_count > 0) gl->glDrawBuffer(state.draw_buffers.front());
            } else {
                fn.draw_buffers(static_cast<GLsizei>(state.draw_buffer_count), state.draw_buffers.data());
            }
            fn.bind_framebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(state.read_framebuffer));
            gl->glReadBuffer(static_cast<GLenum>(state.read_buffer));

            fn.active_texture(GL_TEXTURE0);
            gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(state.texture_0));
            fn.bind_sampler(0, static_cast<GLuint>(state.sampler_0));
            fn.active_texture(static_cast<GLenum>(state.active_texture));
            fn.bind_vertex_array(static_cast<GLuint>(state.vertex_array));
            fn.use_program(static_cast<GLuint>(state.current_program));
            fn.bind_buffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(state.pixel_pack_buffer));
            fn.bind_buffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(state.pixel_unpack_buffer));
            for (size_t i = 0; i < kPackParams.size(); ++i) {
                gl->glPixelStorei(kPackParams[i], state.pack_params[i]);
            }
        }

        struct StateGuard {
            Impl &impl;
            State state;
            bool active = false;
            double capture_ms = 0;
            double restore_ms = 0;

            explicit StateGuard(Impl &owner) : impl(owner) {
                const auto start = Clock::now();
                active = impl.capture_state(state);
                capture_ms = elapsed_ms(start);
            }

            ~StateGuard() {
                restore_now();
            }

            void restore_now() {
                if (!active) return;
                const auto start = Clock::now();
                impl.restore_state(state);
                restore_ms += elapsed_ms(start);
                active = false;
            }

            StateGuard(const StateGuard &) = delete;
            StateGuard &operator=(const StateGuard &) = delete;
        };

        bool load_functions() {
#define PROBE_LOAD(field, name) \
            if (!load_required(fn.field, resolver, name)) return false
            PROBE_LOAD(get_booleanv, "glGetBooleanv");
            PROBE_LOAD(is_enabled, "glIsEnabled");
            PROBE_LOAD(color_mask, "glColorMask");
            PROBE_LOAD(polygon_mode, "glPolygonMode");
            PROBE_LOAD(active_texture, "glActiveTexture");
            PROBE_LOAD(bind_sampler, "glBindSampler");
            PROBE_LOAD(use_program, "glUseProgram");
            PROBE_LOAD(draw_arrays, "glDrawArrays");
            PROBE_LOAD(gen_vertex_arrays, "glGenVertexArrays");
            PROBE_LOAD(bind_vertex_array, "glBindVertexArray");
            PROBE_LOAD(delete_vertex_arrays, "glDeleteVertexArrays");
            PROBE_LOAD(create_shader, "glCreateShader");
            PROBE_LOAD(shader_source, "glShaderSource");
            PROBE_LOAD(compile_shader, "glCompileShader");
            PROBE_LOAD(get_shader_iv, "glGetShaderiv");
            PROBE_LOAD(get_shader_info_log, "glGetShaderInfoLog");
            PROBE_LOAD(delete_shader, "glDeleteShader");
            PROBE_LOAD(create_program, "glCreateProgram");
            PROBE_LOAD(attach_shader, "glAttachShader");
            PROBE_LOAD(link_program, "glLinkProgram");
            PROBE_LOAD(get_program_iv, "glGetProgramiv");
            PROBE_LOAD(get_program_info_log, "glGetProgramInfoLog");
            PROBE_LOAD(delete_program, "glDeleteProgram");
            PROBE_LOAD(get_uniform_location, "glGetUniformLocation");
            PROBE_LOAD(uniform_1i, "glUniform1i");
            PROBE_LOAD(uniform_2f, "glUniform2f");
            PROBE_LOAD(bind_framebuffer, "glBindFramebuffer");
            PROBE_LOAD(draw_buffers, "glDrawBuffers");
            PROBE_LOAD(bind_buffer, "glBindBuffer");
            PROBE_LOAD(flush, "glFlush");
            PROBE_LOAD(delete_textures, "glDeleteTextures");
            PROBE_LOAD(get_texture_level, "glGetTexLevelParameteriv");
#undef PROBE_LOAD

            fn.gen_buffers = load_function<PFNGLGENBUFFERSPROC>(resolver, "glGenBuffers");
            fn.delete_buffers = load_function<PFNGLDELETEBUFFERSPROC>(resolver, "glDeleteBuffers");
            fn.buffer_data = load_function<PFNGLBUFFERDATAPROC>(resolver, "glBufferData");
            fn.get_buffer_parameter = load_function<PFNGLGETBUFFERPARAMETERIVPROC>(resolver, "glGetBufferParameteriv");
            fn.map_buffer = load_function<PFNGLMAPBUFFERPROC>(resolver, "glMapBuffer");
            fn.unmap_buffer = load_function<PFNGLUNMAPBUFFERPROC>(resolver, "glUnmapBuffer");
            fn.fence_sync = load_function<PFNGLFENCESYNCPROC>(resolver, "glFenceSync");
            fn.delete_sync = load_function<PFNGLDELETESYNCPROC>(resolver, "glDeleteSync");
            fn.client_wait_sync = load_function<PFNGLCLIENTWAITSYNCPROC>(resolver, "glClientWaitSync");
            return true;
        }

        std::string shader_log(GLuint object, bool shader) {
            GLint length = 0;
            if (shader) fn.get_shader_iv(object, GL_INFO_LOG_LENGTH, &length);
            else fn.get_program_iv(object, GL_INFO_LOG_LENGTH, &length);
            if (length <= 1) return {};
            std::string log(static_cast<size_t>(length), '\0');
            GLsizei written = 0;
            if (shader) fn.get_shader_info_log(object, length, &written, log.data());
            else fn.get_program_info_log(object, length, &written, log.data());
            if (written >= 0 && static_cast<size_t>(written) < log.size()) log.resize(written);
            return log;
        }

        GLuint compile_shader(GLenum type, const char *source) {
            const GLuint shader = fn.create_shader(type);
            if (!shader) {
                set_error("glCreateShader failed");
                return 0;
            }
            const GLchar *sources[] = {source};
            fn.shader_source(shader, 1, sources, nullptr);
            fn.compile_shader(shader);
            GLint compiled = GL_FALSE;
            fn.get_shader_iv(shader, GL_COMPILE_STATUS, &compiled);
            if (!compiled) {
                const auto log = shader_log(shader, true);
                fn.delete_shader(shader);
                set_error(log.empty() ? "GLSL shader compilation failed" : log);
                return 0;
            }
            return shader;
        }

        bool build_program() {
            const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader);
            if (!vertex) return false;
            const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, kFragmentShader);
            if (!fragment) {
                fn.delete_shader(vertex);
                return false;
            }
            program = fn.create_program();
            if (!program) {
                fn.delete_shader(vertex);
                fn.delete_shader(fragment);
                return set_error("glCreateProgram failed");
            }
            fn.attach_shader(program, vertex);
            fn.attach_shader(program, fragment);
            fn.link_program(program);
            GLint linked = GL_FALSE;
            fn.get_program_iv(program, GL_LINK_STATUS, &linked);
            fn.delete_shader(vertex);
            fn.delete_shader(fragment);
            if (!linked) {
                const auto log = shader_log(program, false);
                fn.delete_program(program);
                program = 0;
                return set_error(log.empty() ? "GLSL program link failed" : log);
            }
            source_sampler = fn.get_uniform_location(program, "source_texture");
            source_size_uniform = fn.get_uniform_location(program, "source_size");
            destination_origin_uniform = fn.get_uniform_location(program, "destination_origin");
            destination_size_uniform = fn.get_uniform_location(program, "destination_size");
            pixel_scale_uniform = fn.get_uniform_location(program, "pixel_scale");
            duplicate_uniform = fn.get_uniform_location(program, "duplicate_mode");
            if (source_sampler < 0 || source_size_uniform < 0 || destination_origin_uniform < 0 ||
                    destination_size_uniform < 0 || pixel_scale_uniform < 0 || duplicate_uniform < 0) {
                fn.delete_program(program);
                program = 0;
                return set_error("GLSL uniform lookup failed");
            }
            return true;
        }

        bool ensure_texture(int width, int height, bool linear) {
            if (!source_texture) gl->glGenTextures(1, &source_texture);
            if (!source_texture) return set_error("source texture allocation failed");
            gl->glBindTexture(GL_TEXTURE_2D, source_texture);
            if (source_texture_width != width || source_texture_height != height) {
                gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                        GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                GLint allocated_width = 0, allocated_height = 0;
                fn.get_texture_level(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &allocated_width);
                fn.get_texture_level(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &allocated_height);
                if (allocated_width != width || allocated_height != height) {
                    return set_error("source texture storage allocation failed");
                }
                source_texture_width = width;
                source_texture_height = height;
                source_filter = 0;
            }
            const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
            if (source_filter != filter) {
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                source_filter = filter;
            }
            return true;
        }

        bool ensure_pbo(int width, int height, size_t bytes) {
            if (!fn.pbo_ready()) return set_error("pixel-buffer entrypoints unavailable");
            if (!pbo) fn.gen_buffers(1, &pbo);
            if (!pbo) return set_error("pixel-buffer allocation failed");
            fn.bind_buffer(GL_PIXEL_PACK_BUFFER, pbo);
            if (pbo_width != width || pbo_height != height) {
                fn.buffer_data(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_READ);
                GLint allocated_bytes = 0;
                fn.get_buffer_parameter(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &allocated_bytes);
                if (allocated_bytes < 0 || static_cast<size_t>(allocated_bytes) != bytes) {
                    return set_error("pixel-buffer storage allocation failed");
                }
                pbo_width = width;
                pbo_height = height;
            }
            return true;
        }

        bool force_default_framebuffer(GLenum buffer) {
            fn.bind_framebuffer(GL_DRAW_FRAMEBUFFER, 0);
            fn.bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
            gl->glDrawBuffer(buffer);
            gl->glReadBuffer(buffer);
            return true;
        }

        bool validate_draw(const ImageRect &source, int width, int height,
                const cfg::fullscreen_setting &scene) {
            return width > 0 && height > 0 && source.x >= 0 && source.y >= 0 &&
                source.width > 0 && source.height > 0 && source.x <= width && source.y <= height &&
                source.width <= width - source.x && source.height <= height - source.y &&
                std::isfinite(scene.scale_x) && std::isfinite(scene.scale_y) &&
                scene.scale_x >= .6f && scene.scale_x <= 2.5f &&
                scene.scale_y >= .6f && scene.scale_y <= 2.5f;
        }

        void configure_clean_draw_state() {
            for (const auto cap : kProbeCaps) gl->glDisable(cap);
            for (size_t i = 0; i < 8; ++i) {
                gl->glDisable(GL_CLIP_DISTANCE0 + static_cast<GLenum>(i));
            }
            gl->glDisable(GL_SAMPLE_MASK);
            fn.color_mask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            fn.polygon_mode(GL_FRONT_AND_BACK, GL_FILL);
        }

        void flip_rows(uint8_t *pixels, int width, int height) {
            const size_t row = static_cast<size_t>(width) * 3;
            for (int y = 0; y < height / 2; ++y) {
                auto *top = pixels + static_cast<size_t>(y) * row;
                auto *bottom = pixels + static_cast<size_t>(height - 1 - y) * row;
                for (size_t x = 0; x < row; ++x) std::swap(top[x], bottom[x]);
            }
        }
    };

    ProbeRenderer::ProbeRenderer() : impl_(std::make_unique<Impl>()) {}

    ProbeRenderer::~ProbeRenderer() = default;

    bool ProbeRenderer::initialize(const GlApi &gl, std::function<void *(const char *)> resolver) {
        auto &impl = *impl_;
        if (impl.initialized) return true;
        impl.gl = &gl;
        impl.resolver = std::move(resolver);
        impl.error_text.clear();
        if (!gl.glGetString || !gl.glGetIntegerv || !gl.glGetError ||
                !gl.glGenTextures || !gl.glBindTexture || !gl.glTexImage2D ||
                !gl.glTexParameteri || !gl.glCopyTexSubImage2D || !gl.glReadPixels ||
                !gl.glPixelStorei || !gl.glReadBuffer || !gl.glDrawBuffer || !gl.glViewport ||
                !gl.glEnable || !gl.glDisable) {
            return impl.set_error("legacy OpenGL entrypoints unavailable");
        }
        if (!impl.load_functions()) return impl.set_error("shader entrypoints unavailable");

        Impl::StateGuard guard(impl);
        if (!guard.active) return impl.set_error("unable to snapshot OpenGL state");
        if (!impl.build_program()) return false;
        impl.fn.gen_vertex_arrays(1, &impl.vertex_array);
        if (!impl.vertex_array) {
            impl.fn.delete_program(impl.program);
            impl.program = 0;
            return impl.set_error("vertex-array allocation failed");
        }
        const bool pbo = impl.fn.pbo_ready();
        const bool sync = impl.fn.sync_ready();
        std::ostringstream description;
        description << "GLSL 130 shader=" << (impl.program != 0)
            << " vao=" << (impl.vertex_array != 0)
            << " fbo=" << (impl.fn.bind_framebuffer != nullptr)
            << " pbo=" << pbo << " sync=" << sync << " flush=" << (impl.fn.flush != nullptr)
            << " version=" << gl_string(gl.glGetString(GL_VERSION))
            << " renderer=" << gl_string(gl.glGetString(GL_RENDERER));
        impl.description_text = description.str();
        impl.initialized = true;
        return true;
    }

    const std::string &ProbeRenderer::description() const {
        return impl_->description_text;
    }

    const std::string &ProbeRenderer::error() const {
        return impl_->error_text;
    }

    bool ProbeRenderer::async_supported() const {
        return impl_->initialized && impl_->fn.pbo_ready() && impl_->fn.sync_ready();
    }

    bool ProbeRenderer::draw(const ImageRect &source, int output_width, int output_height,
            const cfg::fullscreen_setting &scene, bool linear, GLenum buffer, ProbeTiming &timing) {
        auto &impl = *impl_;
        if (!impl.initialized) return impl.set_error("probe renderer is not initialized");
        if (!impl.validate_draw(source, output_width, output_height, scene)) {
            return impl.set_error("invalid probe draw dimensions or scene");
        }
        Impl::StateGuard guard(impl);
        if (!guard.active) return impl.set_error("unable to snapshot OpenGL state");
        impl.force_default_framebuffer(buffer);
        impl.fn.active_texture(GL_TEXTURE0);
        impl.fn.bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);
        if (!impl.ensure_texture(source.width, source.height, linear)) return false;
        const auto copy_start = Clock::now();
        impl.gl->glReadBuffer(buffer);
        impl.gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                source.x, source.y, source.width, source.height);
        timing.copy_ms += elapsed_ms(copy_start);

        const float pixel_scale_x = static_cast<float>(output_width) / source.width * scene.scale_x;
        const float pixel_scale_y = static_cast<float>(output_height) / source.height * scene.scale_y;
        const float destination_width = source.width * pixel_scale_x;
        const float destination_height = source.height * pixel_scale_y;
        const float destination_left = (output_width - destination_width) * .5f +
            scene.offset_x * pixel_scale_x;
        const float destination_bottom = (output_height - destination_height) * .5f -
            scene.offset_y * pixel_scale_y;

        const auto draw_start = Clock::now();
        impl.gl->glDrawBuffer(buffer);
        impl.gl->glViewport(0, 0, output_width, output_height);
        impl.configure_clean_draw_state();
        impl.fn.use_program(impl.program);
        impl.fn.active_texture(GL_TEXTURE0);
        impl.fn.bind_sampler(0, 0);
        impl.gl->glBindTexture(GL_TEXTURE_2D, impl.source_texture);
        impl.fn.uniform_1i(impl.source_sampler, 0);
        impl.fn.uniform_2f(impl.source_size_uniform,
                static_cast<float>(source.width), static_cast<float>(source.height));
        impl.fn.uniform_2f(impl.destination_origin_uniform, destination_left, destination_bottom);
        impl.fn.uniform_2f(impl.destination_size_uniform, destination_width, destination_height);
        impl.fn.uniform_2f(impl.pixel_scale_uniform, pixel_scale_x, pixel_scale_y);
        impl.fn.uniform_1i(impl.duplicate_uniform, static_cast<GLint>(scene.duplicate));
        impl.fn.bind_vertex_array(impl.vertex_array);
        impl.fn.draw_arrays(GL_TRIANGLES, 0, 3);
        timing.draw_ms += elapsed_ms(draw_start);
        timing.state_ms += guard.capture_ms;
        guard.restore_now();
        timing.state_ms += guard.restore_ms;
        return true;
    }

    ProbeFrame ProbeRenderer::read_sync(int width, int height, GLenum buffer, ProbeTiming &timing) {
        auto &impl = *impl_;
        ProbeFrame frame;
        size_t bytes = 0;
        if (!impl.initialized || !valid_size(width, height, bytes)) {
            impl.set_error("invalid synchronous readback dimensions or renderer state");
            return frame;
        }
        frame.pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bytes]);
        if (!frame.pixels) {
            impl.set_error("synchronous readback allocation failed");
            return {};
        }
        Impl::StateGuard guard(impl);
        if (!guard.active) {
            impl.set_error("unable to snapshot OpenGL state");
            return {};
        }
        const auto start = Clock::now();
        impl.force_default_framebuffer(buffer);
        impl.fn.bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
        impl.gl->glReadBuffer(buffer);
        impl.gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        impl.gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        impl.gl->glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
        impl.gl->glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        impl.gl->glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
        impl.gl->glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, frame.pixels.get());
        timing.read_issue_ms += elapsed_ms(start);
        impl.flip_rows(frame.pixels.get(), width, height);
        frame.width = width;
        frame.height = height;
        timing.state_ms += guard.capture_ms;
        guard.restore_now();
        timing.state_ms += guard.restore_ms;
        return frame;
    }

    bool ProbeRenderer::issue_readback(int width, int height, GLenum buffer, ProbeTiming &timing) {
        auto &impl = *impl_;
        size_t bytes = 0;
        if (!impl.initialized || !valid_size(width, height, bytes)) {
            return impl.set_error("invalid asynchronous readback dimensions or renderer state");
        }
        if (impl.pending_sync) return impl.set_error("asynchronous readback already pending");
        if (!impl.fn.pbo_ready() || !impl.fn.sync_ready()) {
            return impl.set_error("asynchronous readback entrypoints unavailable");
        }
        Impl::StateGuard guard(impl);
        if (!guard.active) return impl.set_error("unable to snapshot OpenGL state");
        const auto start = Clock::now();
        impl.force_default_framebuffer(buffer);
        if (!impl.ensure_pbo(width, height, bytes)) return false;
        impl.gl->glReadBuffer(buffer);
        impl.gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        impl.gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        impl.gl->glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        impl.gl->glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
        impl.gl->glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        impl.gl->glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
        impl.gl->glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        impl.pending_sync = impl.fn.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!impl.pending_sync) return impl.set_error("glFenceSync failed");
        impl.fn.flush();
        impl.pending_frames = 0;
        timing.read_issue_ms += elapsed_ms(start);
        timing.state_ms += guard.capture_ms;
        guard.restore_now();
        timing.state_ms += guard.restore_ms;
        return true;
    }

    ProbeFrame ProbeRenderer::poll_readback(ProbeTiming &timing) {
        auto &impl = *impl_;
        timing.pbo_latency_frames = 0;
        if (!impl.pending_sync) return {};
        ++impl.pending_frames;
        Impl::StateGuard guard(impl);
        if (!guard.active) {
            impl.set_error("unable to snapshot OpenGL state");
            return {};
        }
        const GLenum status = impl.fn.client_wait_sync(impl.pending_sync, 0, 0);
        if (status == GL_TIMEOUT_EXPIRED) {
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            impl.fn.delete_sync(impl.pending_sync);
            impl.pending_sync = nullptr;
            impl.set_error("glClientWaitSync failed");
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        const auto map_start = Clock::now();
        impl.fn.bind_buffer(GL_PIXEL_PACK_BUFFER, impl.pbo);
        auto *mapped = static_cast<uint8_t *>(impl.fn.map_buffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
        if (!mapped) {
            impl.fn.delete_sync(impl.pending_sync);
            impl.pending_sync = nullptr;
            impl.set_error("pixel-buffer map failed");
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        size_t bytes = 0;
        if (!valid_size(impl.pbo_width, impl.pbo_height, bytes)) {
            impl.fn.unmap_buffer(GL_PIXEL_PACK_BUFFER);
            impl.fn.delete_sync(impl.pending_sync);
            impl.pending_sync = nullptr;
            impl.set_error("invalid pending pixel-buffer dimensions");
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        ProbeFrame frame;
        frame.pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bytes]);
        if (!frame.pixels) {
            impl.fn.unmap_buffer(GL_PIXEL_PACK_BUFFER);
            impl.fn.delete_sync(impl.pending_sync);
            impl.pending_sync = nullptr;
            impl.set_error("pixel-buffer map allocation failed");
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        std::memcpy(frame.pixels.get(), mapped, bytes);
        const bool valid = impl.fn.unmap_buffer(GL_PIXEL_PACK_BUFFER) == GL_TRUE;
        impl.fn.delete_sync(impl.pending_sync);
        impl.pending_sync = nullptr;
        if (!valid) {
            impl.set_error("pixel-buffer contents invalidated during readback");
            timing.read_map_ms += elapsed_ms(map_start);
            timing.state_ms += guard.capture_ms;
            guard.restore_now();
            timing.state_ms += guard.restore_ms;
            return {};
        }
        impl.flip_rows(frame.pixels.get(), impl.pbo_width, impl.pbo_height);
        frame.width = impl.pbo_width;
        frame.height = impl.pbo_height;
        timing.read_map_ms += elapsed_ms(map_start);
        timing.pbo_latency_frames = impl.pending_frames;
        impl.pending_frames = 0;
        timing.state_ms += guard.capture_ms;
        guard.restore_now();
        timing.state_ms += guard.restore_ms;
        return frame;
    }

    bool ProbeRenderer::readback_pending() const {
        return impl_->pending_sync != nullptr;
    }

    void ProbeRenderer::abandon_readback() {
        auto &impl = *impl_;
        if (impl.pending_sync && impl.fn.delete_sync) {
            impl.fn.delete_sync(impl.pending_sync);
        }
        impl.pending_sync = nullptr;
        impl.pending_frames = 0;
    }

    void ProbeRenderer::release() {
        auto &impl = *impl_;
        if (!impl.gl) return;
        abandon_readback();
        if (impl.pbo && impl.fn.delete_buffers) impl.fn.delete_buffers(1, &impl.pbo);
        if (impl.source_texture && impl.fn.delete_textures) {
            impl.fn.delete_textures(1, &impl.source_texture);
        }
        if (impl.vertex_array && impl.fn.delete_vertex_arrays) {
            impl.fn.delete_vertex_arrays(1, &impl.vertex_array);
        }
        if (impl.program && impl.fn.delete_program) impl.fn.delete_program(impl.program);
        impl.pbo = 0;
        impl.source_texture = 0;
        impl.vertex_array = 0;
        impl.program = 0;
        impl.source_texture_width = 0;
        impl.source_texture_height = 0;
        impl.pbo_width = 0;
        impl.pbo_height = 0;
        impl.initialized = false;
    }
}

#endif
