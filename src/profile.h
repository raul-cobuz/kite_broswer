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

// A guest profile uses a throwaway directory that is removed when Kite exits.
// It never writes the active-profile marker and is not part of the registry.
void profile_init_guest();
bool profile_is_guest();
void profile_cleanup_guest();

const gchar *profile_current_id();
const gchar *profile_current_name();
bool profile_is_default();

// Deterministic avatar identity shared by the toolbar icon and kite://profiles.
std::string profile_avatar_text(const std::string &name);
int profile_avatar_color(const std::string &id);
void profile_avatar_rgb(const std::string &id, double *red, double *green, double *blue);

std::vector<ProfileInfo> profile_list();
bool profile_exists(const gchar *id);
bool profile_create(const gchar *name, std::string *error, std::string *new_id = nullptr);
bool profile_rename(const gchar *id, const gchar *name, std::string *error);
bool profile_delete(const gchar *id, std::string *error);

// Writes the active-profile marker and launches a new Kite process with
// --profile=<id>. The caller is responsible for saving state and quitting.
bool profile_switch(const gchar *id);
bool profile_switch_guest();

const gchar *profile_data_dir();
const gchar *profile_config_dir();
const gchar *profile_cache_dir();
const gchar *profile_extension_storage_dir();
const gchar *profile_webkit_data_dir();
const gchar *profile_webkit_cache_dir();
const gchar *profile_base_data_dir();
const gchar *profile_base_config_dir();
