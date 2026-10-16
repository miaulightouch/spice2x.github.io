#include "info.h"
#include <functional>
#include <iomanip>
#include <algorithm>
#include <vector>
#include "external/rapidjson/document.h"
#include "avs/game.h"
#include "avs/ea3.h"
#include "util/logging.h"
#include "util/utils.h"
#include "util/memutils.h"
#include "util/crypt.h"
#include "util/scope_guard.h"
#include "build/defs.h"

using namespace std::placeholders;
using namespace rapidjson;


namespace api::modules {

    Info::Info() : Module("info") {
        functions["avs"] = std::bind(&Info::avs, this, _1, _2);
        functions["launcher"] = std::bind(&Info::launcher, this, _1, _2);
        functions["memory"] = std::bind(&Info::memory, this, _1, _2);
        functions["log"] = std::bind(&Info::log, this, _1, _2);
        require_password("log");
    }

    // Read only this process's active log; callers cannot supply a file path.
    void Info::log(Request &req, Response &res) {
        const auto params = req.params.GetArray();
        if (params.Size() > 2 || (params.Size() > 0 && !params[0].IsNull() && !params[0].IsUint64()) ||
                (params.Size() > 1 && (!params[1].IsUint() || params[1].GetUint() == 0 || params[1].GetUint() > 262144))) {
            return error(res, "expected [offset=null or unsigned byte offset, limit=1..262144]");
        }
        if (LOG_FILE_PATH.empty()) return error(res, "No active log file is available.");
        HANDLE handle = CreateFileA(LOG_FILE_PATH.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return error(res, "Unable to open the active log file.");
        const auto file = scope_guard{[handle] { CloseHandle(handle); }};
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle, &length) || length.QuadPart < 0) return error(res, "Unable to read log size.");
        const auto size = static_cast<uint64_t>(length.QuadPart);
        const DWORD limit = params.Size() > 1 ? params[1].GetUint() : 65536;
        const uint64_t offset = params.Size() > 0 && !params[0].IsNull() ? params[0].GetUint64() :
                (size > limit ? size - limit : 0);
        if (offset > size) return error(res, "Log size changed; read it again.");
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return error(res, "Unable to seek the log file.");
        std::vector<uint8_t> bytes(static_cast<size_t>(std::min<uint64_t>(limit, size - offset)));
        DWORD count = 0;
        if (!bytes.empty() && !ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            return error(res, "Unable to read the log file.");
        }
        const auto encoded = crypt::base64_encode(bytes.data(), count);
        auto &alloc = res.doc()->GetAllocator();
        Value data(kObjectType);
        data.AddMember("offset", offset, alloc);
        data.AddMember("next", offset + count, alloc);
        data.AddMember("size", size, alloc);
        data.AddMember("data", Value(encoded.c_str(), static_cast<SizeType>(encoded.size()), alloc), alloc);
        res.add_data(data);
    }

    /**
     * avs()
     */
    void Info::avs(Request &req, Response &res) {

        // get allocator
        auto &alloc = res.doc()->GetAllocator();

        // build info object
        Value info(kObjectType);
        info.AddMember("model", StringRef(avs::game::MODEL, 3), alloc);
        info.AddMember("dest", StringRef(avs::game::DEST, 1), alloc);
        info.AddMember("spec", StringRef(avs::game::SPEC, 1), alloc);
        info.AddMember("rev", StringRef(avs::game::REV, 1), alloc);
        info.AddMember("ext", StringRef(avs::game::EXT, 10), alloc);
        info.AddMember("services", StringRef(avs::ea3::EA3_BOOT_URL.c_str()), alloc);

        // add info object
        res.add_data(info);
    }

    /**
     * launcher()
     */
    void Info::launcher(Request &req, Response &res) {

        // get allocator
        auto &alloc = res.doc()->GetAllocator();

        // build args
        Value args(kArrayType);
        for (int count = 0; count < LAUNCHER_ARGC; count++) {
            auto arg = LAUNCHER_ARGV[count];
            args.PushBack(StringRef(arg), alloc);
        }

        // get system time
        auto t_now = std::time(nullptr);
        auto tm_now = *std::gmtime(&t_now);
        auto tm_str = to_string(std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%SZ"));
        Value system_time(tm_str.c_str(), alloc);

        // build info object
        Value info(kObjectType);
        info.AddMember("version", StringRef(VERSION_STRING), alloc);
        info.AddMember("compile_date", StringRef(__DATE__), alloc);
        info.AddMember("compile_time", StringRef(__TIME__), alloc);
        info.AddMember("system_time", system_time, alloc);
        info.AddMember("args", args, alloc);

        // add info object
        res.add_data(info);
    }

    /**
     * memory()
     */
    void Info::memory(Request &req, Response &res) {

        // get allocator
        auto &alloc = res.doc()->GetAllocator();

        // build info object
        Value info(kObjectType);
        info.AddMember("mem_total", memutils::mem_total(), alloc);
        info.AddMember("mem_total_used", memutils::mem_total_used(), alloc);
        info.AddMember("mem_used", memutils::mem_used(), alloc);
        info.AddMember("vmem_total", memutils::vmem_total(), alloc);
        info.AddMember("vmem_total_used", memutils::vmem_total_used(), alloc);
        info.AddMember("vmem_used", memutils::vmem_used(), alloc);

        // add info object
        res.add_data(info);
    }
}
