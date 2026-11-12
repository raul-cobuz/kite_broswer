#pragma once

#include <glib.h>

#include <string>

struct SyncResult {
    int bookmarks_added = 0;
    int pins_added = 0;
    int history_added = 0;
    int history_merged = 0;
    bool settings_applied = false;
};

// Exports bookmarks, pins, history and selected settings to a JSON file.
bool sync_write_file(const gchar *path, std::string *error);

// Merges a JSON file into the current profile. Existing local entries win for
// conflicting titles; history keeps the newest visit time and highest count.
bool sync_merge_file(const gchar *path, bool include_settings, SyncResult *result, std::string *error);

// Merges <folder>/kite-sync.json (when present) and writes the merged result
// back to the same file. The folder is created if needed.
bool sync_run(const gchar *folder, bool include_settings, SyncResult *result, std::string *error);

const gchar *sync_file_name();
