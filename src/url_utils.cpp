#include "url_utils.h"

#include <arpa/inet.h>
#include <glib.h>

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
    return "https://duckduckgo.com/?q=" + escape_query(text);
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

std::string resolve_url_input(const std::string &text) {
    if (starts_with(text, "!yt "))
        return "https://www.youtube.com/results?search_query=" + escape_query(text.substr(4));
    if (starts_with(text, "!gh "))
        return "https://github.com/search?q=" + escape_query(text.substr(4));
    if (starts_with(text, "!w "))
        return "https://en.wikipedia.org/wiki/Special:Search?search=" + escape_query(text.substr(3));
    if (starts_with(text, "!r "))
        return "https://www.reddit.com/search/?q=" + escape_query(text.substr(3));

    return normalize_url(text);
}
