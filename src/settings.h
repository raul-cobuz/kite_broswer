#pragma once

#include <glib.h>

#include <string>
#include <vector>

struct NewTabLink {
    std::string title;
    std::string url;
};

constexpr int KITE_MAX_NEWTAB_LINKS = 12;

void load_settings();
void save_settings();

gchar *permission_key(const gchar *type, const gchar *origin);
gint get_permission_decision(const gchar *type, const gchar *origin);
void remember_permission_decision(const gchar *type, const gchar *origin, gboolean allow);

bool settings_dark_mode();
void settings_set_dark_mode(bool enabled);

const gchar *settings_homepage();
void settings_set_homepage(const gchar *url);

const gchar *settings_search_engine();
void settings_set_search_engine(const gchar *engine);
bool settings_google_fallback();
void settings_set_google_fallback(bool enabled);

bool settings_restore_session();
void settings_set_restore_session(bool enabled);

const gchar *settings_download_directory();
void settings_set_download_directory(const gchar *path);

double settings_site_zoom(const gchar *host);
void settings_set_site_zoom(const gchar *host, double level);
void settings_clear_site_zoom(const gchar *host);

bool settings_block_ads();
void settings_set_block_ads(bool enabled);
bool settings_site_adblock_enabled(const gchar *host);
void settings_set_site_adblock_enabled(const gchar *host, bool enabled);
gchar **settings_adblock_exception_hosts();

bool settings_strip_tracking_params();
void settings_set_strip_tracking_params(bool enabled);

bool settings_clear_on_exit();
void settings_set_clear_on_exit(bool enabled);

const gchar *settings_sync_folder();
void settings_set_sync_folder(const gchar *path);
bool settings_sync_on_startup();
void settings_set_sync_on_startup(bool enabled);
bool settings_sync_settings();
void settings_set_sync_settings(bool enabled);

// Start-page customization. These preferences are separate from the five
// pinned links stored in the SQLite database: they control the presentation of
// kite://newtab and its optional quick-link shelf.
bool settings_newtab_clock();
void settings_set_newtab_clock(bool enabled);
bool settings_newtab_weather();
void settings_set_newtab_weather(bool enabled);
const gchar *settings_newtab_weather_city();
void settings_set_newtab_weather_city(const gchar *city);
const gchar *settings_newtab_weather_units();
void settings_set_newtab_weather_units(const gchar *units);
const gchar *settings_newtab_background();
void settings_set_newtab_background(const gchar *value);
bool settings_newtab_compact();
void settings_set_newtab_compact(bool enabled);
std::vector<NewTabLink> settings_newtab_links();
void settings_set_newtab_links(const std::vector<NewTabLink> &links);
bool settings_newtab_link_add(const gchar *title, const gchar *url);
bool settings_newtab_link_remove(const gchar *url);
