#include "bridge_logic.h"

#include <cstdio>

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

int main() {
    check(query_param("k=abc&x=1", "k") == "abc", "query parameter found");
    check(query_param("k=a%20b", "k") == "a b", "query parameter decoded");
    check(query_param("x=1", "k").empty(), "missing query parameter is empty");
    check(query_param(nullptr, "k").empty(), "missing query is empty");

    check(kite_action_permitted("", "", ""), "empty path is read-only");
    check(kite_action_permitted("/", "", ""), "root path is read-only");
    check(!kite_action_permitted("/clear", "", "secret"), "mutation without a token is refused");
    check(!kite_action_permitted("/clear", "k=wrong", "secret"), "mutation with a wrong token is refused");
    check(kite_action_permitted("/clear", "k=secret", "secret"), "mutation with the token is allowed");
    check(!kite_action_permitted("/clear", "k=secret", ""), "mutation without a configured token is refused");

    check(extension_api_route("") == ExtensionApiRoute::Invalid, "empty type is invalid");
    check(extension_api_route("runtime.response") == ExtensionApiRoute::RuntimeResponse, "runtime response route");
    check(extension_api_route("storage.get") == ExtensionApiRoute::Storage, "storage route");
    check(extension_api_route("tabs.query") == ExtensionApiRoute::Tabs, "tabs route");
    check(extension_api_route("notifications.create") == ExtensionApiRoute::Notifications, "notifications route");
    check(extension_api_route("runtime.sendMessage") == ExtensionApiRoute::RuntimeMessage, "runtime send route");
    check(extension_api_route("tabs.sendMessage") == ExtensionApiRoute::TabMessage, "tab send route");
    check(extension_api_route("bogus.method") == ExtensionApiRoute::Unknown, "unknown route");

    if (failures) {
        std::fprintf(stderr, "%d bridge logic test(s) failed\n", failures);
        return 1;
    }
    std::printf("bridge logic tests passed\n");
    return 0;
}
