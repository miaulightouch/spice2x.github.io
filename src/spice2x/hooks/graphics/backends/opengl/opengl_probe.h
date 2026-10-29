#pragma once

#ifdef SPICE_OPENGL_PROBE

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "opengl_resize.h"

namespace graphics::opengl {
    struct ProbeTiming {
        double state_ms = 0;
        double copy_ms = 0;
        double draw_ms = 0;
        double read_issue_ms = 0;
        double read_map_ms = 0;
        unsigned pbo_latency_frames = 0;
    };

    struct ProbeFrame {
        std::unique_ptr<uint8_t[]> pixels;
        int width = 0;
        int height = 0;
    };

    class ProbeRenderer {
    public:
        ProbeRenderer();
        ~ProbeRenderer();

        ProbeRenderer(const ProbeRenderer &) = delete;
        ProbeRenderer &operator=(const ProbeRenderer &) = delete;

        bool initialize(const GlApi &gl, std::function<void *(const char *)> resolver);
        const std::string &description() const;
        const std::string &error() const;

        bool draw(const ImageRect &source, int output_width, int output_height,
                const cfg::fullscreen_setting &scene, bool linear, GLenum buffer,
                ProbeTiming &timing);
        ProbeFrame read_sync(int width, int height, GLenum buffer, ProbeTiming &timing);
        bool issue_readback(int width, int height, GLenum buffer, ProbeTiming &timing);
        ProbeFrame poll_readback(ProbeTiming &timing);
        bool readback_pending() const;
        bool async_supported() const;
        void abandon_readback();
        void release();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

#endif
