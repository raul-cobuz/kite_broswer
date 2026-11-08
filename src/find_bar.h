#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

GtkWidget *create_find_bar();
void connect_find_handlers(WebKitWebView *web_view);
void run_find(WebKitWebView *web_view, const gchar *text);
void sync_find_bar(WebKitWebView *web_view);
void close_find_bar();
