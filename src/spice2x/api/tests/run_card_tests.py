#!/usr/bin/env python3
"""Compile the card API and shared native store against portable file/input adapters."""

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
    "api/modules/card.cpp": ["misc/eamuse.h", "util/logging.h", "util/utils.h"],
    "cfg/card_manager.cpp": ["util/fileutils.h", "util/logging.h", "util/utils.h"],
}


def main():
    with tempfile.TemporaryDirectory(prefix="spice-card-test-") as directory:
        temp = Path(directory)
        sources = []
        for name, headers in ADAPTERS.items():
            contents = (ROOT / name).read_text()
            for header in headers:
                original = '#include "' + header + '"'
                if original not in contents:
                    raise RuntimeError("Platform include changed: " + original)
                contents = contents.replace(original, '#include "card_portable.h"')
            target = temp / name.replace("/", "_")
            target.write_text(contents)
            sources.append(str(target))
        utils = (ROOT / "util/utils.h").read_text()
        begin = utils.index("static inline int _hex2bin_helper")
        end = utils.index("template<typename T>\nstatic inline std::string bin2hex(const std::vector", begin)
        (temp / "card_hex.h").write_text(
            'inline constexpr char HEX_LOOKUP_UPPERCASE[] = "0123456789ABCDEF";\n' + utils[begin:end]
        )
        (temp / "card_portable.h").write_text((ROOT / "api/tests/card_portable.h").read_text())
        test = temp / "card_test.cpp"
        test.write_text((ROOT / "api/tests/card_test.cpp").read_text())
        binary = temp / "test"
        subprocess.run([
            os.getenv("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.getenv("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-DFMT_HEADER_ONLY",
            "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "api/modules"), "-I" + str(ROOT / "cfg"), "-I" + str(temp),
            *sources, str(test), "-o", str(binary),
        ], check=True)
        for mode in ("missing", "legacy", "invalid", "invalid-color", "invalid-id"):
            fixture = temp / mode
            fixture.mkdir()
            subprocess.run([str(binary), mode], cwd=fixture, check=True)


if __name__ == "__main__":
    main()
