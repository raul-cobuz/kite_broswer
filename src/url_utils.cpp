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

static std::string search_url(const std::string &text) {
    return "https://www.google.com/search?q=" + escape_query(text);
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
