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
    check(settings_restore_session(), "session restore defaults to on");
    check(settings_download_directory() && *settings_download_directory(), "download directory has a default");
    check(!settings_ask_download_directory(), "download destination defaults to automatic");
    check(settings_site_zoom("example.com") == 1.0, "site zoom defaults to 100%");

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
    settings_set_restore_session(false);
    settings_set_download_directory("/tmp/kite-downloads");
    settings_set_ask_download_directory(true);
    settings_set_site_zoom("example.com", 1.25);
    settings_set_site_zoom("a_b.test", 2.0);
    check(settings_site_zoom("example.com") == 1.25, "site zoom in memory");
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
    check(!g_key_file_get_boolean(file, "session", "restore", nullptr), "session restore persisted");
    value = g_key_file_get_string(file, "downloads", "directory", nullptr);
    expect_str(value, "/tmp/kite-downloads", "download directory persisted");
    g_free(value);
    check(g_key_file_get_boolean(file, "downloads", "ask", nullptr), "ask flag persisted");
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
    check(!settings_restore_session(), "session restore restored");
    expect_str(settings_download_directory(), "/tmp/kite-downloads", "download directory restored");
    check(settings_ask_download_directory(), "ask flag restored");
    check(settings_site_zoom("example.com") == 1.25, "site zoom restored");
    check(settings_site_zoom("a_b.test") == 2.0, "escaped site zoom restored");

    settings_clear_site_zoom("example.com");
    save_settings();
    load_settings();
    check(settings_site_zoom("example.com") == 1.0, "cleared site zoom defaults to 100%");

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
