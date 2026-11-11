#pragma once

#include <glib.h>

#include <string>
#include <vector>

struct ExtensionInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string path;
    bool valid = false;
    bool enabled = false;
    int script_count = 0;
    int style_count = 0;
    std::string error;
};

// Creates the extension directory and injects every enabled extension into the
// shared user content manager. Call once after init_adblocker().
void extensions_init();

// Rescans the extension directory and rebuilds injected scripts and styles.
void extensions_reload();

std::vector<ExtensionInfo> extensions_list();
const gchar *extensions_directory();

// Installs a folder containing manifest.json under the Kite data directory.
// The folder is copied; the source is left untouched.
bool extensions_install_from_folder(const gchar *source_dir, std::string *error);
bool extensions_set_enabled(const gchar *id, bool enabled);
bool extensions_remove(const gchar *id);
