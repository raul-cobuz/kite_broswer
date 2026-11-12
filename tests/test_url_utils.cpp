#include "url_utils.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "settings.h"

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

static std::string google_for(const std::string &text) {
    gchar *escaped = g_uri_escape_string(text.c_str(), NULL, TRUE);
    std::string url = std::string("https://www.google.com/search?q=") + escaped;
    g_free(escaped);
    return url;
}

int main() {
    expect_eq(normalize_url(""), "", "empty input");
    expect_eq(normalize_url("https://example.com"), "https://example.com", "https kept");
    expect_eq(normalize_url("http://example.com/a?b=1"), "http://example.com/a?b=1", "http kept");
    expect_eq(normalize_url("about:blank"), "about:blank", "about scheme kept");
    expect_eq(normalize_url("view-source:https://example.com"), "view-source:https://example.com", "view-source kept");
    expect_eq(normalize_url("kite://newtab"), "kite://newtab", "kite scheme kept");
    expect_eq(normalize_url("kite://history"), "kite://history", "kite history kept");

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
    expect_eq(normalize_url("hello world"), google_for("hello world"), "spaces search");
    expect_eq(normalize_url("hello"), google_for("hello"), "plain word search");
    expect_eq(normalize_url("::1"), google_for("::1"), "unbracketed IPv6 searches");

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
    expect_eq(normalize_url(missing), google_for(missing), "missing absolute path searches");

    expect_eq(resolve_url_input("!yt kitten videos"), "https://www.youtube.com/results?search_query=kitten%20videos",
              "YouTube bang");
    expect_eq(resolve_url_input("!gh gtk+"), "https://github.com/search?q=gtk%2B", "GitHub bang");
    expect_eq(resolve_url_input("!w Kite Browser"),
              "https://en.wikipedia.org/wiki/Special:Search?search=Kite%20Browser", "Wikipedia bang");
    expect_eq(resolve_url_input("!r linux"), "https://www.reddit.com/search/?q=linux", "Reddit bang");
    expect_eq(resolve_url_input("example.com"), "https://example.com", "regular input uses normalization");

    expect_eq(url_host("https://Example.COM/path"), "example.com", "host is lowercased");
    expect_eq(url_host("http://127.0.0.1:8080/admin"), "127.0.0.1", "IPv4 host");
    expect_eq(url_host("https://[::1]:8443/"), "::1", "IPv6 host");
    expect_eq(url_host("file:///tmp/page.html"), "", "file URL has no host");
    expect_eq(url_host("not a url"), "", "invalid URL has no host");
    expect_eq(url_host(""), "", "empty URL has no host");

    expect_eq(strip_tracking_params("https://x.test/a?utm_source=news&b=2&fbclid=abc#frag"),
              "https://x.test/a?b=2#frag", "tracking params removed, fragment kept");
    expect_eq(strip_tracking_params("https://x.test/a?utm_source=news"), "https://x.test/a",
              "only tracking params drops query");
    expect_eq(strip_tracking_params("https://x.test/a?b=2"), "https://x.test/a?b=2", "normal params kept");
    expect_eq(strip_tracking_params("https://x.test/a#utm_source=news"), "https://x.test/a#utm_source=news",
              "param in fragment untouched");
    expect_eq(strip_tracking_params("file:///tmp/a?utm_source=news"), "file:///tmp/a?utm_source=news",
              "non-http URLs untouched");
    expect_eq(strip_tracking_params("https://x.test/a?UTM_Source=news&a=1"), "https://x.test/a?a=1",
              "tracking names are case-insensitive");
    expect_eq(strip_tracking_params("https://x.test/a?q=a%26b&gclid=1"), "https://x.test/a?q=a%26b",
              "encoded values survive");
    expect_eq(strip_tracking_params("https://x.test/a?utm_source=one&utm_medium=two"), "https://x.test/a",
              "multiple tracking params removed");
    expect_eq(strip_tracking_params("https://x.test/a?si=keepme"), "https://x.test/a?si=keepme",
              "generic params kept");
    expect_eq(resolve_url_input("https://x.test/?utm_source=news&q=1"), "https://x.test/?q=1",
              "typed URL is cleaned");
    expect_eq(resolve_url_input("x.test/path?fbclid=abc"), "https://x.test/path", "typed bare URL is cleaned");

    expect_eq(search_url_for_engine("hello world", "duckduckgo"),
              "https://html.duckduckgo.com/html/?q=hello%20world", "DuckDuckGo search URL");
    expect_eq(search_url_for_engine("hello world", "startpage"),
              "https://www.startpage.com/sp/search?query=hello%20world", "Startpage search URL");
    expect_eq(search_url_for_engine("hello world", "brave"), "https://search.brave.com/search?q=hello%20world",
              "Brave search URL");
    settings_set_search_engine("duckduckgo");
    expect_eq(normalize_url("hello world"), "https://html.duckduckgo.com/html/?q=hello%20world",
              "configured engine is used for searches");
    settings_set_search_engine("google");
    expect_eq(normalize_url("hello world"), google_for("hello world"), "Google is the default engine");

    const std::string challenge =
        "https://www.google.com/sorry/index?continue=https://www.google.com/search%3Fq%3Dhpair%25201%2520handler"
        "%25209908%26sei%3Dabc&q=EgS8G4Hw";
    expect_true(is_google_challenge_url(challenge), "Google challenge URL detected");
    expect_eq(google_challenge_query(challenge), "hpair 1 handler 9908", "challenge query recovered");
    expect_true(is_google_challenge_url("https://www.google.co.uk/sorry/index?continue=x"),
                "Google country domain challenge detected");
    expect_true(!is_google_challenge_url("https://www.google.com/search?q=hello"), "normal Google search is not a challenge");
    expect_true(!is_google_challenge_url("https://example.com/sorry/index?continue=x"), "other domains are not challenges");
    expect_eq(google_challenge_query("https://www.google.com/sorry/index?continue=https%3A%2F%2Fwww.google.com"
                                     "%2Fsearch%3Fq%3Dcats%2Band%2Bdogs"),
              "cats and dogs", "plus-encoded challenge query recovered");

    if (failures) {
        std::fprintf(stderr, "%d URL test(s) failed\n", failures);
        return 1;
    }
    std::printf("url_utils tests passed\n");
    return 0;
}
