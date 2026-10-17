#include "keypads.h"

#include <chrono>
#include <functional>
#include <thread>

#include <windows.h>

#include "avs/game.h"
#include "external/rapidjson/document.h"
#include "games/iidx/iidx.h"
#include "games/popn/popn.h"
#include "misc/eamuse.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    struct KeypadMapping {
        char character;
        uint16_t state;
    };

    static KeypadMapping KEYPAD_MAPPINGS[] = {
        { '0', 1 << EAM_IO_KEYPAD_0 },
        { '1', 1 << EAM_IO_KEYPAD_1 },
        { '2', 1 << EAM_IO_KEYPAD_2 },
        { '3', 1 << EAM_IO_KEYPAD_3 },
        { '4', 1 << EAM_IO_KEYPAD_4 },
        { '5', 1 << EAM_IO_KEYPAD_5 },
        { '6', 1 << EAM_IO_KEYPAD_6 },
        { '7', 1 << EAM_IO_KEYPAD_7 },
        { '8', 1 << EAM_IO_KEYPAD_8 },
        { '9', 1 << EAM_IO_KEYPAD_9 },
        { 'A', 1 << EAM_IO_KEYPAD_00 },
        { 'D', 1 << EAM_IO_KEYPAD_DECIMAL },
    };

    Keypads::Keypads() : Module("keypads") {
        functions["capabilities"] = std::bind(&Keypads::capabilities, this, _1, _2);
        functions["insert_card"] = std::bind(&Keypads::insert_card, this, _1, _2);
        functions["write"] = std::bind(&Keypads::write, this, _1, _2);
        functions["set"] = std::bind(&Keypads::set, this, _1, _2);
        functions["get"] = std::bind(&Keypads::get, this, _1, _2);
    }

    void Keypads::capabilities(Request &req, Response &res) {
        if (!req.params.Empty()) {
            return error(res, "capabilities takes no parameters.");
        }

        auto &alloc = res.doc()->GetAllocator();
        Value capabilities(kObjectType);
        capabilities.AddMember("model", StringRef(avs::game::MODEL, 3), alloc);
        capabilities.AddMember("keypad_count", eamuse_get_game_keypads(), alloc);
        const bool keys = !(avs::game::is_model("LDJ") && games::iidx::TDJ_MODE)
                && !games::popn::is_pikapika_model();
        capabilities.AddMember("keys", keys, alloc);
        capabilities.AddMember("insert_card", true, alloc);
        res.add_data(capabilities);
    }

    void Keypads::insert_card(Request &req, Response &res) {
        if (req.params.Size() != 1) {
            return error(res, "insert_card expects exactly one parameter.");
        }
        if (!req.params[0].IsUint()) {
            return error_type(res, "keypad", "uint");
        }
        const auto keypad = req.params[0].GetUint();
        if (keypad >= static_cast<unsigned>(eamuse_get_game_keypads())) {
            return error(res, "keypad is unavailable for this model.");
        }

        DWORD sleep_time = 70;
        if (avs::game::is_model("MDX")) {
            sleep_time = 150;
        }
        eamuse_set_keypad_overrides(keypad, 1 << EAM_IO_INSERT);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        eamuse_set_keypad_overrides(keypad, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
    }

    /**
     * write(keypad: uint, input: str)
     */
    void Keypads::write(Request &req, Response &res) {

        // check params
        if (req.params.Size() < 2) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsUint()) {
            return error_type(res, "keypad", "uint");
        }
        if (!req.params[1].IsString()) {
            return error_type(res, "input", "string");
        }

        // get params
        auto keypad = req.params[0].GetUint();
        auto input = std::string(req.params[1].GetString());

        // process all chars
        for (auto c : input) {
            uint16_t state = 0;

            // find mapping
            bool mapping_found = false;
            for (auto &mapping : KEYPAD_MAPPINGS) {
                if (_strnicmp(&mapping.character, &c, 1) == 0) {
                    state |= mapping.state;
                    mapping_found = true;
                    break;
                }
            }

            // check for error
            if (!mapping_found) {
                return error_unknown(res, "char", std::string("") + c);
            }

            /*
             * Write input to keypad.
             * We try to make sure it was accepted by waiting a bit more than two frames.
             */
            DWORD sleep_time = 70;
            if (avs::game::is_model("MDX")) {

                // cuz fuck DDR
                sleep_time = 150;
            }

            // set
            eamuse_set_keypad_overrides(keypad, state);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));

            // unset
            eamuse_set_keypad_overrides(keypad, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }
    }

    /**
     * set(keypad: uint, key: char, ...)
     */
    void Keypads::set(Request &req, Response &res) {

        // check keypad
        if (req.params.Size() < 1) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsUint()) {
            return error_type(res, "keypad", "uint");
        }
        auto keypad = req.params[0].GetUint();

        // iterate params
        uint16_t state = 0;
        auto params = req.params.GetArray();
        for (size_t i = 1; i < params.Size(); i++) {
            auto &param = params[i];

            // check key
            if (!param.IsString()) {
                error_type(res, "key", "char");
            }
            if (param.GetStringLength() < 1) {
                error_size(res, "key", 1);
            }

            // find mapping
            auto key = param.GetString();
            bool mapping_found = false;
            for (auto &mapping : KEYPAD_MAPPINGS) {
                if (_strnicmp(&mapping.character, key, 1) == 0) {
                    state |= mapping.state;
                    mapping_found = true;
                    break;
                }
            }

            // check for error
            if (!mapping_found) {
                return error_unknown(res, "key", key);
            }
        }

        // set keypad state
        eamuse_set_keypad_overrides(keypad, state);
    }

    /**
     * get(keypad: uint)
     */
    void Keypads::get(Request &req, Response &res) {

        // check keypad
        if (req.params.Size() < 1) {
            return error_params_insufficient(res);
        }
        if (!req.params[0].IsUint()) {
            return error_type(res, "keypad", "uint");
        }
        auto keypad = req.params[0].GetUint();

        // get keypad state
        auto state = eamuse_get_keypad_state(keypad);

        // add keys to response
        for (auto &mapping : KEYPAD_MAPPINGS) {
            if (state & mapping.state) {
                Value val(&mapping.character, 1);
                res.add_data(val);
            }
        }
    }
}
