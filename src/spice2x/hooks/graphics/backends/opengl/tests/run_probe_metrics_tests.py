#!/usr/bin/env python3
"""Check diagnostic percentiles, bounded storage and invalid timer samples."""
from pathlib import Path
import subprocess
import tempfile

backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="spice-probe-metrics-") as directory:
    root = Path(directory)
    source = root / "metrics.cpp"
    source.write_text(r'''
#include "opengl_probe_metrics.h"
#include <cassert>
#include <limits>
int main() {
    graphics::opengl::ProbeMetric metric;
    assert(metric.summarize().count == 0);
    metric.add(-1); metric.add(std::numeric_limits<double>::quiet_NaN());
    metric.add(std::numeric_limits<double>::infinity());
    assert(metric.summarize().count == 0);
    for (int value = 1; value <= 100; ++value) metric.add(value);
    auto summary = metric.summarize();
    assert(summary.count == 100 && summary.samples == 100);
    assert(summary.mean == 50.5 && summary.p50 == 50);
    assert(summary.p95 == 95 && summary.p99 == 99 && summary.maximum == 100);
    for (size_t i = 0; i < metric.capacity; ++i) metric.add(0);
    summary = metric.summarize();
    assert(summary.count == metric.capacity + 100 && summary.samples == metric.capacity);
    assert(summary.p99 == 0 && summary.maximum == 100 && summary.mean > 0);
}
''')
    binary = root / "metrics"
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(backend),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("OpenGL probe metrics: passed")
