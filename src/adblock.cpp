#include "adblock.h"

#include <glib.h>
#include <glib/gstdio.h>

static WebKitUserContentManager *content_manager = nullptr;

// Look for filters.json next to the binary, then CWD, then ~/.config/kite/
static gchar *find_filters_json_path() {
    gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        gchar *exe_dir = g_path_get_dirname(exe_path);
        gchar *candidate = g_build_filename(exe_dir, "filters.json", NULL);
        g_free(exe_dir);
        g_free(exe_path);
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
            return candidate;
        g_free(candidate);
    }

    if (g_file_test("filters.json", G_FILE_TEST_IS_REGULAR))
        return g_strdup("filters.json");

    gchar *config_path = g_build_filename(g_get_user_config_dir(), "kite", "filters.json", NULL);
    if (g_file_test(config_path, G_FILE_TEST_IS_REGULAR))
        return config_path;
    g_free(config_path);

    return nullptr;
}

static void on_filter_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = nullptr;
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(store, res, &error);

    if (error) {
        g_printerr("[Kite] EROARE la compilarea filtrelor: %s\n", error->message);
        g_error_free(error);
    } else {
        webkit_user_content_manager_add_filter(content_manager, filter);
        webkit_user_content_filter_unref(filter);
        g_print("\n>>> [Kite] AD-BLOCKER ACTIVAT CU SUCCES! <<<\n\n");
    }
    g_object_unref(store);
}

WebKitUserContentManager *adblock_content_manager() {
    return content_manager;
}

void init_adblocker() {
    if (!content_manager)
        content_manager = webkit_user_content_manager_new();

    gchar *filter_cache_dir = g_build_filename(g_get_user_cache_dir(), "kite", "adblock", NULL);
    g_mkdir_with_parents(filter_cache_dir, 0700);

    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(filter_cache_dir);
    g_free(filter_cache_dir);

    gchar *rules_content = nullptr;
    gsize length = 0;
    GError *file_error = nullptr;
    gchar *filters_path = find_filters_json_path();

    if (filters_path && g_file_get_contents(filters_path, &rules_content, &length, &file_error)) {
        g_print("[Kite] Se incarca %s (%lu bytes)...\n", filters_path, length);
        GBytes *bytes = g_bytes_new_take(rules_content, length);
        webkit_user_content_filter_store_save(store, "kite_rules", bytes, NULL, on_filter_saved, NULL);
        g_bytes_unref(bytes);
    } else {
        if (file_error)
            g_error_free(file_error);
        g_printerr("[Kite] EROARE: filters.json lipseste (cautat langa binar, CWD, ~/.config/kite/). Ruleaza: "
                   "python3 update_filters.py\n");
        g_object_unref(store);
    }
    g_free(filters_path);
}
