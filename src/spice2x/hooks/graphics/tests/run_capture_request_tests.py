#!/usr/bin/env python3
"""Compile the capture queue implementation in probe and legacy modes."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / "src/spice2x/hooks/graphics/graphics.cpp"


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


def capture_struct(source: str) -> str:
    return extract_function(source, "struct CaptureData")


def make_shim(source: str, probe: bool) -> str:
    globals_start = source.index(
        "#ifdef SPICE_OPENGL_PROBE\nstruct GraphicsCaptureRequest"
    )
    globals_end = source.index(
        "#ifdef SPICE_OPENGL_PROBE\nstatic void graphics_capture_cancel_pending",
        globals_start,
    )
    globals_block = source[globals_start:globals_end]
    timeout = "static constexpr std::chrono::milliseconds GRAPHICS_CAPTURE_RECEIVE_TIMEOUT {2000};"
    assert timeout in globals_block
    globals_block = globals_block.replace(
        timeout,
        "static constexpr std::chrono::milliseconds GRAPHICS_CAPTURE_RECEIVE_TIMEOUT {20};",
        1,
    )

    cancel_start = source.index(
        "#ifdef SPICE_OPENGL_PROBE\nstatic void graphics_capture_cancel_pending"
    )
    cancel_end = source.index(
        "static std::optional<graphics_orientation>", cancel_start
    )
    cancel_block = source[cancel_start:cancel_end].rstrip()
    functions = [
        cancel_block,
        extract_function(source, "void graphics_capture_trigger(int screen)"),
    ]
    if probe:
        functions += [
            extract_function(source, "bool graphics_capture_consume_request"),
        ]
    functions += [extract_function(source, "bool graphics_capture_consume(int *screen)")]
    if probe:
        functions += [
            extract_function(source, "bool graphics_capture_request_active"),
            extract_function(source, "bool graphics_capture_enqueue_request"),
            extract_function(source, "bool graphics_capture_skip_request"),
        ]
    functions += [
        extract_function(
            source,
            "void graphics_capture_enqueue(int screen, std::shared_ptr<uint8_t[]> data",
        ),
        extract_function(source, "void graphics_capture_enqueue(int screen, uint8_t *data"),
        extract_function(source, "void graphics_capture_skip(int screen)"),
        extract_function(source, "bool graphics_capture_last_size"),
        extract_function(source, "bool graphics_capture_receive_raw"),
    ]

    mode = "#define SPICE_OPENGL_PROBE 1\n" if probe else ""
    main = r'''
static void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

#ifdef SPICE_OPENGL_PROBE
static std::shared_ptr<uint8_t[]> frame(uint8_t value) {
    auto result = std::shared_ptr<uint8_t[]>(new uint8_t[3]);
    result[0] = value;
    result[1] = value + 1;
    result[2] = value + 2;
    return result;
}

static void test_probe_requests() {
    int screen = -1;
    uint64_t first = 0;
    graphics_capture_trigger(0);
    require(graphics_capture_consume_request(&screen, &first), "first request was not consumed");
    require(screen == 0 && first != 0, "first request id is invalid");

    std::shared_ptr<uint8_t[]> out;
    require(!graphics_capture_receive_raw(0, out, 1, nullptr, nullptr, nullptr),
            "empty request did not time out");
    require(!graphics_capture_request_active(0, first), "timed-out request stayed active");
    require(!graphics_capture_enqueue_request(0, first, frame(1), 1, 1),
            "late enqueue for timed-out request was accepted");

    uint64_t second = 0;
    graphics_capture_trigger(0);
    require(graphics_capture_consume_request(&screen, &second), "second request was not consumed");
    require(second != first, "new request reused the old generation");
    require(graphics_capture_enqueue_request(0, second, frame(10), 1, 1),
            "current request enqueue failed");
    require(graphics_capture_receive_raw(0, out, 1, nullptr, nullptr, nullptr) && out[0] == 10,
            "current request did not receive its frame");

    uint64_t third = 0;
    graphics_capture_trigger(0);
    require(graphics_capture_consume_request(&screen, &third), "third request was not consumed");
    require(!graphics_capture_skip_request(0, second),
            "skip for an old request signaled the current request");
    require(graphics_capture_request_active(0, third), "old skip invalidated the current request");
    require(graphics_capture_enqueue_request(0, third, frame(20), 1, 1),
            "current request was corrupted by an old skip");
    require(graphics_capture_receive_raw(0, out, 1, nullptr, nullptr, nullptr) && out[0] == 20,
            "current request frame was lost after an old skip");

    uint64_t fourth = 0;
    graphics_capture_trigger(0);
    require(graphics_capture_consume_request(&screen, &fourth), "fourth request was not consumed");
    auto waiter = std::async(std::launch::async, [&] {
        std::shared_ptr<uint8_t[]> ignored;
        return graphics_capture_receive_raw(0, ignored, 1, nullptr, nullptr, nullptr);
    });
    require(graphics_capture_skip_request(0, fourth), "current skip was rejected");
    require(!waiter.get(), "current skip did not wake the receiver");

    uint64_t fifth = 0;
    graphics_capture_trigger(0);
    require(graphics_capture_consume_request(&screen, &fifth), "fifth request was not consumed");
    uint64_t queued = 0;
    graphics_capture_trigger(0);
    require(!graphics_capture_request_active(0, fifth), "new trigger left the old request active");
    graphics_capture_cancel_pending(0, fifth);
    require(graphics_capture_consume_request(&screen, &queued),
            "queued newer request was removed by old cancellation");
    require(queued != fifth, "queued request id did not advance");
    require(graphics_capture_skip_request(0, queued), "queued request cleanup failed");
}
#endif

int main() {
#ifdef SPICE_OPENGL_PROBE
    test_probe_requests();
#else
    graphics_capture_trigger(0);
    int screen = -1;
    require(graphics_capture_consume(&screen) && screen == 0,
            "legacy trigger/consume behavior changed");
#endif
    return 0;
}
'''

    return f'''{mode}
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

inline constexpr size_t GRAPHICS_CAPTURE_SCREEN_NO = 4;
{capture_struct(source)};
static uint64_t get_performance_milliseconds() {{
    static uint64_t value = 0;
    return ++value;
}}
{globals_block}
{chr(10).join(functions)}
{main}
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


def run_variant(cxx: str, source: str, probe: bool, directory: Path) -> None:
    suffix = "probe" if probe else "legacy"
    shim = directory / f"capture_request_{suffix}.cpp"
    binary = directory / f"capture_request_{suffix}"
    shim.write_text(make_shim(source, probe), encoding="utf-8")
    command = [cxx, "-std=c++20", "-O0", "-pthread", str(shim), "-o", str(binary)]
    if probe:
        command.insert(1, "-DSPICE_OPENGL_PROBE=1")
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{suffix} compile failed:\n{result.stdout}{result.stderr}")
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{suffix} test failed:\n{result.stdout}{result.stderr}")


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    cxx = compiler()
    with tempfile.TemporaryDirectory(prefix="spice-capture-request-") as temp:
        directory = Path(temp)
        run_variant(cxx, source, True, directory)
        run_variant(cxx, source, False, directory)
    print("capture request tests passed (probe and legacy)")


if __name__ == "__main__":
    main()
