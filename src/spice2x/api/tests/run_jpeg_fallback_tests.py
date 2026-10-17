#!/usr/bin/env python3
"""Compile and exercise the XP stb JPEG fallback with a real JPEG decoder."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="spice-jpeg-fallback-test-") as directory:
        binary = Path(directory) / "jpeg_fallback_test"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
            "-Wno-unused-parameter",
            "-DSPICE_XP=1", "-U", "SPICE_JPEG", "-U", "SPICE_H264",
            "-I" + str(ROOT),
            str(ROOT / "hooks/graphics/jpeg_encoder.cpp"),
            str(ROOT / "api/stream_format.cpp"),
            str(ROOT / "api/tests/jpeg_stb_fixture.cpp"),
            str(ROOT / "api/tests/jpeg_fallback_test.cpp"),
            "-ljpeg", "-o", str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], cwd=directory, check=True)


if __name__ == "__main__":
    main()
