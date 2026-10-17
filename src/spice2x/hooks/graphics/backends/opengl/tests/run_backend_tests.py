#!/usr/bin/env python3
"""Run the portable OpenGL hook state-machine tests without Win32 or a GPU."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[6]
TEST = Path(__file__).with_name("backend_test.cpp")

with tempfile.TemporaryDirectory(prefix="spice-opengl-backend-test-") as directory:
    binary = Path(directory) / "backend_test"
    subprocess.run([
        "g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-DSPICE_OPENGL_TEST=1", "-I" + str(ROOT), str(TEST), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
