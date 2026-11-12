#pragma once

#include <glib.h>

#include <string>
#include <vector>

struct ProfileInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    bool current = false;
};

// Loads the profile registry and selects the profile for this process. Pass
// nullptr to use the active-profile marker (or the default profile). The
// default profile keeps the legacy Kite directories; named profiles live in
// isolated subdirectories.
void profile_init(const gchar *requested_id = nullptr);

const gchar *profile_current_id();
const gchar *profile_current_name();
bool profile_is_default();

std::vector<ProfileInfo> profile_list();
bool profile_exists(const gchar *id);
bool profile_create(const gchar *name, std::string *error, std::string *new_id = nullptr);
bool profile_rename(const gchar *id, const gchar *name, std::string *error);
bool profile_delete(const gchar *id, std::string *error);

// Writes the active-profile marker and launches a new Kite process with
// --profile=<id>. The caller is responsible for saving state and quitting.
bool profile_switch(const gchar *id);

const gchar *profile_data_dir();
const gchar *profile_config_dir();
const gchar *profile_cache_dir();
const gchar *profile_extension_storage_dir();
const gchar *profile_webkit_data_dir();
const gchar *profile_webkit_cache_dir();
const gchar *profile_base_data_dir();
const gchar *profile_base_config_dir();
