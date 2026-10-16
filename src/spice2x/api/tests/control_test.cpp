#include "control_portable.h"
#include "api/modules/control.h"
#include <cassert>
#include <iostream>

using namespace rapidjson;
static Document call(const std::string &function) {
    Document doc;
    const auto text = "{\"id\":1,\"module\":\"control\",\"function\":\"" + function + "\",\"params\":[]}";
    doc.Parse(text.c_str());
    api::Request request(doc); api::Response response(1); api::modules::Control control;
    control.handle(request, response);
    Document result; result.Parse(response.get_string().c_str()); return result;
}
int main() {
    auto result = call("shutdown");
    assert(result["errors"].Empty());
#if SPICE_XP
    assert(exit_flags == (EWX_POWEROFF | EWX_FORCE));
#else
    assert(exit_flags == (EWX_SHUTDOWN | EWX_HYBRID_SHUTDOWN | EWX_FORCE));
#endif
    assert(stop_calls == 1); assert(open_calls == close_calls);
    result = call("reboot"); assert(result["errors"].Empty()); assert(exit_flags == (EWX_REBOOT | EWX_FORCE));
    const auto exits = exit_calls; const auto stops = stop_calls;
    adjust_error = ERROR_NOT_ALL_ASSIGNED;
    result = call("shutdown"); assert(!result["errors"].Empty());
    assert(std::string(result["errors"][0].GetString()).find("1300") != std::string::npos);
    assert(exit_calls == exits); assert(stop_calls == stops); assert(open_calls == close_calls);
    adjust_error = 0; lookup_ok = false;
    result = call("shutdown"); assert(!result["errors"].Empty()); assert(GetLastError() == 1313);
    assert(exit_calls == exits); assert(open_calls == close_calls);
    lookup_ok = true; open_ok = false;
    result = call("reboot"); assert(!result["errors"].Empty()); assert(GetLastError() == ERROR_ACCESS_DENIED);
    open_ok = true; exit_ok = false;
    result = call("shutdown"); assert(!result["errors"].Empty()); assert(stop_calls == stops);
    assert(std::string(result["errors"][0].GetString()).find("87") != std::string::npos);
    result = call("exit"); assert(result["errors"].Empty()); assert(stop_calls == stops + 1);
    std::cout << "Native control tests passed (SPICE_XP=" << SPICE_XP
              << "): platform shutdown policy, privilege failures, token cleanup, Windows errors, exit.\n";
}
