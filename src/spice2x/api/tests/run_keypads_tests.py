#!/usr/bin/env python3
"""Compile and exercise the portable keypads API contract."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="spice-keypads-test-") as directory:
        temp = Path(directory)
        sources = []
        adapters = {
            "api/module.cpp": ["util/logging.h"],
            "api/request.cpp": ["../util/logging.h"],
            "api/response.cpp": ["util/logging.h"],
            "api/modules/keypads.cpp": [
                "windows.h", "avs/game.h", "games/iidx/iidx.h", "games/popn/popn.h",
                "misc/eamuse.h",
            ],
        }
        for name, headers in adapters.items():
            contents = (ROOT / name).read_text()
            for header in headers:
                original = '#include "' + header + '"'
                if original not in contents and header == "windows.h":
                    original = "#include <" + header + ">"
                if original not in contents:
                    raise RuntimeError("Platform include changed: " + original)
                contents = contents.replace(original, '#include "keypads_portable.h"')
            target = temp / name.replace("/", "_")
            target.write_text(contents)
            sources.append(str(target))
        (temp / "keypads_portable.h").write_text(
            (ROOT / "api/tests/keypads_portable.h").read_text())
        executable = temp / "keypads_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-DFMT_HEADER_ONLY",
            "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "api/modules"), "-I" + str(temp),
            *sources, str(ROOT / "api/tests/keypads_test.cpp"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
