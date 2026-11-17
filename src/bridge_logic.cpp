#include "bridge_logic.h"

std::string query_param(const gchar *query, const gchar *name) {
    if (!query || !*query || !name)
        return "";
    GHashTable *params = g_uri_parse_params(query, -1, "&", G_URI_PARAMS_WWW_FORM, nullptr);
    if (!params)
        return "";
    const gchar *value = (const gchar *)g_hash_table_lookup(params, name);
    std::string result = value ? value : "";
    g_hash_table_destroy(params);
    return result;
}

bool kite_action_permitted(const std::string &path, const std::string &query, const std::string &token) {
    if (path.empty() || path == "/")
        return true;
    if (token.empty())
        return false;
    std::string supplied = query_param(query.c_str(), "k");
    return !supplied.empty() && supplied == token;
}

ExtensionApiRoute extension_api_route(const std::string &type) {
    if (type.empty())
        return ExtensionApiRoute::Invalid;
    if (type == "runtime.response")
        return ExtensionApiRoute::RuntimeResponse;
    if (type == "storage.get" || type == "storage.set" || type == "storage.remove" || type == "storage.clear")
        return ExtensionApiRoute::Storage;
    if (type.rfind("tabs.", 0) == 0 && type != "tabs.sendMessage")
        return ExtensionApiRoute::Tabs;
    if (type.rfind("notifications.", 0) == 0)
        return ExtensionApiRoute::Notifications;
    if (type == "runtime.sendMessage")
        return ExtensionApiRoute::RuntimeMessage;
    if (type == "tabs.sendMessage")
        return ExtensionApiRoute::TabMessage;
    return ExtensionApiRoute::Unknown;
}
