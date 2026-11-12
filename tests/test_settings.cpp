#include "settings.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

static void expect_str(const gchar *actual, const char *expected, const char *label) {
    if (g_strcmp0(actual, expected) != 0) {
        std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", label, expected, actual ? actual : "(null)");
        failures++;
    }
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-settings-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }
    g_setenv("XDG_CONFIG_HOME", tmp, TRUE);

    load_settings();
    check(!settings_dark_mode(), "dark mode defaults to off");
    check(get_permission_decision("media", "example.com") == -1, "unknown permission decision");
    expect_str(settings_homepage(), "kite://newtab", "homepage defaults to new tab");
    expect_str(settings_search_engine(), "google", "search engine defaults to Google");
    check(settings_google_fallback(), "Google fallback defaults to on");
    check(settings_restore_session(), "session restore defaults to on");
    check(settings_download_directory() && *settings_download_directory(), "download directory has a default");
    check(settings_site_zoom("example.com") == 1.0, "site zoom defaults to 100%");
    check(settings_block_ads(), "ad blocking defaults to on");
    check(settings_strip_tracking_params(), "tracking parameter removal defaults to on");
    check(!settings_clear_on_exit(), "clear on exit defaults to off");
    check(settings_site_adblock_enabled("example.com"), "site ad blocking defaults to on");
    check(settings_newtab_clock(), "start-page clock defaults to on");
    check(!settings_newtab_weather(), "weather widget defaults to off");
    check(!settings_newtab_compact(), "start page defaults to expanded layout");
    expect_str(settings_newtab_background(), "", "start page has no background by default");
    expect_str(settings_newtab_weather_units(), "celsius", "weather units default to Celsius");
    check(settings_newtab_links().empty(), "custom quick links start empty");

    settings_set_dark_mode(true);
    remember_permission_decision("media", "example.com", TRUE);
    remember_permission_decision("geolocation", "fe80::1%eth0", FALSE);

    check(get_permission_decision("media", "example.com") == 1, "allow decision in memory");
    check(get_permission_decision("geolocation", "fe80::1%eth0") == 0, "deny decision in memory");

    gchar *key = permission_key("media", "example.com");
    expect_str(key, "media|example.com", "plain origin key");
    g_free(key);

    key = permission_key("geolocation", "fe80::1%eth0");
    expect_str(key, "geolocation|fe80::1_25eth0", "percent in origin is escaped");
    g_free(key);

    key = permission_key("notifications", "local files");
    expect_str(key, "notifications|local_20files", "space in origin is escaped");
    g_free(key);

    key = permission_key("clipboard", "a_b");
    expect_str(key, "clipboard|a_5Fb", "underscore in origin is escaped");
    g_free(key);

    settings_set_homepage("https://example.org");
    settings_set_search_engine("not-a-search-engine");
    expect_str(settings_search_engine(), "google", "unknown search engine falls back to Google");
    settings_set_search_engine("brave");
    settings_set_google_fallback(false);
    check(!settings_google_fallback(), "Google fallback disabled in memory");
    settings_set_restore_session(false);
    settings_set_download_directory("/tmp/kite-downloads");
    settings_set_site_zoom("example.com", 1.25);
    settings_set_site_zoom("a_b.test", 2.0);
    check(settings_site_zoom("example.com") == 1.25, "site zoom in memory");

    settings_set_block_ads(false);
    settings_set_strip_tracking_params(false);
    settings_set_clear_on_exit(true);
    settings_set_site_adblock_enabled("Example.COM", false);
    settings_set_site_adblock_enabled("news.test", false);
    check(!settings_block_ads(), "ad blocking disabled in memory");
    check(!settings_strip_tracking_params(), "tracking removal disabled in memory");
    check(settings_clear_on_exit(), "clear on exit enabled in memory");
    check(!settings_site_adblock_enabled("example.com"), "host exception disables site blocking");
    check(!settings_site_adblock_enabled("EXAMPLE.com"), "host exception lookup is case-insensitive");
    check(settings_site_adblock_enabled("other.test"), "other sites keep blocking");

    settings_set_newtab_clock(false);
    settings_set_newtab_weather(true);
    settings_set_newtab_weather_city("  Lisbon  ");
    settings_set_newtab_weather_units("fahrenheit");
    settings_set_newtab_background("file:///tmp/kite wallpaper.png");
    settings_set_newtab_compact(true);
    check(!settings_newtab_clock(), "start-page clock disabled in memory");
    check(settings_newtab_weather(), "weather widget enabled in memory");
    expect_str(settings_newtab_weather_city(), "Lisbon", "weather city is trimmed");
    expect_str(settings_newtab_weather_units(), "fahrenheit", "weather units normalized to Fahrenheit");
    expect_str(settings_newtab_background(), "file:///tmp/kite wallpaper.png", "background stored in memory");
    check(settings_newtab_compact(), "compact layout enabled in memory");
    check(settings_newtab_link_add("Docs", "https://example.com/docs"), "custom quick link added");
    check(settings_newtab_link_add("", "https://news.test"), "untitled custom quick link added");
    check(!settings_newtab_link_add("", "https://example.com/docs"), "duplicate quick link rejected");
    check(settings_newtab_link_add("Documentation", "https://example.com/docs"),
          "duplicate quick link title updated");
    check(!settings_newtab_link_remove("https://missing.test"), "missing quick link not removed");
    std::vector<NewTabLink> newtab_links = settings_newtab_links();
    check(newtab_links.size() == 2, "two custom quick links stored");
    if (newtab_links.size() == 2) {
        expect_str(newtab_links[0].title.c_str(), "Documentation", "quick link title updated");
        expect_str(newtab_links[0].url.c_str(), "https://example.com/docs", "quick link URL retained");
        expect_str(newtab_links[1].url.c_str(), "https://news.test", "second quick link retained");
    }
    gchar **exceptions = settings_adblock_exception_hosts();
    check(g_strv_length(exceptions) == 2, "two ad-block exceptions listed");
    expect_str(exceptions[0], "example.com", "exceptions are normalized and sorted");
    expect_str(exceptions[1], "news.test", "second exception listed");
    g_strfreev(exceptions);

    save_settings();

    gchar *settings_path = g_build_filename(tmp, "kite", "settings.ini", NULL);
    check(g_file_test(settings_path, G_FILE_TEST_IS_REGULAR), "settings file written");

    GKeyFile *file = g_key_file_new();
    check(g_key_file_load_from_file(file, settings_path, G_KEY_FILE_NONE, nullptr), "settings file loads");
    check(g_key_file_get_boolean(file, "appearance", "dark_mode", nullptr), "dark mode persisted");

    gchar *value = g_key_file_get_string(file, "permissions", "media|example.com", nullptr);
    expect_str(value, "allow", "allow persisted");
    g_free(value);

    value = g_key_file_get_string(file, "permissions", "geolocation|fe80::1_25eth0", nullptr);
    expect_str(value, "deny", "deny persisted under escaped key");
    g_free(value);

    value = g_key_file_get_string(file, "general", "homepage", nullptr);
    expect_str(value, "https://example.org", "homepage persisted");
    g_free(value);
    value = g_key_file_get_string(file, "search", "engine", nullptr);
    expect_str(value, "brave", "search engine persisted");
    g_free(value);
    check(!g_key_file_get_boolean(file, "search", "google_fallback", nullptr), "fallback preference persisted");
    check(!g_key_file_get_boolean(file, "session", "restore", nullptr), "session restore persisted");
    value = g_key_file_get_string(file, "downloads", "directory", nullptr);
    expect_str(value, "/tmp/kite-downloads", "download directory persisted");
    g_free(value);
    check(!g_key_file_has_key(file, "downloads", "ask", nullptr), "obsolete ask flag is removed");
    check(!g_key_file_get_boolean(file, "privacy", "block_ads", nullptr), "ad blocking persisted");
    check(!g_key_file_get_boolean(file, "privacy", "strip_tracking_params", nullptr),
          "tracking removal persisted");
    check(g_key_file_get_boolean(file, "privacy", "clear_on_exit", nullptr), "clear on exit persisted");
    value = g_key_file_get_string(file, "privacy", "adblock_exceptions", nullptr);
    expect_str(value, "example.com;news.test", "ad-block exceptions persisted");
    g_free(value);
    check(!g_key_file_get_boolean(file, "newtab", "clock", nullptr), "start-page clock persisted");
    check(g_key_file_get_boolean(file, "newtab", "weather", nullptr), "weather widget persisted");
    value = g_key_file_get_string(file, "newtab", "weather_city", nullptr);
    expect_str(value, "Lisbon", "weather city persisted");
    g_free(value);
    value = g_key_file_get_string(file, "newtab", "weather_units", nullptr);
    expect_str(value, "fahrenheit", "weather units persisted");
    g_free(value);
    value = g_key_file_get_string(file, "newtab", "background", nullptr);
    expect_str(value, "file:///tmp/kite wallpaper.png", "start-page background persisted");
    g_free(value);
    check(g_key_file_get_boolean(file, "newtab", "compact", nullptr), "compact layout persisted");
    check(g_key_file_get_integer(file, "newtab", "link_count", nullptr) == 2, "quick link count persisted");
    value = g_key_file_get_string(file, "newtab", "link0_title", nullptr);
    expect_str(value, "Documentation", "quick link title persisted");
    g_free(value);
    value = g_key_file_get_string(file, "newtab", "link0_url", nullptr);
    expect_str(value, "https://example.com/docs", "quick link URL persisted");
    g_free(value);
    check(g_key_file_get_double(file, "zoom", "zoom|example.com", nullptr) == 1.25, "site zoom persisted");
    check(g_key_file_get_double(file, "zoom", "zoom|a_5Fb.test", nullptr) == 2.0,
          "site zoom key escapes host");
    g_key_file_free(file);

    // Reload from disk into a fresh in-memory state.
    load_settings();
    check(settings_dark_mode(), "dark mode restored");
    check(get_permission_decision("media", "example.com") == 1, "allow restored");
    check(get_permission_decision("geolocation", "fe80::1%eth0") == 0, "deny restored");
    expect_str(settings_homepage(), "https://example.org", "homepage restored");
    expect_str(settings_search_engine(), "brave", "search engine restored");
    check(!settings_google_fallback(), "fallback preference restored");
    check(!settings_restore_session(), "session restore restored");
    expect_str(settings_download_directory(), "/tmp/kite-downloads", "download directory restored");
    check(settings_site_zoom("example.com") == 1.25, "site zoom restored");
    check(settings_site_zoom("a_b.test") == 2.0, "escaped site zoom restored");
    check(!settings_block_ads(), "ad blocking restored");
    check(!settings_strip_tracking_params(), "tracking removal restored");
    check(settings_clear_on_exit(), "clear on exit restored");
    check(!settings_site_adblock_enabled("example.com"), "ad-block exception restored");
    check(!settings_newtab_clock(), "start-page clock restored");
    check(settings_newtab_weather(), "weather widget restored");
    expect_str(settings_newtab_weather_city(), "Lisbon", "weather city restored");
    expect_str(settings_newtab_weather_units(), "fahrenheit", "weather units restored");
    expect_str(settings_newtab_background(), "file:///tmp/kite wallpaper.png", "background restored");
    check(settings_newtab_compact(), "compact layout restored");
    newtab_links = settings_newtab_links();
    check(newtab_links.size() == 2, "quick links restored");
    if (newtab_links.size() == 2) {
        expect_str(newtab_links[0].title.c_str(), "Documentation", "quick link title restored");
        expect_str(newtab_links[1].url.c_str(), "https://news.test", "quick link URL restored");
    }
    exceptions = settings_adblock_exception_hosts();
    check(g_strv_length(exceptions) == 2, "restored exception count");
    g_strfreev(exceptions);

    settings_set_site_adblock_enabled("example.com", true);
    settings_set_site_adblock_enabled("news.test", true);
    save_settings();
    load_settings();
    exceptions = settings_adblock_exception_hosts();
    check(g_strv_length(exceptions) == 0, "enabling sites clears exceptions");
    g_strfreev(exceptions);
    file = g_key_file_new();
    g_key_file_load_from_file(file, settings_path, G_KEY_FILE_NONE, nullptr);
    check(!g_key_file_has_key(file, "privacy", "adblock_exceptions", nullptr),
          "empty exception list removes key");
    g_key_file_free(file);

    settings_clear_site_zoom("example.com");
    save_settings();
    load_settings();
    check(settings_site_zoom("example.com") == 1.0, "cleared site zoom defaults to 100%");

    std::vector<NewTabLink> many_links;
    for (int i = 0; i < KITE_MAX_NEWTAB_LINKS + 5; i++) {
        NewTabLink link;
        link.title = "Link " + std::to_string(i);
        link.url = "https://example.test/" + std::to_string(i);
        many_links.push_back(link);
    }
    settings_set_newtab_links(many_links);
    check((int)settings_newtab_links().size() == KITE_MAX_NEWTAB_LINKS, "quick links capped at the maximum");
    check(!settings_newtab_link_add("Overflow", "https://overflow.test"), "quick link limit enforced");
    check(settings_newtab_link_remove("https://example.test/0"), "quick link can be removed");
    check(settings_newtab_link_add("Overflow", "https://overflow.test"), "removed slot can be reused");

    g_remove(settings_path);
    gchar *kite_dir = g_path_get_dirname(settings_path);
    g_rmdir(kite_dir);
    g_rmdir(tmp);
    g_free(kite_dir);
    g_free(settings_path);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d settings test(s) failed\n", failures);
        return 1;
    }
    std::printf("settings tests passed\n");
    return 0;
}
