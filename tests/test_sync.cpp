#include "sync.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <cstdio>
#include <string>

#include "history.h"
#include "profile.h"
#include "settings.h"

static int failures = 0;

static void check(bool condition, const char *label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        failures++;
    }
}

int main() {
    GError *error = nullptr;
    gchar *tmp = g_dir_make_tmp("kite-sync-XXXXXX", &error);
    if (!tmp) {
        std::fprintf(stderr, "Could not create temp dir: %s\n", error ? error->message : "unknown error");
        if (error)
            g_error_free(error);
        return 1;
    }

    gchar *config_home = g_build_filename(tmp, "config", nullptr);
    gchar *data_home = g_build_filename(tmp, "data", nullptr);
    gchar *cache_home = g_build_filename(tmp, "cache", nullptr);
    g_mkdir_with_parents(config_home, 0700);
    g_mkdir_with_parents(data_home, 0700);
    g_mkdir_with_parents(cache_home, 0700);
    g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
    g_setenv("XDG_DATA_HOME", data_home, TRUE);
    g_setenv("XDG_CACHE_HOME", cache_home, TRUE);

    profile_init(nullptr);
    if (!history_init()) {
        std::fprintf(stderr, "Could not open history database\n");
        return 1;
    }
    load_settings();

    history_record_visit("https://local.test/", "Local page");
    bookmark_add("https://local-bookmark.test/", "Local bookmark");
    pinned_link_add("https://local-pin.test/", "Local pin");

    gchar *folder = g_build_filename(tmp, "shared", nullptr);
    g_mkdir_with_parents(folder, 0700);
    gchar *sync_path = g_build_filename(folder, sync_file_name(), nullptr);

    const char *remote = R"JSON({
      "format": 1,
      "bookmarks": [
        {"url": "https://remote-bookmark.test/", "title": "Remote bookmark"},
        {"url": "https://local-bookmark.test/", "title": "Remote title"}
      ],
      "pins": [
        {"url": "https://remote-pin-1.test/", "title": "Remote pin 1"},
        {"url": "https://remote-pin-2.test/", "title": "Remote pin 2"},
        {"url": "https://remote-pin-3.test/", "title": "Remote pin 3"},
        {"url": "https://remote-pin-4.test/", "title": "Remote pin 4"},
        {"url": "https://remote-pin-5.test/", "title": "Remote pin 5"}
      ],
      "history": [
        {"url": "https://remote-history.test/", "title": "Remote history", "last_visit": 5000, "visit_count": 4},
        {"url": "https://local.test/", "title": "Remote local title", "last_visit": 9000, "visit_count": 9}
      ],
      "settings": {
        "homepage": "https://synced.example/",
        "search_engine": "brave",
        "google_fallback": false,
        "dark_mode": true
      }
    })JSON";
    check(g_file_set_contents(sync_path, remote, -1, nullptr), "remote sync file written");

    SyncResult result;
    std::string sync_error;
    check(sync_merge_file(sync_path, false, &result, &sync_error), "sync file merged");
    check(result.bookmarks_added == 1, "one new bookmark imported");
    check(result.pins_added == 4, "pins imported up to the five-link limit");
    check(bookmark_exists("https://remote-bookmark.test/"), "remote bookmark present");
    check(bookmark_exists("https://local-bookmark.test/"), "local bookmark kept");
    check(history_recent(20).size() == 2, "history merged without duplicates");
    check(pinned_links_count() == KITE_MAX_PINNED_LINKS, "pin limit enforced during sync");
    check(std::string(settings_homepage()) == "kite://newtab", "settings untouched when disabled");

    gchar *export_path = g_build_filename(tmp, "export.json", nullptr);
    check(sync_write_file(export_path, &sync_error), "sync export written");
    gchar *contents = nullptr;
    check(g_file_get_contents(export_path, &contents, nullptr, nullptr), "sync export readable");
    if (contents) {
        check(std::string(contents).find("https://local-bookmark.test/") != std::string::npos,
              "export contains local bookmarks");
        check(std::string(contents).find("https://remote-history.test/") != std::string::npos,
              "export contains merged history");
        g_free(contents);
    }

    check(sync_run(folder, true, &result, &sync_error), "folder sync runs");
    check(result.settings_applied, "settings applied when enabled");
    check(std::string(settings_homepage()) == "https://synced.example/", "homepage synced");
    check(std::string(settings_search_engine()) == "brave", "search engine synced");
    check(!settings_google_fallback(), "Google fallback setting synced");
    check(settings_dark_mode(), "dark mode synced");

    check(g_file_get_contents(sync_path, &contents, nullptr, nullptr), "synced file readable");
    if (contents) {
        check(std::string(contents).find("https://remote-bookmark.test/") != std::string::npos,
              "merged file contains imported bookmark");
        g_free(contents);
    }

    history_close();
    g_remove(sync_path);
    g_remove(export_path);
    g_rmdir(folder);
    g_free(sync_path);
    g_free(export_path);
    g_free(folder);
    g_free(cache_home);
    g_free(data_home);
    g_free(config_home);
    g_rmdir(tmp);
    g_free(tmp);

    if (failures) {
        std::fprintf(stderr, "%d sync test(s) failed\n", failures);
        return 1;
    }
    std::printf("sync tests passed\n");
    return 0;
}
