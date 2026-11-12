#include "session.h"

#include <glib/gstdio.h>

#include <algorithm>

#include "profile.h"

static gchar *session_path() {
    return g_build_filename(profile_config_dir(), "session.ini", nullptr);
}

bool session_load(SessionState *state) {
    if (!state)
        return false;
    state->urls.clear();
    state->pinned.clear();
    state->active_index = 0;

    gchar *path = session_path();
    GKeyFile *file = g_key_file_new();
    GError *error = nullptr;
    bool loaded = g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &error);
    if (!loaded) {
        g_clear_error(&error);
        g_key_file_free(file);
        g_free(path);
        return false;
    }

    gint count = g_key_file_get_integer(file, "session", "count", &error);
    if (error || count <= 0) {
        g_clear_error(&error);
        g_key_file_free(file);
        g_free(path);
        return false;
    }
    count = std::min(count, 100);

    for (gint i = 0; i < count; i++) {
        gchar *key = g_strdup_printf("url%d", i);
        gchar *url = g_key_file_get_string(file, "session", key, nullptr);
        g_free(key);
        if (url && *url)
            state->urls.push_back(url);
        g_free(url);

        gchar *pinned_key = g_strdup_printf("pinned%d", i);
        state->pinned.push_back(g_key_file_get_boolean(file, "session", pinned_key, nullptr) != FALSE);
        g_free(pinned_key);
    }
    while (state->pinned.size() < state->urls.size())
        state->pinned.push_back(false);

    if (state->urls.empty()) {
        g_key_file_free(file);
        g_free(path);
        return false;
    }

    gint active = g_key_file_get_integer(file, "session", "active", &error);
    if (error) {
        g_clear_error(&error);
        active = 0;
    }
    state->active_index = CLAMP(active, 0, (gint)state->urls.size() - 1);

    g_key_file_free(file);
    g_free(path);
    return true;
}

void session_save(const SessionState &state) {
    if (state.urls.empty()) {
        session_clear();
        return;
    }

    GKeyFile *file = g_key_file_new();
    g_key_file_set_integer(file, "session", "count", (gint)state.urls.size());
    g_key_file_set_integer(file, "session", "active",
                           CLAMP(state.active_index, 0, (gint)state.urls.size() - 1));
    for (size_t i = 0; i < state.urls.size(); i++) {
        gchar *key = g_strdup_printf("url%zu", i);
        g_key_file_set_string(file, "session", key, state.urls[i].c_str());
        g_free(key);
        gchar *pinned_key = g_strdup_printf("pinned%zu", i);
        bool pinned = i < state.pinned.size() && state.pinned[i];
        g_key_file_set_boolean(file, "session", pinned_key, pinned);
        g_free(pinned_key);
    }

    gchar *path = session_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gsize length = 0;
    gchar *data = g_key_file_to_data(file, &length, nullptr);
    GError *error = nullptr;
    if (!g_file_set_contents(path, data, (gssize)length, &error)) {
        g_printerr("[Kite] Could not save session: %s\n", error->message);
        g_clear_error(&error);
    } else {
        g_chmod(path, 0600);
    }
    g_free(data);
    g_free(path);
    g_key_file_free(file);
}

void session_clear() {
    gchar *path = session_path();
    g_remove(path);
    g_free(path);
}
