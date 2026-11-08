#include "settings.h"

#include <glib/gstdio.h>

static GKeyFile *settings = nullptr;
static gchar *settings_file = nullptr;
static GHashTable *permission_decisions = nullptr;
static bool dark_mode = false;

void load_settings() {
    if (permission_decisions)
        g_hash_table_destroy(permission_decisions);
    if (settings)
        g_key_file_free(settings);
    g_free(settings_file);

    permission_decisions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    settings = g_key_file_new();
    settings_file = g_build_filename(g_get_user_config_dir(), "kite", "settings.ini", NULL);
    dark_mode = false;

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
}

void save_settings() {
    if (!settings || !settings_file)
        return;

    g_key_file_set_boolean(settings, "appearance", "dark_mode", dark_mode);
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
