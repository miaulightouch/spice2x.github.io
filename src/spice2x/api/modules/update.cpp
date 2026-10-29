#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>

#include "update.h"

#include <filesystem>
#include <stdexcept>
#include <vector>

#include "build/defs.h"
#include "util/logging.h"
#include "util/ota_update.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    Update::Update() : Module("update", true) {
        functions["status"] = std::bind(&Update::status, this, _1, _2);
        functions["begin"] = std::bind(&Update::begin, this, _1, _2);
        functions["chunk"] = std::bind(&Update::chunk, this, _1, _2);
        functions["finish"] = std::bind(&Update::finish, this, _1, _2);
        functions["apply"] = std::bind(&Update::apply, this, _1, _2);
        functions["cancel"] = std::bind(&Update::cancel, this, _1, _2);
    }

    Update::~Update() {
        if (!upload_id.empty()) {
            try {
                ota::cancel(upload_id);
            } catch (const std::exception &exception) {
                log_warning("ota", "Upload cleanup failed: {}", exception.what());
            }
        }
    }

    bool Update::owns_upload(Request &req, Response &res, size_t count) {
        if (req.params.Size() != count || !req.params[0].IsString() || upload_id.empty() ||
                std::string(req.params[0].GetString(), req.params[0].GetStringLength()) != upload_id) {
            error(res, "Invalid upload session.");
            return false;
        }
        return true;
    }

    void Update::status(Request &req, Response &res) {
        if (!req.params.Empty()) return error(res, "Expected no parameters.");
        wchar_t path[32768] {};
        const auto length = GetModuleFileNameW(nullptr, path, std::size(path));
        if (length == 0 || length >= std::size(path)) return error(res, "Unable to locate executable.");
        const auto executable = std::filesystem::path(path).filename().string();
        auto &alloc = res.doc()->GetAllocator();
        Value data(kObjectType);
        data.AddMember("version", Value(VERSION_STRING, alloc), alloc);
        data.AddMember("architecture", Value(sizeof(void *) == 8 ? "x64" : "x86", alloc), alloc);
#if SPICE_XP
        data.AddMember("xp", true, alloc);
#else
        data.AddMember("xp", false, alloc);
#endif
        data.AddMember("executable", Value(executable.c_str(), alloc), alloc);
        data.AddMember("max_file_bytes", static_cast<uint64_t>(ota::max_file_bytes), alloc);
        data.AddMember("max_total_bytes", static_cast<uint64_t>(ota::max_total_bytes), alloc);
        data.AddMember("chunk_bytes", static_cast<uint64_t>(ota::chunk_bytes), alloc);
        try {
            data.AddMember("executable_crc32", ota::executable_crc32(), alloc);
        } catch (const std::exception &exception) {
            return error(res, exception.what());
        }
        res.add_data(data);
    }

    void Update::begin(Request &req, Response &res) {
        if (!upload_id.empty()) return error(res, "Cancel the current upload first.");
        if (req.params.Size() != 1 || !req.params[0].IsArray() ||
                req.params[0].Empty() || req.params[0].Size() > 2) {
            return error(res, "Expected one or two executable files.");
        }
        std::vector<ota::UploadFile> files;
        for (const auto &file : req.params[0].GetArray()) {
            if (!file.IsObject() || file.MemberCount() != 3 || !file.HasMember("name") ||
                    !file["name"].IsString() || !file.HasMember("size") ||
                    !file["size"].IsUint64() || !file.HasMember("crc32") || !file["crc32"].IsUint()) {
                return error(res, "Expected file name, size and CRC32.");
            }
            files.push_back({std::string(file["name"].GetString(), file["name"].GetStringLength()),
                file["size"].GetUint64(), file["crc32"].GetUint()});
        }
        try {
            upload_id = ota::begin(files);
            auto &alloc = res.doc()->GetAllocator();
            Value data(kObjectType);
            data.AddMember("id", Value(upload_id.c_str(), alloc), alloc);
            res.add_data(data);
        } catch (const std::exception &exception) {
            error(res, exception.what());
        }
    }

    void Update::chunk(Request &req, Response &res) {
        if (!owns_upload(req, res, 4)) return;
        if (!req.params[1].IsString() || !req.params[2].IsUint64() || !req.params[3].IsString()) {
            return error(res, "Expected file name, byte offset and base64 data.");
        }
        const auto &encoded = req.params[3];
        const auto length = encoded.GetStringLength();
        if (length == 0 || length > ((ota::chunk_bytes + 2) / 3) * 4 || length % 4 != 0) {
            return error(res, "Invalid upload chunk size.");
        }
        DWORD size = static_cast<DWORD>(ota::chunk_bytes);
        std::vector<uint8_t> bytes(size);
        if (!CryptStringToBinaryA(encoded.GetString(), length, CRYPT_STRING_BASE64,
                bytes.data(), &size, nullptr, nullptr) || size == 0 || size > ota::chunk_bytes) {
            return error(res, "Invalid base64 upload data.");
        }
        bytes.resize(size);
        try {
            const auto received = ota::chunk(upload_id,
                std::string(req.params[1].GetString(), req.params[1].GetStringLength()),
                req.params[2].GetUint64(), bytes);
            auto &alloc = res.doc()->GetAllocator();
            Value data(kObjectType);
            data.AddMember("received", received, alloc);
            res.add_data(data);
        } catch (const std::exception &exception) {
            error(res, exception.what());
        }
    }

    void Update::finish(Request &req, Response &res) {
        if (!owns_upload(req, res, 1)) return;
        try {
            const auto files = ota::finish(upload_id);
            auto &alloc = res.doc()->GetAllocator();
            Value data(kObjectType);
            Value list(kArrayType);
            for (const auto &file : files) {
                Value item(kObjectType);
                item.AddMember("name", Value(file.name.c_str(), alloc), alloc);
                item.AddMember("size", file.size, alloc);
                Value signature(kObjectType);
                signature.AddMember("status", Value(file.signature.status.c_str(), alloc), alloc);
                signature.AddMember("signer", Value(file.signature.signer.c_str(), alloc), alloc);
                signature.AddMember("detail", Value(file.signature.detail.c_str(), alloc), alloc);
                item.AddMember("signature", signature, alloc);
                list.PushBack(item, alloc);
            }
            data.AddMember("files", list, alloc);
            res.add_data(data);
        } catch (const std::exception &exception) {
            error(res, exception.what());
        }
    }

    void Update::apply(Request &req, Response &res) {
        if (!owns_upload(req, res, 1)) return;
        try {
            ota::apply(upload_id);
            upload_id.clear();
            auto &alloc = res.doc()->GetAllocator();
            Value data(kObjectType);
            data.AddMember("restarting", true, alloc);
            res.add_data(data);
        } catch (const std::exception &exception) {
            error(res, exception.what());
        }
    }

    void Update::cancel(Request &req, Response &res) {
        if (!owns_upload(req, res, 1)) return;
        try {
            ota::cancel(upload_id);
            upload_id.clear();
        } catch (const std::exception &exception) {
            error(res, exception.what());
        }
    }
}
