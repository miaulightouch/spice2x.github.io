#!/usr/bin/env python3
"""Compile and exercise the portable OTA staging/validation engine."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="spice-ota-engine-") as directory:
    temp = Path(directory)
    executable = temp / "spice64.exe"
    command = [
        os.environ.get("CXX", "g++"), "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT),
        str(ROOT / "util/ota_update.cpp"),
        str(ROOT / "util/ota_signature.cpp"),
        str(ROOT / "api/tests/ota_update_engine_test.cpp"),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(executable)], check=True)
