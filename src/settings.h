#pragma once

#include <glib.h>

void load_settings();
void save_settings();

gchar *permission_key(const gchar *type, const gchar *origin);
gint get_permission_decision(const gchar *type, const gchar *origin);
void remember_permission_decision(const gchar *type, const gchar *origin, gboolean allow);

bool settings_dark_mode();
void settings_set_dark_mode(bool enabled);

const gchar *settings_homepage();
void settings_set_homepage(const gchar *url);

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
