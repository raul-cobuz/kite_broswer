#include "settings.h"

#include <glib/gstdio.h>

#include <cmath>

static GKeyFile *settings = nullptr;
static gchar *settings_file = nullptr;
static GHashTable *permission_decisions = nullptr;
static GHashTable *site_zoom_levels = nullptr;
static bool dark_mode = false;
static gchar *homepage = nullptr;
static gchar *download_directory = nullptr;
static bool restore_session = true;
static bool ask_download_directory = false;

static gchar *default_download_directory() {
    const gchar *special = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (special && *special)
        return g_strdup(special);
    return g_build_filename(g_get_home_dir(), "Downloads", NULL);
}

static gchar *string_setting(const gchar *group, const gchar *key, const gchar *fallback) {
    GError *error = nullptr;
    gchar *value = g_key_file_get_string(settings, group, key, &error);
    if (error || !value || !*value) {
        g_clear_error(&error);
        g_free(value);
        return g_strdup(fallback);
    }
    return value;
}

static double clamp_zoom(double level) {
    if (!(level > 0.0))
        return 1.0;
    return CLAMP(level, 0.25, 5.0);
}

void load_settings() {
    if (permission_decisions)
        g_hash_table_destroy(permission_decisions);
    if (site_zoom_levels)
        g_hash_table_destroy(site_zoom_levels);
    if (settings)
        g_key_file_free(settings);
    g_free(settings_file);
    g_free(homepage);
    g_free(download_directory);

    permission_decisions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    site_zoom_levels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    settings = g_key_file_new();
    settings_file = g_build_filename(g_get_user_config_dir(), "kite", "settings.ini", NULL);
    dark_mode = false;
    homepage = g_strdup("kite://newtab");
    download_directory = default_download_directory();
    restore_session = true;
    ask_download_directory = false;

    GError *error = nullptr;
    if (!g_key_file_load_from_file(settings, settings_file, G_KEY_FILE_NONE, &error)) {
        if (error && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("[Kite] Could not load settings: %s\n", error->message);
        g_clear_error(&error);
    }

    gboolean dark = g_key_file_get_boolean(settings, "appearance", "dark_mode", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        dark_mode = dark;
    }

    gsize n_keys = 0;
    gchar **keys = g_key_file_get_keys(settings, "permissions", &n_keys, nullptr);
    if (keys) {
        for (gsize i = 0; i < n_keys; i++) {
            gchar *value = g_key_file_get_string(settings, "permissions", keys[i], nullptr);
            if (!value)
                continue;
            g_hash_table_replace(permission_decisions, g_strdup(keys[i]),
                                 GINT_TO_POINTER(g_strcmp0(value, "allow") == 0 ? 1 : 0));
            g_free(value);
        }
        g_strfreev(keys);
    }

    g_free(homepage);
    homepage = string_setting("general", "homepage", "kite://newtab");

    g_free(download_directory);
    gchar *default_dir = default_download_directory();
    download_directory = string_setting("downloads", "directory", default_dir);
    g_free(default_dir);

    gboolean restore = g_key_file_get_boolean(settings, "session", "restore", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        restore_session = restore;
    }

    gboolean ask = g_key_file_get_boolean(settings, "downloads", "ask", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        ask_download_directory = ask;
    }

    keys = g_key_file_get_keys(settings, "zoom", &n_keys, nullptr);
    if (keys) {
        for (gsize i = 0; i < n_keys; i++) {
            GError *zoom_error = nullptr;
            double level = g_key_file_get_double(settings, "zoom", keys[i], &zoom_error);
            if (zoom_error) {
                g_clear_error(&zoom_error);
                continue;
            }
            g_hash_table_replace(site_zoom_levels, g_strdup(keys[i]), g_memdup2(&level, sizeof(double)));
        }
        g_strfreev(keys);
    }
}

void save_settings() {
    if (!settings || !settings_file)
        return;

    g_key_file_set_boolean(settings, "appearance", "dark_mode", dark_mode);
    g_key_file_set_string(settings, "general", "homepage", settings_homepage());
    g_key_file_set_boolean(settings, "session", "restore", restore_session);
    g_key_file_set_string(settings, "downloads", "directory", settings_download_directory());
    g_key_file_set_boolean(settings, "downloads", "ask", ask_download_directory);

    g_key_file_remove_group(settings, "permissions", nullptr);

    if (permission_decisions) {
        GHashTableIter iter;
        gpointer key = nullptr;
        gpointer value = nullptr;
        g_hash_table_iter_init(&iter, permission_decisions);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_key_file_set_string(settings, "permissions", (const gchar *)key,
                                  GPOINTER_TO_INT(value) ? "allow" : "deny");
    }

    g_key_file_remove_group(settings, "zoom", nullptr);
    if (site_zoom_levels) {
        GHashTableIter iter;
        gpointer key = nullptr;
        gpointer value = nullptr;
        g_hash_table_iter_init(&iter, site_zoom_levels);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_key_file_set_double(settings, "zoom", (const gchar *)key, *(double *)value);
    }

    gchar *dir = g_path_get_dirname(settings_file);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gsize length = 0;
    gchar *data = g_key_file_to_data(settings, &length, nullptr);
    GError *error = nullptr;
    if (!g_file_set_contents(settings_file, data, (gssize)length, &error)) {
        g_printerr("[Kite] Could not save settings: %s\n", error->message);
        g_clear_error(&error);
    }
    g_free(data);
}

// GKeyFile keys only allow alphanumerics, '-', '_', '.', '|' and ':'. Origins can
// contain characters outside that set (IPv6 brackets, spaces, zone ids), so escape
// them. '_' is escaped too, keeping the mapping unambiguous.
gchar *permission_key(const gchar *type, const gchar *origin) {
    GString *escaped = g_string_new(type);
    g_string_append_c(escaped, '|');
    for (const guchar *p = (const guchar *)origin; *p; p++) {
        guchar c = *p;
        if (g_ascii_isalnum(c) || c == '-' || c == '.' || c == '|' || c == ':')
            g_string_append_c(escaped, (gchar)c);
        else
            g_string_append_printf(escaped, "_%02X", c);
    }
    return g_string_free(escaped, FALSE);
}

gint get_permission_decision(const gchar *type, const gchar *origin) {
    if (!permission_decisions || !type || !origin)
        return -1;

    gchar *key = permission_key(type, origin);
    gpointer value = nullptr;
    gboolean found = g_hash_table_lookup_extended(permission_decisions, key, nullptr, &value);
    g_free(key);
    return found ? (GPOINTER_TO_INT(value) ? 1 : 0) : -1;
}

void remember_permission_decision(const gchar *type, const gchar *origin, gboolean allow) {
    if (!permission_decisions || !type || !origin)
        return;

    gchar *key = permission_key(type, origin);
    g_hash_table_replace(permission_decisions, key, GINT_TO_POINTER(allow ? 1 : 0));
    save_settings();
}

bool settings_dark_mode() {
    return dark_mode;
}

void settings_set_dark_mode(bool enabled) {
    dark_mode = enabled;
}

const gchar *settings_homepage() {
    return homepage ? homepage : "kite://newtab";
}

void settings_set_homepage(const gchar *url) {
    if (!url || !*url)
        return;
    g_free(homepage);
    homepage = g_strdup(url);
}

bool settings_restore_session() {
    return restore_session;
}

void settings_set_restore_session(bool enabled) {
    restore_session = enabled;
}

const gchar *settings_download_directory() {
    return download_directory ? download_directory : "";
}

void settings_set_download_directory(const gchar *path) {
    if (!path || !*path)
        return;
    g_free(download_directory);
    download_directory = g_strdup(path);
}

bool settings_ask_download_directory() {
    return ask_download_directory;
}

void settings_set_ask_download_directory(bool ask) {
    ask_download_directory = ask;
}

double settings_site_zoom(const gchar *host) {
    if (!site_zoom_levels || !host || !*host)
        return 1.0;
    gchar *key = permission_key("zoom", host);
    gpointer value = g_hash_table_lookup(site_zoom_levels, key);
    g_free(key);
    return value ? *(double *)value : 1.0;
}

void settings_set_site_zoom(const gchar *host, double level) {
    if (!site_zoom_levels || !host || !*host)
        return;
    gchar *key = permission_key("zoom", host);
    double *stored = g_new(double, 1);
    *stored = std::round(clamp_zoom(level) * 100.0) / 100.0;
    g_hash_table_replace(site_zoom_levels, key, stored);
}

void settings_clear_site_zoom(const gchar *host) {
    if (!site_zoom_levels || !host || !*host)
        return;
    gchar *key = permission_key("zoom", host);
    g_hash_table_remove(site_zoom_levels, key);
    g_free(key);
}
