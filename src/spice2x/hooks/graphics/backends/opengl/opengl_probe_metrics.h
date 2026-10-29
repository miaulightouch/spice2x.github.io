#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace graphics::opengl {
    struct ProbeMetric {
        static constexpr size_t capacity = 4096;
        std::array<double, capacity> values {};
        uint64_t count = 0;
        double total = 0;
        double maximum = 0;

        void add(double value) {
            if (!std::isfinite(value) || value < 0) return;
            values[count % capacity] = value;
            ++count;
            total += value;
            maximum = std::max(maximum, value);
        }

        struct Summary {
            uint64_t count;
            size_t samples;
            double mean, p50, p95, p99, maximum;
        };

        Summary summarize() const {
            const auto size = static_cast<size_t>(std::min<uint64_t>(count, capacity));
            if (!size) return {};
            std::vector<double> sorted(values.begin(), values.begin() + size);
            std::sort(sorted.begin(), sorted.end());
            const auto percentile = [&](double fraction) {
                return sorted[static_cast<size_t>(std::ceil(size * fraction)) - 1];
            };
            return {count, size, total / count, percentile(.5), percentile(.95), percentile(.99), maximum};
        }
    };
}
