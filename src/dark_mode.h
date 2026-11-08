#pragma once

#include <gtk/gtk.h>

void init_dark_mode();
void apply_dark_mode(bool enable);
void toggle_dark_mode();
void on_dark_mode_toggled(GtkToggleButton *button, gpointer user_data);
