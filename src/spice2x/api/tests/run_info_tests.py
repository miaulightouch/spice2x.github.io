#!/usr/bin/env python3
"""Compile the production info API against a portable file/Win32 adapter."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
ADAPTERS = {
    "api/module.cpp": ["util/logging.h"],
    "api/request.cpp": ["../util/logging.h"],
    "api/response.cpp": ["util/logging.h"],
    "api/modules/info.cpp": [
        "avs/game.h", "avs/ea3.h", "util/logging.h", "util/utils.h",
        "util/memutils.h", "util/crypt.h", "build/defs.h",
    ],
}


def main():
    with tempfile.TemporaryDirectory(prefix="spice-info-test-") as directory:
        temp = Path(directory)
        compiled = []
        for name, headers in ADAPTERS.items():
            source = ROOT / name
            contents = source.read_text()
            for header in headers:
                original = '#include "' + header + '"'
                if original not in contents:
                    raise RuntimeError("Platform include changed: " + original)
                contents = contents.replace(original, '#include "info_portable.h"')
            adapted = temp / name.replace("/", "_")
            adapted.write_text(contents)
            compiled.append(str(adapted))

        (temp / "info_portable.h").write_text((ROOT / "api/tests/info_portable.h").read_text())
        test = temp / "info_test.cpp"
        test.write_text((ROOT / "api/tests/info_test.cpp").read_text())
        executable = temp / "info_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-DFMT_HEADER_ONLY",
            "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "api/modules"), "-I" + str(temp),
            *compiled, str(test), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable), str(temp)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
