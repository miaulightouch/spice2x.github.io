#!/usr/bin/env python3
"""Exercise the production OTA API with inert platform and engine adapters."""

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ADAPTERS = {
    "api/module.cpp": ["util/logging.h"],
    "api/request.cpp": ["../util/logging.h"],
    "api/response.cpp": ["util/logging.h"],
    "api/modules/update.cpp": ["build/defs.h", "util/logging.h", "util/ota_update.h"],
}

with tempfile.TemporaryDirectory(prefix="spice-update-api-") as directory:
    temp = Path(directory)
    sources = []
    for name, headers in ADAPTERS.items():
        content = (ROOT / name).read_text()
        for header in headers:
            original = '#include "' + header + '"'
            assert original in content
            content = content.replace(original, '#include "update_portable.h"')
        for header in ("winsock2.h", "windows.h", "wincrypt.h"):
            content = content.replace('#include <' + header + '>', '#include "update_portable.h"')
        source = temp / name.replace("/", "_")
        source.write_text(content)
        sources.append(str(source))
    command = [os.environ.get("CXX", "g++"), "-std=c++20", "-pthread", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-Wno-class-memaccess", "-Wno-deprecated-declarations",
        "-DRAPIDJSON_HAS_STDSTRING", "-I" + str(ROOT), "-I" + str(ROOT / "api"),
        "-I" + str(ROOT / "api/modules"), "-I" + str(ROOT / "api/tests"), *sources,
        str(ROOT / "api/tests/update_test.cpp"), "-o", str(temp / "tests")]
    for xp in (0, 1):
        subprocess.run(command + ["-DSPICE_XP=" + str(xp)], check=True)
        subprocess.run([str(temp / "tests")], check=True)
