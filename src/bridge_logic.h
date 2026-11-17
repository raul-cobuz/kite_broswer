#pragma once

#include <glib.h>

#include <string>

// Extracts a decoded query parameter. Empty when the key is absent.
std::string query_param(const gchar *query, const gchar *name);

// True when a kite:// path is read-only or carries the per-run action token.
// Mutating paths never pass without the token.
bool kite_action_permitted(const std::string &path, const std::string &query, const std::string &token);

enum class ExtensionApiRoute {
    Invalid,
    RuntimeResponse,
    Storage,
    Tabs,
    Notifications,
    RuntimeMessage,
    TabMessage,
    Unknown,
};

// Routes an extension API message by its type, before any extension lookup.
ExtensionApiRoute extension_api_route(const std::string &type);
