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
    check(sync_write_file(export_path, true, &sync_error), "sync export written");
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

    // Deletions travel through the sync file instead of being re-imported.
    bookmark_add("https://tombstone-bookmark.test/", "Tombstone bookmark");
    history_record_visit("https://tombstone-history.test/", "Tombstone history");
    bookmark_remove("https://tombstone-bookmark.test/");
    history_delete_url("https://tombstone-history.test/");
    check(sync_write_file(export_path, true, &sync_error), "tombstone export written");
    contents = nullptr;
    check(g_file_get_contents(export_path, &contents, nullptr, nullptr), "tombstone export readable");
    if (contents) {
        std::string exported(contents);
        g_free(contents);
        check(exported.find("\"kind\":\"bookmark\"") != std::string::npos, "bookmark tombstone exported");
        check(exported.find("\"kind\":\"history\"") != std::string::npos, "history tombstone exported");
    }

    const char *stale = R"JSON({
      "format": 1,
      "bookmarks": [{"url": "https://tombstone-bookmark.test/", "title": "Stale", "created": 1}],
      "history": [{"url": "https://tombstone-history.test/", "title": "Stale", "last_visit": 1, "visit_count": 1}],
      "deleted": [
        {"kind": "bookmark", "url": "https://tombstone-bookmark.test/", "deleted_at": 9999999999999999},
        {"kind": "history", "url": "https://tombstone-history.test/", "deleted_at": 9999999999999999}
      ]
    })JSON";
    check(g_file_set_contents(export_path, stale, -1, nullptr), "stale sync file written");
    check(sync_merge_file(export_path, false, &result, &sync_error), "stale sync file merged");
    check(!bookmark_exists("https://tombstone-bookmark.test/"), "deleted bookmark not resurrected");
    bool tombstone_history_found = false;
    for (const auto &entry : history_recent(100)) {
        if (entry.url == "https://tombstone-history.test/")
            tombstone_history_found = true;
    }
    check(!tombstone_history_found, "deleted history not resurrected");

    // A newer bookmark is a deliberate re-add and clears the tombstone.
    const char *readded = R"JSON({
      "format": 1,
      "bookmarks": [{"url": "https://readd-bookmark.test/", "title": "Re-added", "created": 2000}],
      "deleted": [{"kind": "bookmark", "url": "https://readd-bookmark.test/", "deleted_at": 1000}]
    })JSON";
    check(g_file_set_contents(export_path, readded, -1, nullptr), "re-add sync file written");
    check(sync_merge_file(export_path, false, &result, &sync_error), "re-add sync file merged");
    check(bookmark_exists("https://readd-bookmark.test/"), "bookmark created after a deletion is accepted");

    // Stale settings are ignored once the local settings file is newer.
    save_settings();
    gint64 fresh_settings = settings_saved_at();
    check(fresh_settings > 0, "settings mtime available");
    std::string current_homepage = settings_homepage();
    std::string stale_settings = "{\"format\":1,\"settings\":{\"homepage\":\"https://stale.example/\","
                                 "\"updated_at\":" +
                                 std::to_string(fresh_settings - 1000000) + "}}";
    check(g_file_set_contents(export_path, stale_settings.c_str(), -1, nullptr), "stale settings written");
    check(sync_merge_file(export_path, true, &result, &sync_error), "stale settings merged");
    check(std::string(settings_homepage()) == current_homepage, "older settings do not overwrite local ones");

    // Opting out of settings sync keeps the existing shared settings block.
    const char *kept_settings = R"JSON({
      "format": 1,
      "settings": {"homepage": "https://keep.example/", "updated_at": 1}
    })JSON";
    check(g_file_set_contents(export_path, kept_settings, -1, nullptr), "kept settings written");
    check(sync_write_file(export_path, false, &sync_error), "settings-opt-out export written");
    contents = nullptr;
    check(g_file_get_contents(export_path, &contents, nullptr, nullptr), "settings-opt-out readable");
    if (contents) {
        std::string exported(contents);
        g_free(contents);
        check(exported.find("https://keep.example/") != std::string::npos, "existing shared settings preserved");
        check(exported.find(current_homepage) == std::string::npos, "local settings not published when disabled");
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
