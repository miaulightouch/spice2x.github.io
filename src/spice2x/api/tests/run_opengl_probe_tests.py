#!/usr/bin/env python3
"""Compile the real OpenGL probe API validator against tiny test doubles."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
INCLUDE_ROOT = ROOT / "src/spice2x"
SOURCE = ROOT / "src/spice2x/api/modules/capture.cpp"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    index = brace
    state = "code"
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                state = "line_comment"
                index += 2
                continue
            if char == "/" and next_char == "*":
                state = "block_comment"
                index += 2
                continue
            if char in "\"'":
                state = char
                index += 1
                continue
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 2
                continue
        else:
            if char == "\\":
                index += 2
                continue
            if char == state:
                state = "code"
        index += 1
    raise AssertionError(f"unterminated function: {signature}")


def make_shim(source: str) -> str:
    method = extract_function(source, "void Capture::opengl_probe")
    return f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "external/rapidjson/document.h"

using namespace rapidjson;

namespace api::modules {{

struct Request {{
    Document params;
}};

struct Response {{
    Document document;
    Value data;
    std::string error_message;
    bool has_data = false;

    Document *doc() {{ return &document; }}

    void add_data(const Value &value) {{
        data.CopyFrom(value, document.GetAllocator());
        has_data = true;
    }}
}};

class Capture {{
public:
    void opengl_probe(Request &req, Response &res);

private:
    void error(Response &res, const char *message) {{
        res.error_message = message;
    }}
}};

}}

static int BACKEND_CALLS = 0;
static std::vector<std::string> BACKEND_MODES;
static std::vector<bool> BACKEND_RESETS;
static std::string BACKEND_REPORT = "{{\\"ok\\":true}}";

std::string graphics_opengl_probe(std::string_view mode = {{}}, bool reset = false) {{
    ++BACKEND_CALLS;
    BACKEND_MODES.emplace_back(mode);
    BACKEND_RESETS.push_back(reset);
    return BACKEND_REPORT;
}}

using namespace api::modules;
{method}

static void require(bool condition, const char *message) {{
    if (!condition) {{
        std::fprintf(stderr, "FAIL: %s\\n", message);
        std::abort();
    }}
}}

static Value string_value(const char *value, size_t length, Document &document) {{
    Value result;
    result.SetString(value, length, document.GetAllocator());
    return result;
}}

static void reset_backend() {{
    BACKEND_CALLS = 0;
    BACKEND_MODES.clear();
    BACKEND_RESETS.clear();
    BACKEND_REPORT = "{{\\"ok\\":true}}";
}}

static void expect_valid(Capture &capture, Request &req, const char *mode, bool reset) {{
    Response response;
    capture.opengl_probe(req, response);
    require(response.error_message.empty(), "valid request returned an error");
    require(response.has_data, "valid request returned no data");
    require(BACKEND_CALLS == 1, "valid request did not call the backend once");
    require(BACKEND_MODES[0] == mode && BACKEND_RESETS[0] == reset,
            "backend received the wrong probe arguments");
    reset_backend();
}}

int main() {{
    Capture capture;
    reset_backend();

    Request empty;
    empty.params.SetArray();
    expect_valid(capture, empty, "", false);

    Request legacy;
    legacy.params.SetArray();
    legacy.params.PushBack(string_value("legacy", 6, legacy.params), legacy.params.GetAllocator());
    expect_valid(capture, legacy, "legacy", false);

    Request shader;
    shader.params.SetArray();
    shader.params.PushBack(string_value("shader", 6, shader.params), shader.params.GetAllocator());
    expect_valid(capture, shader, "shader", false);

    Request pbo;
    pbo.params.SetArray();
    pbo.params.PushBack(string_value("shader-pbo", 10, pbo.params), pbo.params.GetAllocator());
    pbo.params.PushBack(true, pbo.params.GetAllocator());
    expect_valid(capture, pbo, "shader-pbo", true);

    auto reject = [&](Request &req, const char *message) {{
        Response response;
        capture.opengl_probe(req, response);
        require(!response.error_message.empty(), message);
        require(BACKEND_CALLS == 0, "rejected request invoked the backend");
    }};

    Request wrong_first;
    wrong_first.params.SetArray();
    wrong_first.params.PushBack(true, wrong_first.params.GetAllocator());
    reject(wrong_first, "wrong first parameter was accepted");

    Request wrong_second;
    wrong_second.params.SetArray();
    wrong_second.params.PushBack(string_value("shader", 6, wrong_second.params),
            wrong_second.params.GetAllocator());
    wrong_second.params.PushBack(string_value("true", 4, wrong_second.params),
            wrong_second.params.GetAllocator());
    reject(wrong_second, "wrong reset parameter was accepted");

    Request too_many;
    too_many.params.SetArray();
    too_many.params.PushBack(string_value("legacy", 6, too_many.params),
            too_many.params.GetAllocator());
    too_many.params.PushBack(false, too_many.params.GetAllocator());
    too_many.params.PushBack(false, too_many.params.GetAllocator());
    reject(too_many, "too many parameters were accepted");

    Request unknown;
    unknown.params.SetArray();
    unknown.params.PushBack(string_value("other", 5, unknown.params),
            unknown.params.GetAllocator());
    reject(unknown, "unknown mode was accepted");

    Request embedded_nul;
    embedded_nul.params.SetArray();
    const char mode_with_suffix[] = "shader\\0suffix";
    embedded_nul.params.PushBack(
            string_value(mode_with_suffix, sizeof(mode_with_suffix) - 1, embedded_nul.params),
            embedded_nul.params.GetAllocator());
    reject(embedded_nul, "mode with an embedded NUL suffix was accepted");

    BACKEND_REPORT = "not-json";
    Request invalid_report;
    invalid_report.params.SetArray();
    Response response;
    capture.opengl_probe(invalid_report, response);
    require(!response.error_message.empty(), "invalid backend report was accepted");
    require(BACKEND_CALLS == 1, "invalid report did not call the backend once");
    return 0;
}}
'''


def compiler() -> str:
    requested = os.environ.get("CXX")
    if requested:
        path = shutil.which(requested)
        if path:
            return path
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    raise RuntimeError("no C++ compiler found; set CXX to a C++20 compiler")


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    cxx = compiler()
    with tempfile.TemporaryDirectory(prefix="spice-opengl-probe-") as temp:
        directory = Path(temp)
        shim = directory / "opengl_probe.cpp"
        binary = directory / "opengl_probe"
        shim.write_text(make_shim(source), encoding="utf-8")
        command = [
            cxx,
            "-std=c++20",
            "-O0",
            "-DSPICE_OPENGL_PROBE=1",
            "-I",
            str(INCLUDE_ROOT),
            str(shim),
            "-o",
            str(binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"compile failed:\n{result.stdout}{result.stderr}")
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f"test failed:\n{result.stdout}{result.stderr}")
    print("OpenGL probe API tests passed")


if __name__ == "__main__":
    main()
