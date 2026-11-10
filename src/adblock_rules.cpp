#include "adblock_rules.h"

#include <glib.h>

#include <set>

static bool valid_host_char(gchar c) {
    return g_ascii_isalnum(c) || c == '.' || c == '-' || c == ':' || c == '_';
}

static bool valid_host(const std::string &host) {
    if (host.empty())
        return false;
    for (gchar c : host) {
        if (!valid_host_char(c))
            return false;
    }
    return true;
}

std::string adblock_exception_rules_json(const std::vector<std::string> &hosts) {
    std::set<std::string> unique;
    for (const std::string &host : hosts) {
        gchar *lower = g_ascii_strdown(host.c_str(), -1);
        std::string normalized = lower ? lower : "";
        g_free(lower);
        if (valid_host(normalized))
            unique.insert(normalized);
    }

    std::string json = "[";
    bool first = true;
    for (const std::string &host : unique) {
        if (!first)
            json += ",";
        first = false;
        json += "{\"trigger\":{\"url-filter\":\".*\",\"if-domain\":[\"";
        json += host;
        json += "\"]},\"action\":{\"type\":\"ignore-previous-rules\"}}";
    }
    json += "]";
    return json;
}
