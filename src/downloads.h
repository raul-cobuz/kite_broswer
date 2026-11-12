#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

void init_downloads(WebKitWebContext *context);
void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data);

// Returns the toolbar download button; creates the downloads popover on first call.
GtkWidget *create_downloads_button();
