#!/usr/bin/env python3
"""Exercise production config routing with real files and inert Win32 adapters."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'util/fileutils.cpp').read_text()
start = source.index('// Config path helpers.')
end = source.index('\nbool fileutils::write_config_file', start)
ADAPTER = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using DWORD = uint32_t;
constexpr DWORD MAX_PATH = 260;
std::map<std::wstring, std::wstring> environment;
std::wstring executable;
int module_calls = 0;
bool fail_expand = false, fail_module = false, grow_expand = false;
DWORD GetLastError() { return 5; }
std::wstring upper(std::wstring text) {
    for (auto &c : text) c = std::towupper(c);
    return text;
}
const wchar_t *_wgetenv(const wchar_t *name) {
    const auto entry = environment.find(upper(name));
    return entry == environment.end() ? nullptr : entry->second.c_str();
}
DWORD ExpandEnvironmentStringsW(const wchar_t *input, wchar_t *output, DWORD capacity) {
    if (fail_expand) return 0;
    std::wstring text(input), result;
    for (size_t i = 0; i < text.size();) {
        const auto end = text[i] == L'%' ? text.find(L'%', i + 1) : std::wstring::npos;
        if (end != std::wstring::npos) {
            const auto value = _wgetenv(text.substr(i + 1, end - i - 1).c_str());
            result += value ? value : text.substr(i, end - i + 1);
            i = end + 1;
        } else result += text[i++];
    }
    if (!output && grow_expand) { grow_expand = false; return 2; }
    const DWORD required = static_cast<DWORD>(result.size() + 1);
    if (output && capacity >= required) std::copy_n(result.c_str(), required, output);
    return required;
}
DWORD GetModuleFileNameW(void *, wchar_t *output, DWORD capacity) {
    ++module_calls;
    if (fail_module) return 0;
    const auto count = std::min<size_t>(capacity, executable.size());
    std::copy_n(executable.data(), count, output);
    if (count < capacity) output[count] = 0;
    // Match XP: truncated data has no NUL and does not set LastError.
    return static_cast<DWORD>(count);
}
#define log_info(...) ((void)0)
#define log_fatal(...) throw std::runtime_error("Win32/path failure")
namespace fileutils {
extern std::optional<std::filesystem::path> CONFIG_DIRECTORY;
std::filesystem::path expand_config_path(const std::filesystem::path &);
std::filesystem::path executable_directory();
void set_config_directory(const std::string &, bool);
std::filesystem::path get_config_file_path(const std::string, const std::string, bool * = nullptr);
bool file_exists(const std::filesystem::path &path) { return std::filesystem::is_regular_file(path); }
}
'''
TEST = r'''
int main() {
    namespace fs = std::filesystem;
    const auto base = fs::current_path();
    environment[L"APPDATA"] = (base / "profile with spaces").wstring();
    executable = (base / "bin" / "spice.exe").wstring();
    using namespace fileutils;
    assert(expand_config_path("relative/file.json") == fs::path("relative/file.json"));
    assert(expand_config_path("%appdata%/spice/image.json") == base / "profile with spaces/spice/image.json");
    grow_expand = true;
    assert(expand_config_path("%APPDATA%/spice/image.json") == base / "profile with spaces/spice/image.json");
    assert(expand_config_path("%UNDEFINED%/file.json") == fs::path("%UNDEFINED%/file.json"));
    fail_expand = true;
    bool threw = false;
    try { expand_config_path("%APPDATA%/file.json"); } catch (const std::runtime_error &) { threw = true; }
    assert(threw); fail_expand = false;

    set_config_directory("", false); assert(!CONFIG_DIRECTORY);
    const auto appdata = fs::path(environment[L"APPDATA"]);
    fs::create_directories(appdata / "spice2x");
    std::ofstream(appdata / "legacy.json") << "legacy";
    bool exists = false;
    assert(get_config_file_path("test", "legacy.json", &exists) == appdata / "legacy.json" && exists);
    std::ofstream(appdata / "spice2x/legacy.json") << "new";
    assert(get_config_file_path("test", "legacy.json") == appdata / "spice2x/legacy.json");
    assert(get_config_file_path("test", "missing.json", &exists) == appdata / "spice2x/missing.json" && !exists);

    set_config_directory("%APPDATA%/spice", false);
    assert(*CONFIG_DIRECTORY == appdata / "spice");
    // An explicit root never silently reads a different legacy directory.
    assert(get_config_file_path("test", "legacy.json", &exists) == appdata / "spice/legacy.json" && !exists);
    for (const auto *name : {"spicetools.xml", "spicetools_screen_resize.json", "spicetools_patch_manager.json",
            "spicetools_card_manager.json", "spicetools_presets.xml", "spicetools_camera_control.json"}) {
        const auto path = get_config_file_path("test", name, &exists); assert(!exists);
        std::ofstream(path) << "round trip";
        const auto read_path = get_config_file_path("test", name, &exists); assert(exists && read_path == path);
        std::ifstream stream(read_path); std::string value; std::getline(stream, value); assert(value == "round trip");
    }
    CONFIG_DIRECTORY.reset();
    set_config_directory("", true); assert(*CONFIG_DIRECTORY == base / "bin");
    assert(get_config_file_path("test", "spicetools.xml") == base / "bin/spicetools.xml");
    set_config_directory((base / "chosen").string(), true); assert(*CONFIG_DIRECTORY == base / "chosen");
    CONFIG_DIRECTORY.reset();
    set_config_directory("relative-root", false); assert(*CONFIG_DIRECTORY == base / "relative-root");
    fs::current_path(base / "bin"); assert(get_config_file_path("test", "x.json") == base / "relative-root/x.json");
    fs::current_path(base);

    executable = (base / std::string(170, 'a') / std::string(170, 'b') / "spice.exe").wstring();
    module_calls = 0;
    assert(executable_directory() == fs::path(executable).parent_path()); assert(module_calls > 1);
    fail_module = true; threw = false;
    try { executable_directory(); } catch (const std::runtime_error &) { threw = true; }
    assert(threw); fail_module = false;
    std::ofstream(base / "not-a-directory") << "file";
    threw = false;
    try { set_config_directory((base / "not-a-directory").string(), false); } catch (const std::runtime_error &) { threw = true; }
    assert(threw);
    std::cout << "Config path tests passed: environment expansion/retry/failure, legacy precedence, "
        "shared XML/JSON roots, read/write round trips, portable/explicit priority, CWD changes and XP truncation.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='spice-config-paths-') as directory:
    temp = Path(directory)
    cpp = temp / 'paths.cpp'
    cpp.write_text(ADAPTER + source[start:end] + TEST)
    exe = temp / 'paths'
    subprocess.run([os.getenv('CXX', 'g++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], cwd=temp, check=True)
