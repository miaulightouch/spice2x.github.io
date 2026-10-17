#!/usr/bin/env python3
"""Compile the production patches API against portable patch-engine adapters."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    with tempfile.TemporaryDirectory(prefix="spice-patches-test-") as directory:
        temp = Path(directory)
        portable = ROOT / "api/tests/patches_portable.h"
        sources = []
        adapters = {
            "api/module.cpp": ["util/logging.h"],
            "api/request.cpp": ["../util/logging.h"],
            "api/response.cpp": ["util/logging.h"],
            "api/modules/patches.cpp": [
                "avs/game.h", "cfg/configurator.h", "patcher/patch_manager.h", "launcher/launcher.h",
                "util/utils.h",
            ],
        }
        for name, includes in adapters.items():
            content = (ROOT / name).read_text()
            for include in includes:
                original = '#include "' + include + '"'
                if original not in content:
                    raise RuntimeError("Platform include changed: " + original)
                content = content.replace(original, '#include "patches_portable.h"')
            target = temp / name.replace("/", "_")
            target.write_text(content)
            sources.append(str(target))

        loader_group = (ROOT / "patcher/loader_group.cpp").read_text()
        for include in ['"internal.h"', '"util/logging.h"', '"util/utils.h"']:
            original = "#include " + include
            if original not in loader_group:
                raise RuntimeError("Platform include changed: " + original)
            loader_group = loader_group.replace(original, '#include "patches_portable.h"')
        loader_group_source = temp / "patcher_loader_group.cpp"
        loader_group_source.write_text(loader_group)
        sources.append(str(loader_group_source))

        executable = temp / "patches_tests"
        command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DRAPIDJSON_HAS_STDSTRING", "-DFMT_HEADER_ONLY",
            "-I" + str(ROOT), "-I" + str(ROOT / "api"),
            "-I" + str(ROOT / "api/modules"), "-I" + str(ROOT / "api/tests"),
            "-I" + str(temp), *sources,
            str(ROOT / "api/tests/patches_test.cpp"), "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], cwd=temp, check=True)

        lifecycle = (ROOT / "patcher/lifecycle.cpp").read_text()
        for include in [
            '"internal.h"', '<windows.h>', '"avs/game.h"',
            '"cfg/configurator.h"', '"util/fileutils.h"',
            '"util/logging.h"', '"util/utils.h"',
        ]:
            original = "#include " + include
            if original not in lifecycle:
                raise RuntimeError("Platform include changed: " + original)
            lifecycle = lifecycle.replace(original, '#include "patches_lifecycle_portable.h"')
        lifecycle = lifecycle.replace(
            "std::filesystem::path({\n                data->Loaded.FullDllName->Buffer,\n                data->Loaded.FullDllName->Length / sizeof(wchar_t)\n            })",
            "std::filesystem::path(data->Loaded.FullDllName->Buffer)")
        lifecycle_source = temp / "patcher_lifecycle.cpp"
        lifecycle_source.write_text(lifecycle)
        lifecycle_executable = temp / "patches_lifecycle_tests"
        lifecycle_command = [
            os.environ.get("CXX", "g++"), "-std=c++20", "-pthread",
            *shlex.split(os.environ.get("CXXFLAGS", "")),
            "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-deprecated-declarations", "-Wno-class-memaccess",
            "-DPATCHER_TEST_HOOKS",
            "-I" + str(ROOT), "-I" + str(ROOT / "api/tests"),
            "-I" + str(temp), str(lifecycle_source),
            str(ROOT / "api/tests/patches_lifecycle_test.cpp"),
            "-o", str(lifecycle_executable),
        ]
        subprocess.run(lifecycle_command, check=True)
        subprocess.run([str(lifecycle_executable)], cwd=temp, check=True)


if __name__ == "__main__":
    main()
