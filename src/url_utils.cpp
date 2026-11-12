#include "url_utils.h"

#include <arpa/inet.h>
#include <glib.h>

#include <vector>

#include "settings.h"

static bool starts_with(const std::string &text, const gchar *prefix) {
    return text.rfind(prefix, 0) == 0;
}

static bool has_known_scheme(const std::string &text) {
    return starts_with(text, "http://") || starts_with(text, "https://") || starts_with(text, "file://") ||
           starts_with(text, "about:") || starts_with(text, "view-source:") || starts_with(text, "kite://");
}

static bool is_local_host(const std::string &text) {
    if (starts_with(text, "localhost")) {
        if (text.size() == 9 || text[9] == ':' || text[9] == '/')
            return true;
    }
    if (!text.empty() && text[0] == '[')
        return true; // bracketed IPv6

    std::string host = text.substr(0, text.find_first_of(":/"));
    struct in_addr addr;
    return inet_pton(AF_INET, host.c_str(), &addr) == 1;
}

static std::string escape_query(const std::string &text) {
    gchar *escaped = g_uri_escape_string(text.c_str(), NULL, TRUE);
    std::string result = escaped ? escaped : "";
    g_free(escaped);
    return result;
}

static std::string lower_ascii(const std::string &text) {
    gchar *lower = g_ascii_strdown(text.c_str(), -1);
    std::string result = lower ? lower : "";
    g_free(lower);
    return result;
}

std::string google_search_url(const std::string &text) {
    return "https://www.google.com/search?q=" + escape_query(text);
}

std::string search_url_for_engine(const std::string &text, const gchar *engine) {
    std::string name = lower_ascii(engine ? engine : "");
    std::string query = escape_query(text);
    if (name == "duckduckgo")
        return "https://html.duckduckgo.com/html/?q=" + query;
    if (name == "startpage")
        return "https://www.startpage.com/sp/search?query=" + query;
    if (name == "brave")
        return "https://search.brave.com/search?q=" + query;
    return google_search_url(text);
}

std::string search_url(const std::string &text) {
    return search_url_for_engine(text, settings_search_engine());
}

const gchar *search_engine_display_name(const gchar *engine) {
    std::string name = lower_ascii(engine ? engine : "");
    if (name == "duckduckgo")
        return "DuckDuckGo";
    if (name == "startpage")
        return "Startpage";
    if (name == "brave")
        return "Brave";
    return "Google";
}

static int hex_value(gchar c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static std::string decode_form_value(const std::string &value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '+') {
            result.push_back(' ');
            continue;
        }
        if (value[i] == '%' && i + 2 < value.size()) {
            int high = hex_value(value[i + 1]);
            int low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back((gchar)((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(value[i]);
    }
    return result;
}

static std::string query_value(const gchar *query, const gchar *name) {
    if (!query || !name)
        return "";
    std::string raw = query;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t amp = raw.find('&', start);
        std::string pair = raw.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t equals = pair.find('=');
        std::string key = decode_form_value(equals == std::string::npos ? pair : pair.substr(0, equals));
        if (key == name)
            return decode_form_value(equals == std::string::npos ? "" : pair.substr(equals + 1));
        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }
    return "";
}

static bool is_google_domain(const std::string &host) {
    if (host == "google.com" || g_str_has_suffix(host.c_str(), ".google.com"))
        return true;

    size_t first_dot = host.find('.');
    if (first_dot == std::string::npos)
        return false;
    std::string first = host.substr(0, first_dot);
    if (first == "google")
        return true;
    if (first != "www")
        return false;
    size_t second_dot = host.find('.', first_dot + 1);
    return second_dot != std::string::npos && host.substr(first_dot + 1, second_dot - first_dot - 1) == "google";
}

bool is_google_challenge_url(const std::string &url) {
    GError *error = nullptr;
    GUri *parsed = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        return false;
    }
    const gchar *host = g_uri_get_host(parsed);
    const gchar *path = g_uri_get_path(parsed);
    bool challenge = host && path && is_google_domain(host) && g_str_has_prefix(path, "/sorry");
    g_uri_unref(parsed);
    return challenge;
}

std::string google_challenge_query(const std::string &url) {
    GError *error = nullptr;
    GUri *parsed = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        return "";
    }
    std::string continue_url = query_value(g_uri_get_query(parsed), "continue");
    g_uri_unref(parsed);
    if (continue_url.empty())
        return "";

    GUri *search = g_uri_parse(continue_url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!search) {
        g_clear_error(&error);
        return "";
    }
    std::string query = query_value(g_uri_get_query(search), "q");
    g_uri_unref(search);
    return query;
}

std::string normalize_url(const std::string &text) {
    if (text.empty())
        return text;
    if (has_known_scheme(text))
        return text;

    if (text[0] == '/' || starts_with(text, "~/") || starts_with(text, "./") || starts_with(text, "../")) {
        std::string candidate = text;
        if (starts_with(text, "~/"))
            candidate = std::string(g_get_home_dir()) + text.substr(1);

        gchar *canonical = g_canonicalize_filename(candidate.c_str(), NULL);
        if (g_file_test(canonical, G_FILE_TEST_EXISTS)) {
            gchar *uri = g_filename_to_uri(canonical, NULL, NULL);
            std::string result = uri ? uri : text;
            g_free(uri);
            g_free(canonical);
            return result;
        }
        g_free(canonical);
    }

    if (is_local_host(text))
        return "http://" + text;

    if (text.find(' ') == std::string::npos && text.find('.') != std::string::npos)
        return "https://" + text;

    return search_url(text);
}

std::string url_host(const std::string &url) {
    if (url.empty())
        return "";

    GError *error = nullptr;
    GUri *parsed = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        g_clear_error(&error);
        return "";
    }

    const gchar *host = g_uri_get_host(parsed);
    std::string result;
    if (host && *host) {
        gchar *lower = g_ascii_strdown(host, -1);
        result = lower;
        g_free(lower);
    }
    g_uri_unref(parsed);
    return result;
}

static bool is_tracking_param(const std::string &name) {
    std::string lower;
    lower.reserve(name.size());
    for (guchar c : name)
        lower.push_back((gchar)g_ascii_tolower(c));

    if (lower.rfind("utm_", 0) == 0 || lower.rfind("hsa_", 0) == 0 || lower.rfind("mtm_", 0) == 0)
        return true;

    static const char *const names[] = {
        "fbclid",       "gclid",         "dclid",        "gclsrc",       "wbraid",       "gbraid",
        "gad_source",   "msclkid",       "mc_cid",       "mc_eid",       "mkt_tok",      "igshid",
        "twclid",       "ttclid",        "yclid",        "srsltid",      "_hsenc",       "_hsmi",
        "__hssc",       "__hstc",        "__hsfp",       "oly_anon_id",  "oly_enc_id",   "vero_id",
        "vero_conv",    "wickedid",      "_ga",          "_gl",          "ref_src",      "ref_url",
        "__twitter_impression",          "guccounter",   "guce_referrer", "guce_referrer_sig",
        "spm",          "openstat",      "_openstat",    "pk_campaign",  "pk_kwd",       "pk_source",
        "pk_medium",
    };
    for (const char *candidate : names) {
        if (lower == candidate)
            return true;
    }
    return false;
}

std::string strip_tracking_params(const std::string &url) {
    if (!starts_with(url, "http://") && !starts_with(url, "https://"))
        return url;

    size_t query_pos = url.find('?');
    if (query_pos == std::string::npos)
        return url;

    size_t fragment_pos = url.find('#', query_pos);
    size_t query_end = fragment_pos == std::string::npos ? url.size() : fragment_pos;
    std::string query = url.substr(query_pos + 1, query_end - query_pos - 1);
    if (query.empty())
        return url;

    std::vector<std::string> kept;
    bool removed = false;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        size_t end = amp == std::string::npos ? query.size() : amp;
        std::string pair = query.substr(start, end - start);
        size_t equals = pair.find('=');
        std::string name = equals == std::string::npos ? pair : pair.substr(0, equals);

        gchar *decoded = g_uri_unescape_string(name.c_str(), nullptr);
        std::string normalized = decoded ? decoded : name;
        g_free(decoded);

        if (is_tracking_param(normalized))
            removed = true;
        else
            kept.push_back(pair);

        if (amp == std::string::npos)
            break;
        start = amp + 1;
    }

    if (!removed)
        return url;

    std::string result = url.substr(0, query_pos);
    if (!kept.empty()) {
        result.push_back('?');
        for (size_t i = 0; i < kept.size(); i++) {
            if (i > 0)
                result.push_back('&');
            result += kept[i];
        }
    }
    if (fragment_pos != std::string::npos)
        result += url.substr(fragment_pos);
    return result;
}

std::string resolve_url_input(const std::string &text) {
    if (starts_with(text, "!yt "))
        return "https://www.youtube.com/results?search_query=" + escape_query(text.substr(4));
    if (starts_with(text, "!gh "))
        return "https://github.com/search?q=" + escape_query(text.substr(4));
    if (starts_with(text, "!w "))
        return "https://en.wikipedia.org/wiki/Special:Search?search=" + escape_query(text.substr(3));
    if (starts_with(text, "!r "))
        return "https://www.reddit.com/search/?q=" + escape_query(text.substr(3));

    std::string resolved = normalize_url(text);
    if (settings_strip_tracking_params())
        resolved = strip_tracking_params(resolved);
    return resolved;
}
