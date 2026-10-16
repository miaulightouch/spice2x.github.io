#!/usr/bin/env python3
"""Test the portable display-mode logging helper."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "util/sysutils.cpp").read_text()
START = SOURCE.index("// display mode helper begin")
END = SOURCE.index("// display mode helper end", START)
HELPER = SOURCE[START:END]

PROLOGUE = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using BOOL = int;
using DWORD = std::uint32_t;
constexpr DWORD DM_INTERLACED = 2;

struct DEVMODEA {
    DWORD dmSize = 0;
    DWORD dmPelsWidth = 0;
    DWORD dmPelsHeight = 0;
    DWORD dmDisplayFrequency = 0;
    DWORD dmBitsPerPel = 0;
    DWORD dmDisplayFlags = 0;
    DWORD dmFields = 0;
};

struct FakeMode {
    DWORD width;
    DWORD height;
    DWORD refresh;
    DWORD bits_per_pixel;
    DWORD flags;
    DWORD fields;
};

struct Call {
    std::string display_name;
    DWORD mode_index;
};

const std::map<std::string, std::vector<FakeMode>> available = {
    {"\\\\.\\DISPLAY1", {
        {1920, 1080, 60, 32, 0, 0x1111},
        {1920, 1080, 60, 32, 0, 0x2222},
        {1920, 1080, 60, 32, DM_INTERLACED, 0x3333},
    }},
    {"\\\\.\\DISPLAY2", {
        {1280, 720, 60, 32, 0, 0x4444},
        {1280, 720, 60, 32, 0, 0x5555},
    }},
};
std::vector<Call> calls;
std::vector<std::string> logs;

BOOL EnumDisplaySettingsA(const char *display_name, DWORD mode_index, DEVMODEA *out) {
    calls.push_back({display_name, mode_index});
    assert(out->dmSize == sizeof(DEVMODEA));
    const auto found = available.find(display_name);
    if (found == available.end() || mode_index >= found->second.size()) return 0;

    const auto &mode = found->second[mode_index];
    out->dmPelsWidth = mode.width;
    out->dmPelsHeight = mode.height;
    out->dmDisplayFrequency = mode.refresh;
    out->dmBitsPerPel = mode.bits_per_pixel;
    out->dmDisplayFlags = mode.flags;
    out->dmFields = mode.fields;
    return 1;
}

template <typename... Args>
void log_misc(const char *, const char *format, Args &&...args) {
    std::ostringstream output;
    output << format;
    ((output << '|' << std::forward<Args>(args)), ...);
    logs.push_back(output.str());
}
'''

TEST = r'''
int main() {
    print_display_modes(0, "\\\\.\\DISPLAY1");
    assert(calls.size() == 4);
    assert(calls[0].display_name == "\\\\.\\DISPLAY1" && calls[0].mode_index == 0);
    assert(calls[3].mode_index == 3);
    assert(logs.size() == 2);
    assert(logs[0].find("|1920|") != std::string::npos);
    assert(logs[1].find("|2|") != std::string::npos);

    calls.clear();
    logs.clear();
    print_display_modes(1, "\\\\.\\DISPLAY2");
    assert(calls.size() == 3);
    assert(calls[0].display_name == "\\\\.\\DISPLAY2" && calls[0].mode_index == 0);
    assert(logs.size() == 1);
    assert(logs[0].find("|1280|") != std::string::npos);

    calls.clear();
    logs.clear();
    print_display_modes(2, "\\\\.\\UNKNOWN");
    assert(calls.size() == 1);
    assert(calls[0].mode_index == 0);
    assert(logs.size() == 1);
    assert(logs[0].find("none") != std::string::npos);
}
'''

with tempfile.TemporaryDirectory(prefix="spice-display-modes-test-") as directory:
    temp = Path(directory)
    source = temp / "display_modes.cpp"
    source.write_text(PROLOGUE + HELPER + TEST)
    executable = temp / "display_modes_test"
    subprocess.run([
        "g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        str(source), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)

print("Display mode enumeration tests passed: termination, deduplication, multi-display input and empty lists.")
