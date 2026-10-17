#!/usr/bin/env python3
"""Compile the production config API and Config XML persistence with portable adapters."""

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
    "api/modules/config.cpp": [
        "cfg/config.h", "games/io.h", "hooks/audio/asio_driver_scan.h", "launcher/launcher.h",
        "launcher/options.h", "misc/eamuse.h", "util/sysutils.h", "util/utils.h",
    ],
    "cfg/config.cpp": ["config.h", "util/logging.h", "util/fileutils.h"],
}


def main():
    with tempfile.TemporaryDirectory(prefix="spice-config-test-") as directory:
        temp = Path(directory)
        sources = []
        for name, headers in ADAPTERS.items():
            contents = (ROOT / name).read_text()
            for header in headers:
                original = '#include "' + header + '"'
                if original not in contents:
                    raise RuntimeError("Platform include changed: " + original)
                contents = contents.replace(original, '#include "config_portable.h"')
            if name == "cfg/config.cpp":
                contents = contents.replace("this->configFile.LoadFile(", "this->configFile.LoadFileA(")
                contents = contents.replace(
                    "this->configFile.SaveFile(this->configLocationTemp.c_str(), false)",
                    "config_test_save_file(this->configFile, this->configLocationTemp)",
                )
            output = temp / name.replace("/", "_")
            output.write_text(contents)
            sources.append(str(output))

        (temp / "config_portable.h").write_text((ROOT / "api/tests/config_portable.h").read_text())
        tinyxml = (ROOT / "external/tinyxml2/tinyxml2.cpp").read_text()
        tinyxml = tinyxml.replace(
            '#include "tinyxml2.h"',
            '#include "tinyxml2.h"\n'
            '#include <string>\n'
            'inline FILE *_wfopen(const wchar_t *path, const wchar_t *mode) {\n'
            '    const std::wstring wide_path(path), wide_mode(mode);\n'
            '    return std::fopen(std::string(wide_path.begin(), wide_path.end()).c_str(),\n'
            '        std::string(wide_mode.begin(), wide_mode.end()).c_str());\n'
            '}\n',
        )
        tinyxml_source = temp / "tinyxml2.cpp"
        tinyxml_source.write_text(tinyxml)
        executable = temp / "config_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "api/modules"), "-I" + str(ROOT / "cfg"),
            "-I" + str(ROOT / "external/tinyxml2"), "-I" + str(temp),
            *sources, str(tinyxml_source),
            str(ROOT / "api/tests/config_test.cpp"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
