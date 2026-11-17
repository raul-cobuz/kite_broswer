#pragma once

#include <glib.h>

#include <string>

std::string normalize_url(const std::string &text);
std::string resolve_url_input(const std::string &text);
std::string url_host(const std::string &url);
// scheme://host:port with the scheme's default port filled in, e.g.
// "https://example.com:443". Empty when the URL has no host.
std::string url_origin(const std::string &url);
std::string strip_tracking_params(const std::string &url);

// Lowercases ASCII text for case-insensitive matching.
std::string lower_ascii(const std::string &text);

// Search URLs use the configured engine by default. Explicit engine names are
// useful for the Google-blocked fallback and for tests.
std::string search_url(const std::string &text);
std::string search_url_for_engine(const std::string &text, const gchar *engine);
std::string google_search_url(const std::string &text);
const gchar *search_engine_display_name(const gchar *engine);

// Google redirects suspicious searches to /sorry/. These helpers detect that
// page and recover the original query from its "continue" parameter.
bool is_google_challenge_url(const std::string &url);
std::string google_challenge_query(const std::string &url);
