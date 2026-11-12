#pragma once

#include <glib.h>
#include <webkit2/webkit2.h>

#include <string>
#include <vector>

struct ExtensionInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string path;
    std::string manifest_json;
    std::vector<std::string> permissions;
    std::vector<std::string> host_permissions;
    std::vector<std::string> background_scripts;
    std::string background_page;
    bool valid = false;
    bool enabled = false;
    int script_count = 0;
    int style_count = 0;
    std::string error;

    bool has_background() const { return !background_scripts.empty() || !background_page.empty(); }
};

// Creates the extension directory, registers the kite-ext scheme and injects
// enabled content scripts and background pages.
void extensions_init(WebKitWebContext *context = nullptr);

// Registers the kite-ext:// scheme on an additional WebKit context.
void extensions_register_scheme(WebKitWebContext *context);

// Rescans the extension directory and rebuilds injected scripts, background
// pages and API handlers.
void extensions_reload();

std::vector<ExtensionInfo> extensions_list();
const gchar *extensions_directory();

// Installs a folder containing manifest.json under the Kite data directory.
// The folder is copied; the source is left untouched.
bool extensions_install_from_folder(const gchar *source_dir, std::string *error);
bool extensions_set_enabled(const gchar *id, bool enabled);
bool extensions_remove(const gchar *id);

bool extension_has_permission(const gchar *id, const gchar *permission);
