#include "settings.h"

#include <glib/gstdio.h>

#include <algorithm>
#include <cmath>

#include "profile.h"

static GKeyFile *settings = nullptr;
static gchar *settings_file = nullptr;
static GHashTable *permission_decisions = nullptr;
static GHashTable *site_zoom_levels = nullptr;
static GHashTable *adblock_exceptions = nullptr;
static bool dark_mode = false;
static gchar *homepage = nullptr;
static gchar *search_engine = nullptr;
static bool google_fallback = true;
static gchar *download_directory = nullptr;
static bool restore_session = true;
static bool block_ads = true;
static bool strip_tracking_params = true;
static bool clear_on_exit = false;
static gchar *sync_folder = nullptr;
static bool sync_on_startup = true;
static bool sync_settings = true;
static bool newtab_clock = true;
static bool newtab_weather = false;
static gchar *newtab_weather_city = nullptr;
static gchar *newtab_weather_units = nullptr;
static gchar *newtab_background = nullptr;
static bool newtab_compact = false;
static std::vector<NewTabLink> newtab_links;

static gchar *default_download_directory() {
    const gchar *special = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (special && *special)
        return g_strdup(special);
    return g_build_filename(g_get_home_dir(), "Downloads", NULL);
}

static const char *normalize_search_engine(const gchar *engine) {
    static const char *const allowed[] = {"google", "duckduckgo", "startpage", "brave"};
    if (engine) {
        for (const char *candidate : allowed) {
            if (g_ascii_strcasecmp(engine, candidate) == 0)
                return candidate;
        }
    }
    return "google";
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

static bool boolean_setting(const gchar *group, const gchar *key, bool fallback) {
    GError *error = nullptr;
    gboolean value = g_key_file_get_boolean(settings, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return fallback;
    }
    return value != FALSE;
}

static const char *normalize_weather_units(const gchar *units) {
    return units && g_ascii_strcasecmp(units, "fahrenheit") == 0 ? "fahrenheit" : "celsius";
}

static void load_newtab_links() {
    newtab_links.clear();
    GError *error = nullptr;
    gint count = g_key_file_get_integer(settings, "newtab", "link_count", &error);
    if (error) {
        g_clear_error(&error);
        return;
    }
    count = CLAMP(count, 0, KITE_MAX_NEWTAB_LINKS);
    for (gint i = 0; i < count; i++) {
        gchar *title_key = g_strdup_printf("link%d_title", i);
        gchar *url_key = g_strdup_printf("link%d_url", i);
        gchar *title = g_key_file_get_string(settings, "newtab", title_key, nullptr);
        gchar *url = g_key_file_get_string(settings, "newtab", url_key, nullptr);
        g_free(title_key);
        g_free(url_key);
        if (url && *url) {
            NewTabLink link;
            link.title = title ? title : "";
            link.url = url;
            newtab_links.push_back(link);
        }
        g_free(title);
        g_free(url);
    }
}

static gchar *normalize_host(const gchar *host) {
    if (!host)
        return nullptr;
    gchar *trimmed = g_strstrip(g_strdup(host));
    gchar *lower = g_ascii_strdown(trimmed, -1);
    g_free(trimmed);
    if (!*lower) {
        g_free(lower);
        return nullptr;
    }
    return lower;
}

void load_settings() {
    if (permission_decisions)
        g_hash_table_destroy(permission_decisions);
    if (site_zoom_levels)
        g_hash_table_destroy(site_zoom_levels);
    if (adblock_exceptions)
        g_hash_table_destroy(adblock_exceptions);
    if (settings)
        g_key_file_free(settings);
    g_free(settings_file);
    g_free(homepage);
    g_free(search_engine);
    g_free(download_directory);
    g_free(sync_folder);
    g_free(newtab_weather_city);
    g_free(newtab_weather_units);
    g_free(newtab_background);

    permission_decisions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    site_zoom_levels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    adblock_exceptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    settings = g_key_file_new();
    settings_file = g_build_filename(profile_config_dir(), "settings.ini", NULL);
    dark_mode = false;
    homepage = g_strdup("kite://newtab");
    search_engine = g_strdup("google");
    google_fallback = true;
    download_directory = default_download_directory();
    restore_session = true;
    block_ads = true;
    strip_tracking_params = true;
    clear_on_exit = false;
    sync_folder = g_strdup("");
    sync_on_startup = true;
    sync_settings = true;
    newtab_clock = true;
    newtab_weather = false;
    newtab_weather_city = g_strdup("");
    newtab_weather_units = g_strdup("celsius");
    newtab_background = g_strdup("");
    newtab_compact = false;
    newtab_links.clear();

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

    g_free(search_engine);
    gchar *configured_engine = string_setting("search", "engine", "google");
    search_engine = g_strdup(normalize_search_engine(configured_engine));
    g_free(configured_engine);

    gboolean fallback = g_key_file_get_boolean(settings, "search", "google_fallback", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        google_fallback = fallback;
    }

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

    gboolean ads = g_key_file_get_boolean(settings, "privacy", "block_ads", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        block_ads = ads;
    }

    gboolean strip = g_key_file_get_boolean(settings, "privacy", "strip_tracking_params", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        strip_tracking_params = strip;
    }

    gboolean clear_data = g_key_file_get_boolean(settings, "privacy", "clear_on_exit", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        clear_on_exit = clear_data;
    }

    g_free(sync_folder);
    sync_folder = string_setting("sync", "folder", "");

    gboolean startup_sync = g_key_file_get_boolean(settings, "sync", "on_startup", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        sync_on_startup = startup_sync;
    }

    gboolean sync_settings_enabled = g_key_file_get_boolean(settings, "sync", "include_settings", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        sync_settings = sync_settings_enabled;
    }

    newtab_clock = boolean_setting("newtab", "clock", true);
    newtab_weather = boolean_setting("newtab", "weather", false);
    newtab_compact = boolean_setting("newtab", "compact", false);
    g_free(newtab_weather_city);
    newtab_weather_city = string_setting("newtab", "weather_city", "");
    g_free(newtab_weather_units);
    {
        gchar *units = string_setting("newtab", "weather_units", "celsius");
        newtab_weather_units = g_strdup(normalize_weather_units(units));
        g_free(units);
    }
    g_free(newtab_background);
    newtab_background = string_setting("newtab", "background", "");
    load_newtab_links();

    gchar *exceptions = g_key_file_get_string(settings, "privacy", "adblock_exceptions", nullptr);
    if (exceptions) {
        gchar **hosts = g_strsplit(exceptions, ";", -1);
        for (gchar **p = hosts; *p; p++) {
            gchar *host = normalize_host(*p);
            if (host)
                g_hash_table_add(adblock_exceptions, host);
        }
        g_strfreev(hosts);
        g_free(exceptions);
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
    g_key_file_set_string(settings, "search", "engine", settings_search_engine());
    g_key_file_set_boolean(settings, "search", "google_fallback", google_fallback);
    g_key_file_set_boolean(settings, "session", "restore", restore_session);
    g_key_file_set_string(settings, "downloads", "directory", settings_download_directory());
    // Downloads always prompt now; drop the obsolete automatic-save flag.
    g_key_file_remove_key(settings, "downloads", "ask", nullptr);

    g_key_file_set_boolean(settings, "privacy", "block_ads", block_ads);
    g_key_file_set_boolean(settings, "privacy", "strip_tracking_params", strip_tracking_params);
    g_key_file_set_boolean(settings, "privacy", "clear_on_exit", clear_on_exit);

    g_key_file_set_string(settings, "sync", "folder", settings_sync_folder());
    g_key_file_set_boolean(settings, "sync", "on_startup", sync_on_startup);
    g_key_file_set_boolean(settings, "sync", "include_settings", sync_settings);

    g_key_file_remove_group(settings, "newtab", nullptr);
    g_key_file_set_boolean(settings, "newtab", "clock", newtab_clock);
    g_key_file_set_boolean(settings, "newtab", "weather", newtab_weather);
    g_key_file_set_string(settings, "newtab", "weather_city", settings_newtab_weather_city());
    g_key_file_set_string(settings, "newtab", "weather_units", settings_newtab_weather_units());
    g_key_file_set_string(settings, "newtab", "background", settings_newtab_background());
    g_key_file_set_boolean(settings, "newtab", "compact", newtab_compact);
    g_key_file_set_integer(settings, "newtab", "link_count", (gint)newtab_links.size());
    for (size_t i = 0; i < newtab_links.size(); i++) {
        gchar *title_key = g_strdup_printf("link%zu_title", i);
        gchar *url_key = g_strdup_printf("link%zu_url", i);
        g_key_file_set_string(settings, "newtab", title_key, newtab_links[i].title.c_str());
        g_key_file_set_string(settings, "newtab", url_key, newtab_links[i].url.c_str());
        g_free(title_key);
        g_free(url_key);
    }

    g_key_file_remove_key(settings, "privacy", "adblock_exceptions", nullptr);
    if (adblock_exceptions && g_hash_table_size(adblock_exceptions) > 0) {
        gchar **hosts = settings_adblock_exception_hosts();
        gchar *joined = g_strjoinv(";", hosts);
        g_key_file_set_string(settings, "privacy", "adblock_exceptions", joined);
        g_free(joined);
        g_strfreev(hosts);
    }

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

const gchar *settings_search_engine() {
    return search_engine ? search_engine : "google";
}

void settings_set_search_engine(const gchar *engine) {
    g_free(search_engine);
    search_engine = g_strdup(normalize_search_engine(engine));
}

bool settings_google_fallback() {
    return google_fallback;
}

void settings_set_google_fallback(bool enabled) {
    google_fallback = enabled;
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

bool settings_block_ads() {
    return block_ads;
}

void settings_set_block_ads(bool enabled) {
    block_ads = enabled;
}

bool settings_site_adblock_enabled(const gchar *host) {
    if (!adblock_exceptions || !host || !*host)
        return true;
    gchar *normalized = normalize_host(host);
    if (!normalized)
        return true;
    bool enabled = !g_hash_table_contains(adblock_exceptions, normalized);
    g_free(normalized);
    return enabled;
}

void settings_set_site_adblock_enabled(const gchar *host, bool enabled) {
    if (!adblock_exceptions)
        return;
    gchar *normalized = normalize_host(host);
    if (!normalized)
        return;
    if (enabled) {
        g_hash_table_remove(adblock_exceptions, normalized);
        g_free(normalized);
    } else {
        g_hash_table_add(adblock_exceptions, normalized);
    }
}

static gint compare_hosts(gconstpointer a, gconstpointer b) {
    const gchar *left = *(const gchar *const *)a;
    const gchar *right = *(const gchar *const *)b;
    return g_strcmp0(left, right);
}

gchar **settings_adblock_exception_hosts() {
    GPtrArray *hosts = g_ptr_array_new_with_free_func(g_free);
    if (adblock_exceptions) {
        GHashTableIter iter;
        gpointer key = nullptr;
        g_hash_table_iter_init(&iter, adblock_exceptions);
        while (g_hash_table_iter_next(&iter, &key, nullptr))
            g_ptr_array_add(hosts, g_strdup((const gchar *)key));
    }
    g_ptr_array_sort(hosts, compare_hosts);
    g_ptr_array_add(hosts, nullptr);
    return (gchar **)g_ptr_array_free(hosts, FALSE);
}

bool settings_strip_tracking_params() {
    return strip_tracking_params;
}

void settings_set_strip_tracking_params(bool enabled) {
    strip_tracking_params = enabled;
}

bool settings_clear_on_exit() {
    return clear_on_exit;
}

void settings_set_clear_on_exit(bool enabled) {
    clear_on_exit = enabled;
}

const gchar *settings_sync_folder() {
    return sync_folder ? sync_folder : "";
}

void settings_set_sync_folder(const gchar *path) {
    g_free(sync_folder);
    sync_folder = g_strdup(path ? path : "");
}

bool settings_sync_on_startup() {
    return sync_on_startup;
}

void settings_set_sync_on_startup(bool enabled) {
    sync_on_startup = enabled;
}

bool settings_sync_settings() {
    return sync_settings;
}

void settings_set_sync_settings(bool enabled) {
    sync_settings = enabled;
}

bool settings_newtab_clock() {
    return newtab_clock;
}

void settings_set_newtab_clock(bool enabled) {
    newtab_clock = enabled;
}

bool settings_newtab_weather() {
    return newtab_weather;
}

void settings_set_newtab_weather(bool enabled) {
    newtab_weather = enabled;
}

const gchar *settings_newtab_weather_city() {
    return newtab_weather_city ? newtab_weather_city : "";
}

void settings_set_newtab_weather_city(const gchar *city) {
    g_free(newtab_weather_city);
    gchar *trimmed = city ? g_strstrip(g_strdup(city)) : g_strdup("");
    newtab_weather_city = trimmed;
}

const gchar *settings_newtab_weather_units() {
    return newtab_weather_units ? newtab_weather_units : "celsius";
}

void settings_set_newtab_weather_units(const gchar *units) {
    g_free(newtab_weather_units);
    newtab_weather_units = g_strdup(normalize_weather_units(units));
}

const gchar *settings_newtab_background() {
    return newtab_background ? newtab_background : "";
}

void settings_set_newtab_background(const gchar *value) {
    g_free(newtab_background);
    gchar *trimmed = value ? g_strstrip(g_strdup(value)) : g_strdup("");
    newtab_background = trimmed;
}

bool settings_newtab_compact() {
    return newtab_compact;
}

void settings_set_newtab_compact(bool enabled) {
    newtab_compact = enabled;
}

std::vector<NewTabLink> settings_newtab_links() {
    return newtab_links;
}

void settings_set_newtab_links(const std::vector<NewTabLink> &links) {
    newtab_links.clear();
    for (const auto &link : links) {
        if (link.url.empty())
            continue;
        newtab_links.push_back(link);
        if ((int)newtab_links.size() >= KITE_MAX_NEWTAB_LINKS)
            break;
    }
}

bool settings_newtab_link_add(const gchar *title, const gchar *url) {
    if (!url || !*url)
        return false;
    for (const auto &link : newtab_links) {
        if (link.url == url) {
            if (title && *title) {
                for (auto &existing : newtab_links) {
                    if (existing.url == url) {
                        existing.title = title;
                        break;
                    }
                }
                return true;
            }
            return false;
        }
    }
    if ((int)newtab_links.size() >= KITE_MAX_NEWTAB_LINKS)
        return false;
    NewTabLink link;
    link.title = title ? title : "";
    link.url = url;
    newtab_links.push_back(link);
    return true;
}

bool settings_newtab_link_remove(const gchar *url) {
    if (!url || !*url)
        return false;
    auto end = std::remove_if(newtab_links.begin(), newtab_links.end(),
                              [url](const NewTabLink &link) { return link.url == url; });
    bool removed = end != newtab_links.end();
    newtab_links.erase(end, newtab_links.end());
    return removed;
}
