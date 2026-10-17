#include "patches.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>

#include "avs/game.h"
#include "cfg/configurator.h"
#include "external/rapidjson/document.h"
#include "patcher/patch_manager.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    namespace {

        const char *patch_type_name(patcher::PatchType type) {
            switch (type) {
                case patcher::PatchType::Memory:
                case patcher::PatchType::Signature:
                    return "memory";
                case patcher::PatchType::Union:
                    return "union";
                case patcher::PatchType::Integer:
                    return "integer";
                case patcher::PatchType::Unknown:
                default:
                    return nullptr;
            }
        }

        const char *patch_status_name(patcher::PatchStatus status) {
            switch (status) {
                case patcher::PatchStatus::Enabled:
                    return "enabled";
                case patcher::PatchStatus::Disabled:
                    return "disabled";
                case patcher::PatchStatus::Error:
                default:
                    return "error";
            }
        }

        bool has_choice(const patcher::PatchData &patch, const std::string &choice) {
            return std::any_of(patch.patches_union.begin(), patch.patches_union.end(),
                [&](const patcher::UnionPatch &candidate) {
                    return candidate.name == choice;
                });
        }

        bool exact_params(const Request &req, size_t count, Module &module, Response &res) {
            if (req.params.Size() != count) {
                module.error(res, req.function + " expects exactly " + std::to_string(count)
                    + (count == 1 ? " parameter" : " parameters"));
                return false;
            }
            return true;
        }

        std::string validate_patch_url(const std::string &url) {
            constexpr size_t max_url_length = 4096;
            if (url.empty()) {
                return "url must be a nonempty HTTP(S) base URL";
            }
            if (url.size() > max_url_length) {
                return "url is too long";
            }
            for (const auto character : url) {
                const auto byte = static_cast<unsigned char>(character);
                if (std::iscntrl(byte) || std::isspace(byte)) {
                    return "url must not contain whitespace or control characters";
                }
            }

            size_t authority_start = 0;
            if (url.compare(0, 7, "http://") == 0) {
                authority_start = 7;
            } else if (url.compare(0, 8, "https://") == 0) {
                authority_start = 8;
            } else {
                return "url must start with http:// or https://";
            }

            if (url.find('?') != std::string::npos || url.find('#') != std::string::npos) {
                return "url must not contain a query or fragment";
            }

            const auto authority_end = url.find('/', authority_start);
            const auto authority = url.substr(
                authority_start,
                authority_end == std::string::npos
                    ? std::string::npos
                    : authority_end - authority_start);
            if (authority.empty()) {
                return "url must contain a nonempty authority";
            }
            if (authority.find('@') != std::string::npos) {
                return "url must not contain userinfo";
            }

            if (authority.front() == ':') {
                return "url must contain a nonempty host";
            }
            return {};
        }

    }

    Patches::Patches() : Module("patches", true) {
        functions["get"] = std::bind(&Patches::get, this, _1, _2);
        functions["set"] = std::bind(&Patches::set, this, _1, _2);
        functions["set_auto_apply"] = std::bind(&Patches::set_auto_apply, this, _1, _2);
        functions["save"] = std::bind(&Patches::save, this, _1, _2);
        functions["reload"] = std::bind(&Patches::reload, this, _1, _2);
        functions["import_url"] = std::bind(&Patches::import_url, this, _1, _2);
    }

    static void prepare() {
        patcher::init();
        patcher::process_pending_loader_reload();
        if (!patcher::local_patches_initialized) {
            patcher::reload_local_patches();
        }
    }

    static void add_state(Response &res) {
        auto &allocator = res.doc()->GetAllocator();
        Value game(kObjectType);
        const auto game_id = avs::game::get_identifier();
        game.AddMember("game", Value(game_id.c_str(), allocator), allocator);
        game.AddMember("auto_apply", patcher::setting_auto_apply, allocator);
        game.AddMember("dirty", patcher::config_dirty, allocator);
        game.AddMember("standalone", cfg::CONFIGURATOR_STANDALONE, allocator);
        game.AddMember(
            "can_import",
            cfg::CONFIGURATOR_STANDALONE && !avs::game::DLL_NAME.empty(),
            allocator);
        game.AddMember("import_url", Value(patcher::patch_url.c_str(), allocator), allocator);
        game.AddMember(
            "import_errors",
            Value(patcher::url_fetch_errors.c_str(), allocator),
            allocator);
        Value recent_urls(kArrayType);
        for (const auto &url : patcher::url_recents) {
            recent_urls.PushBack(Value(url.c_str(), allocator), allocator);
        }
        game.AddMember("recent_urls", recent_urls, allocator);

        Value patch_list(kArrayType);
        for (auto &patch : patcher::patches) {
            if (patch_type_name(patch.type) == nullptr) {
                continue;
            }

            const auto previous_error = patch.error_reason;
            patch.error_reason.clear();
            patch.last_status = patcher::is_patch_active(patch);
            if (patch.last_status == patcher::PatchStatus::Error
                    && !previous_error.empty()
                    && patch.error_reason.find(previous_error) == std::string::npos) {
                if (!patch.error_reason.empty()) {
                    patch.error_reason = previous_error + "; " + patch.error_reason;
                } else {
                    patch.error_reason = previous_error;
                }
            }

            Value item(kObjectType);
            item.AddMember("id", Value(patch.hash.c_str(), allocator), allocator);
            item.AddMember("name", Value(patch.name.c_str(), allocator), allocator);
            item.AddMember("description", Value(patch.description.c_str(), allocator), allocator);
            item.AddMember("caution", Value(patch.caution.c_str(), allocator), allocator);
            item.AddMember("group", Value(patch.group_id.c_str(), allocator), allocator);
            item.AddMember("type", Value(patch_type_name(patch.type), allocator), allocator);
            item.AddMember("enabled", patch.enabled, allocator);
            item.AddMember("status", Value(patch_status_name(patch.last_status), allocator), allocator);
            item.AddMember("error", Value(patch.error_reason.c_str(), allocator), allocator);
            item.AddMember("unverified", patch.unverified, allocator);

            Value choices(kArrayType);
            if (patch.type == patcher::PatchType::Union) {
                for (const auto &choice : patch.patches_union) {
                    choices.PushBack(Value(choice.name.c_str(), allocator), allocator);
                }
            }
            item.AddMember("choices", choices, allocator);
            item.AddMember("selected", Value(patch.selected_union_name.c_str(), allocator), allocator);

            if (patch.type == patcher::PatchType::Integer) {
                item.AddMember("min", patch.patch_number.min, allocator);
                item.AddMember("max", patch.patch_number.max, allocator);
                if (patch.last_status != patcher::PatchStatus::Error && !patch.unverified) {
                    item.AddMember("value", patch.patch_number.value, allocator);
                }
            }
            patch_list.PushBack(item, allocator);
        }
        game.AddMember("patches", patch_list, allocator);
        res.add_data(game);
    }

    void Patches::get(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 0, *this, res)) {
            return;
        }
        prepare();
        add_state(res);
    }

    void Patches::set(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 2, *this, res)) {
            return;
        }
        if (!req.params[0].IsString()) {
            return error_type(res, "id", "string");
        }
        if (!req.params[1].IsObject()) {
            return error_type(res, "change", "object");
        }

        prepare();
        const std::string id(req.params[0].GetString(), req.params[0].GetStringLength());
        auto patch_it = std::find_if(patcher::patches.begin(), patcher::patches.end(),
            [&](const patcher::PatchData &patch) { return patch.hash == id; });
        if (patch_it == patcher::patches.end()) {
            return error_unknown(res, "id", id);
        }
        auto &patch = *patch_it;

        bool has_enabled = false;
        bool has_choice_field = false;
        bool has_value = false;
        bool enabled = false;
        std::string choice;
        int32_t value = 0;
        for (auto it = req.params[1].MemberBegin(); it != req.params[1].MemberEnd(); ++it) {
            const std::string field(it->name.GetString(), it->name.GetStringLength());
            if (field == "enabled") {
                if (has_enabled) {
                    return error(res, "change contains duplicate enabled field");
                }
                if (!it->value.IsBool()) {
                    return error_type(res, "enabled", "bool");
                }
                has_enabled = true;
                enabled = it->value.GetBool();
            } else if (field == "choice") {
                if (has_choice_field) {
                    return error(res, "change contains duplicate choice field");
                }
                if (!it->value.IsString()) {
                    return error_type(res, "choice", "string");
                }
                has_choice_field = true;
                choice.assign(it->value.GetString(), it->value.GetStringLength());
            } else if (field == "value") {
                if (has_value) {
                    return error(res, "change contains duplicate value field");
                }
                if (!it->value.IsInt()) {
                    return error_type(res, "value", "integer");
                }
                has_value = true;
                value = it->value.GetInt();
            } else {
                return error_unknown(res, "change field", field);
            }
        }
        if (!has_enabled && !has_choice_field && !has_value) {
            return error(res, "change must contain enabled, choice, or value");
        }
        if (has_choice_field && patch.type != patcher::PatchType::Union) {
            return error(res, "choice is only valid for union patches");
        }
        if (has_value && patch.type != patcher::PatchType::Integer) {
            return error(res, "value is only valid for integer patches");
        }
        if (has_choice_field && !has_choice(patch, choice)) {
            return error_unknown(res, "choice", choice);
        }
        if (has_value && (value < patch.patch_number.min || value > patch.patch_number.max)) {
            return error(res, "value must be between "
                + std::to_string(patch.patch_number.min) + " and "
                + std::to_string(patch.patch_number.max));
        }
        if (patch_type_name(patch.type) == nullptr) {
            return error(res, "unsupported patch type");
        }

        const bool desired_enabled = has_enabled ? enabled : patch.enabled;
        if ((has_choice_field || has_value) && !desired_enabled) {
            return error(res, "choice and value require enabled=true");
        }

        patch.error_reason.clear();
        patch.last_status = patcher::is_patch_active(patch);

        const bool old_enabled = patch.enabled;
        const bool old_auto_apply = patcher::setting_auto_apply;
        const auto old_choice = patch.selected_union_name;
        int32_t old_value = 0;
        if (patch.type == patcher::PatchType::Integer) {
            old_value = patch.patch_number.value;
        }

        const bool choice_changed = has_choice_field && choice != patch.selected_union_name;
        const bool value_changed = has_value && value != patch.patch_number.value;
        const bool enabled_changed = has_enabled && enabled != patch.enabled;
        if (has_choice_field) {
            patch.selected_union_name = choice;
        }
        if (has_value) {
            patch.patch_number.value = value;
        }
        patch.enabled = desired_enabled;
        if (desired_enabled && !old_enabled) {
            patcher::setting_auto_apply = true;
        }

        const bool needs_apply = enabled_changed || choice_changed || value_changed
            || (has_enabled && patch.last_status != (patch.enabled
                ? patcher::PatchStatus::Enabled
                : patcher::PatchStatus::Disabled));
        if (needs_apply) {
            const bool applied = patcher::apply_patch(patch, patch.enabled);
            patch.error_reason.clear();
            patch.last_status = patcher::is_patch_active(patch);
            const auto wanted_status = patch.enabled
                ? patcher::PatchStatus::Enabled
                : patcher::PatchStatus::Disabled;
            if (!applied || patch.last_status != wanted_status) {
                const auto failure = patch.error_reason.empty()
                    ? "Failed to apply patch"
                    : patch.error_reason;
                patch.enabled = old_enabled;
                patcher::setting_auto_apply = old_auto_apply;
                patch.selected_union_name = old_choice;
                if (patch.type == patcher::PatchType::Integer) {
                    patch.patch_number.value = old_value;
                }
                // apply_patch may have changed an earlier member of a cumulative patch
                // before discovering a later failure; restore the previous requested state.
                const bool rollback_applied = patcher::apply_patch(patch, old_enabled);
                patch.error_reason.clear();
                patch.last_status = patcher::is_patch_active(patch);
                const auto rollback_error = patch.error_reason;
                const auto old_status = old_enabled
                    ? patcher::PatchStatus::Enabled
                    : patcher::PatchStatus::Disabled;
                const bool rollback_ok = rollback_applied && patch.last_status == old_status;
                if (!rollback_ok) {
                    patch.error_reason = failure + "; rollback failed";
                    if (!rollback_error.empty()) {
                        patch.error_reason += ": " + rollback_error;
                    } else if (!rollback_applied) {
                        patch.error_reason += ": apply operation failed";
                    } else if (patch.last_status == patcher::PatchStatus::Error) {
                        patch.error_reason += ": patch status is error";
                    } else {
                        patch.error_reason += ": patch status did not match the requested state";
                    }
                    return error(res, patch.error_reason);
                }
                patch.error_reason.clear();
                return error(res, failure);
            }
        }

        if (enabled_changed || choice_changed || value_changed) {
            patcher::config_dirty = true;
        }
        add_state(res);
    }

    void Patches::set_auto_apply(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 1, *this, res)) {
            return;
        }
        if (!req.params[0].IsBool()) {
            return error_type(res, "auto_apply", "bool");
        }
        prepare();
        const bool auto_apply = req.params[0].GetBool();
        if (patcher::setting_auto_apply != auto_apply) {
            patcher::setting_auto_apply = auto_apply;
            patcher::config_dirty = true;
        }
        add_state(res);
    }

    void Patches::save(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 0, *this, res)) {
            return;
        }
        prepare();
        if (!patcher::config_save()) {
            return error(res, "Unable to save patch settings.");
        }
        add_state(res);
    }

    void Patches::reload(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 0, *this, res)) {
            return;
        }
        prepare();
        patcher::config_load();
        patcher::reload_local_patches();
        add_state(res);
    }

    void Patches::import_url(Request &req, Response &res) {
        patcher::StateLock lock;
        if (!exact_params(req, 1, *this, res)) {
            return;
        }
        if (!req.params[0].IsString()) {
            return error_type(res, "url", "string");
        }
        if (!cfg::CONFIGURATOR_STANDALONE) {
            return error(res, "Remote patch import is only available in standalone configurator mode.");
        }
        if (avs::game::DLL_NAME.empty()) {
            return error(res, "Remote patch import requires a configured game DLL.");
        }

        const std::string url(req.params[0].GetString(), req.params[0].GetStringLength());
        const auto url_error = validate_patch_url(url);
        if (!url_error.empty()) {
            return error(res, url_error);
        }

        prepare();
        patcher::patch_url = url;
        patcher::url_recent_idx = static_cast<size_t>(-1);
        const bool imported = patcher::import_remote_patches_to_disk();
        bool save_failed = false;
        if (imported) {
            if (std::find(patcher::url_recents.begin(), patcher::url_recents.end(), url)
                    == patcher::url_recents.end()) {
                patcher::url_recents.emplace_back(url);
            }
            save_failed = !patcher::config_save();
        }

        // The downloader clears the in-memory patch list before fetching, even on failure.
        patcher::reload_local_patches(true);
        if (!imported) {
            return error(res, "Unable to import patches from URL.");
        }
        if (save_failed) {
            return error(res, "Patches imported, but unable to save patch settings.");
        }
        add_state(res);
    }
}
