#!/usr/bin/env python3
"""Compile production analog/light API modules against inert I/O adapters."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HEADERS = {
    "api/modules/analogs.h": ["cfg/api.h"],
    "api/modules/lights.h": ["cfg/api.h"],
}
SOURCES = {
    "api/modules/analogs.cpp": [
        "misc/eamuse.h", "cfg/analog.h", "launcher/launcher.h", "games/io.h",
        "util/utils.h", "util/scope_guard.h", "acio/mdxf/mdxf_poll.h",
    ],
    "api/modules/lights.cpp": [
        "cfg/configurator.h", "misc/eamuse.h", "cfg/light.h", "launcher/launcher.h",
        "games/io.h", "rawinput/rawinput.h", "util/scope_guard.h", "util/utils.h",
    ],
}


def adapt(contents, headers):
    for header in headers:
        original = '#include "' + header + '"'
        if original in contents:
            contents = contents.replace(original, '#include "io_pulse_portable.h"')
            continue
        original = '#include <' + header + '>'
        if original not in contents:
            raise RuntimeError("Platform include changed: " + original)
        contents = contents.replace(original, '#include "io_pulse_portable.h"')
    return contents


def main():
    with tempfile.TemporaryDirectory(prefix="spice-io-pulse-test-") as directory:
        temp = Path(directory)
        (temp / "io_pulse_portable.h").write_text(
            (ROOT / "api/tests/io_pulse_portable.h").read_text())
        (temp / "io_pulse_test_portable.h").write_text(
            '#include "io_pulse_portable.h"\n')
        sources = []
        for name, headers in HEADERS.items():
            output = temp / Path(name).name
            output.write_text(adapt((ROOT / name).read_text(), headers))
        for name, headers in SOURCES.items():
            output = temp / Path(name).name
            output.write_text(adapt((ROOT / name).read_text(), headers))
            sources.append(str(output))

        executable = temp / "io_pulse_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-I" + str(temp), "-I" + str(ROOT),
            "-I" + str(ROOT / "api"), "-I" + str(ROOT / "api/modules"),
            *sources, str(ROOT / "api/tests/io_pulse_test.cpp"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
