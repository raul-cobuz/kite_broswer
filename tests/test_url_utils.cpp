#include "url_utils.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <cstdio>
#include <string>

static int failures = 0;

static void expect_eq(const std::string &actual, const std::string &expected, const char *label) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", label, expected.c_str(), actual.c_str());
        failures++;
    }
}

static void expect_true(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

static std::string duckduckgo_for(const std::string &text) {
    gchar *escaped = g_uri_escape_string(text.c_str(), NULL, TRUE);
    std::string url = std::string("https://duckduckgo.com/?q=") + escaped;
    g_free(escaped);
    return url;
}

int main() {
    expect_eq(normalize_url(""), "", "empty input");
    expect_eq(normalize_url("https://example.com"), "https://example.com", "https kept");
    expect_eq(normalize_url("http://example.com/a?b=1"), "http://example.com/a?b=1", "http kept");
    expect_eq(normalize_url("about:blank"), "about:blank", "about scheme kept");
    expect_eq(normalize_url("view-source:https://example.com"), "view-source:https://example.com", "view-source kept");

    expect_eq(normalize_url("localhost"), "http://localhost", "localhost");
    expect_eq(normalize_url("localhost:8080"), "http://localhost:8080", "localhost with port");
    expect_eq(normalize_url("localhost/path"), "http://localhost/path", "localhost with path");
    expect_eq(normalize_url("localhost.example.com"), "https://localhost.example.com", "localhost-prefixed domain");
    expect_eq(normalize_url("127.0.0.1"), "http://127.0.0.1", "IPv4");
    expect_eq(normalize_url("127.0.0.1:3000/path"), "http://127.0.0.1:3000/path", "IPv4 with port");
    expect_eq(normalize_url("192.168.1.1/admin"), "http://192.168.1.1/admin", "private IPv4 with path");
    expect_eq(normalize_url("[::1]"), "http://[::1]", "bracketed IPv6");
    expect_eq(normalize_url("[::1]:8080"), "http://[::1]:8080", "bracketed IPv6 with port");

    expect_eq(normalize_url("example.com"), "https://example.com", "domain");
    expect_eq(normalize_url("example.com/path?q=1"), "https://example.com/path?q=1", "domain with path");
    expect_eq(normalize_url("hello world"), duckduckgo_for("hello world"), "spaces search");
    expect_eq(normalize_url("hello"), duckduckgo_for("hello"), "plain word search");
    expect_eq(normalize_url("::1"), duckduckgo_for("::1"), "unbracketed IPv6 searches");

    GError *error = nullptr;
    gchar *path = nullptr;
    gint fd = g_file_open_tmp("kite url XXXXXX", &path, &error);
    expect_true(fd >= 0, "create temp file with spaces");
    if (fd >= 0) {
        close(fd);
        gchar *canonical = g_canonicalize_filename(path, NULL);
        gchar *expected_uri = g_filename_to_uri(canonical, NULL, NULL);
        expect_eq(normalize_url(path), expected_uri ? expected_uri : "", "existing path becomes file URI");
        g_free(expected_uri);
        g_free(canonical);
        g_remove(path);
    } else if (error) {
        std::fprintf(stderr, "temp file error: %s\n", error->message);
        g_error_free(error);
    }
    g_free(path);

    const std::string missing = "/definitely/not/a/real/kite/file";
    expect_eq(normalize_url(missing), duckduckgo_for(missing), "missing absolute path searches");

    expect_eq(resolve_url_input("!yt kitten videos"), "https://www.youtube.com/results?search_query=kitten%20videos",
              "YouTube bang");
    expect_eq(resolve_url_input("!gh gtk+"), "https://github.com/search?q=gtk%2B", "GitHub bang");
    expect_eq(resolve_url_input("!w Kite Browser"),
              "https://en.wikipedia.org/wiki/Special:Search?search=Kite%20Browser", "Wikipedia bang");
    expect_eq(resolve_url_input("!r linux"), "https://www.reddit.com/search/?q=linux", "Reddit bang");
    expect_eq(resolve_url_input("example.com"), "https://example.com", "regular input uses normalization");

    if (failures) {
        std::fprintf(stderr, "%d URL test(s) failed\n", failures);
        return 1;
    }
    std::printf("url_utils tests passed\n");
    return 0;
}
