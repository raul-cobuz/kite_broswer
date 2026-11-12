#include "adblock.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <string>
#include <vector>

#include "adblock_rules.h"
#include "profile.h"
#include "settings.h"
#include "youtube_adblock.h"

static WebKitUserContentManager *content_manager = nullptr;
static WebKitUserContentFilterStore *filter_store = nullptr;
static WebKitUserContentFilter *base_filter = nullptr;
static WebKitUserContentFilter *exception_filter = nullptr;
static gboolean base_added = false;
static gboolean exception_added = false;
static gboolean adblock_enabled = true;
static guint exception_generation = 0;
static guint exception_serial = 0;

static bool is_youtube_host(const std::string &host) {
    return host == "youtube.com" || g_str_has_suffix(host.c_str(), ".youtube.com") ||
           host == "youtube-nocookie.com" || g_str_has_suffix(host.c_str(), ".youtube-nocookie.com");
}

static bool youtube_adblocking_allowed() {
    if (!adblock_enabled)
        return false;

    gchar **hosts = settings_adblock_exception_hosts();
    bool allowed = true;
    for (gchar **host = hosts; host && *host; host++) {
        if (is_youtube_host(*host)) {
            allowed = false;
            break;
        }
    }
    g_strfreev(hosts);
    return allowed;
}

static void sync_youtube_adblock() {
    youtube_adblock_set_enabled(youtube_adblocking_allowed());
}

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

    gchar *config_path = g_build_filename(profile_config_dir(), "filters.json", nullptr);
    if (g_file_test(config_path, G_FILE_TEST_IS_REGULAR))
        return config_path;
    g_free(config_path);

    if (!profile_is_default()) {
        gchar *legacy_path = g_build_filename(g_get_user_config_dir(), "kite", "filters.json", nullptr);
        if (g_file_test(legacy_path, G_FILE_TEST_IS_REGULAR))
            return legacy_path;
        g_free(legacy_path);
    }

    return nullptr;
}

static void apply_base_filter() {
    if (!content_manager || !base_filter)
        return;

    if (adblock_enabled && !base_added) {
        webkit_user_content_manager_add_filter(content_manager, base_filter);
        base_added = TRUE;
        g_print("[Kite] Ad and tracker blocking enabled.\n");
    } else if (!adblock_enabled && base_added) {
        webkit_user_content_manager_remove_filter(content_manager, base_filter);
        base_added = FALSE;
        g_print("[Kite] Ad and tracker blocking disabled.\n");
    }
}

static void remove_exception_filter() {
    if (exception_filter) {
        if (exception_added && content_manager)
            webkit_user_content_manager_remove_filter(content_manager, exception_filter);
        webkit_user_content_filter_unref(exception_filter);
        exception_filter = nullptr;
    }
    exception_added = false;
}

static void on_exception_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    guint generation = GPOINTER_TO_UINT(user_data);
    GError *error = nullptr;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);

    if (error) {
        g_printerr("[Kite] Could not compile site ad-block exceptions: %s\n", error->message);
        g_error_free(error);
    }

    if (generation != exception_generation || !adblock_enabled) {
        if (filter)
            webkit_user_content_filter_unref(filter);
        return;
    }

    remove_exception_filter();
    if (filter) {
        exception_filter = filter;
        webkit_user_content_manager_add_filter(content_manager, filter);
        exception_added = TRUE;
        g_print("[Kite] Per-site ad-block exceptions applied.\n");
    }
}

void adblock_reload_exceptions() {
    exception_generation++;
    sync_youtube_adblock();

    if (!adblock_enabled || !content_manager || !filter_store) {
        remove_exception_filter();
        return;
    }

    gchar **hosts = settings_adblock_exception_hosts();
    std::vector<std::string> exceptions;
    for (gchar **host = hosts; host && *host; host++)
        exceptions.emplace_back(*host);
    g_strfreev(hosts);

    if (exceptions.empty()) {
        remove_exception_filter();
        return;
    }

    std::string json = adblock_exception_rules_json(exceptions);
    gchar *name = g_strdup_printf("kite_exceptions_%u", ++exception_serial);
    GBytes *bytes = g_bytes_new(json.data(), json.size());
    webkit_user_content_filter_store_save(filter_store, name, bytes, nullptr, on_exception_saved,
                                          GUINT_TO_POINTER(exception_generation));
    g_bytes_unref(bytes);
    g_free(name);
}

static void on_base_filter_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = nullptr;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);

    if (error) {
        g_printerr("[Kite] Could not compile ad-block filters: %s\n", error->message);
        g_error_free(error);
        return;
    }

    base_filter = filter;
    apply_base_filter();
    if (adblock_enabled)
        adblock_reload_exceptions();
}

void adblock_set_enabled(bool enabled) {
    adblock_enabled = enabled;
    apply_base_filter();
    adblock_reload_exceptions();
}

WebKitUserContentManager *adblock_content_manager() {
    return content_manager;
}

void init_adblocker() {
    if (!content_manager)
        content_manager = webkit_user_content_manager_new();

    adblock_enabled = settings_block_ads();
    sync_youtube_adblock();

    gchar *filter_cache_dir = g_build_filename(profile_cache_dir(), "adblock", nullptr);
    g_mkdir_with_parents(filter_cache_dir, 0700);
    filter_store = webkit_user_content_filter_store_new(filter_cache_dir);
    g_free(filter_cache_dir);

    gchar *rules_content = nullptr;
    gsize length = 0;
    GError *file_error = nullptr;
    gchar *filters_path = find_filters_json_path();

    if (filters_path && g_file_get_contents(filters_path, &rules_content, &length, &file_error)) {
        g_print("[Kite] Loading %s (%lu bytes)...\n", filters_path, length);
        GBytes *bytes = g_bytes_new_take(rules_content, length);
        webkit_user_content_filter_store_save(filter_store, "kite_rules", bytes, nullptr, on_base_filter_saved,
                                              nullptr);
        g_bytes_unref(bytes);
    } else {
        if (file_error)
            g_error_free(file_error);
        g_printerr("[Kite] filters.json is missing (looked next to the binary, in the CWD, and in "
                   "~/.config/kite/). Run: python3 update_filters.py\n");
    }
    g_free(filters_path);
}
