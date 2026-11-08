#pragma once

#include <glib.h>

void load_settings();
void save_settings();

gchar *permission_key(const gchar *type, const gchar *origin);
gint get_permission_decision(const gchar *type, const gchar *origin);
void remember_permission_decision(const gchar *type, const gchar *origin, gboolean allow);

bool settings_dark_mode();
void settings_set_dark_mode(bool enabled);
