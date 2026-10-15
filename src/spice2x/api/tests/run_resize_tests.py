#!/usr/bin/env python3
"""Run production resize API/config code with portable platform adapters.

Only logging, game identification, and the Windows file helper boundary are
adapted. JSON parsing, dispatch, validation, locking, and config serialization
are compiled from the production sources. No game data is needed.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCES = {
    "api/module.cpp": ["util/logging.h"],
    "api/request.cpp": ["../util/logging.h"],
    "api/response.cpp": ["util/logging.h"],
    "cfg/screen_resize.cpp": [
        "avs/game.h", "misc/eamuse.h", "util/deferlog.h", "util/utils.h",
        "util/fileutils.h", "hooks/graphics/graphics.h",
    ],
}


def main():
    with tempfile.TemporaryDirectory(prefix="spice-resize-test-") as directory:
        temp = Path(directory)
        compiled = []
        for source, includes in SOURCES.items():
            contents = (ROOT / source).read_text()
            for include in includes:
                original = '#include "' + include + '"'
                if original not in contents:
                    raise RuntimeError("Platform include changed: " + original)
                contents = contents.replace(original, '#include "portable.h"')
            adapted = temp / source.replace("/", "_")
            adapted.write_text(contents)
            compiled.append(str(adapted))

        executable = temp / "resize_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-DFMT_HEADER_ONLY",
            "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "cfg"), "-I" + str(ROOT / "api/tests"),
            *compiled, str(ROOT / "api/modules/resize.cpp"),
            str(ROOT / "api/tests/resize_test.cpp"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
