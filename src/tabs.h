#pragma once

#include <glib.h>
#include <webkit2/webkit2.h>

void create_new_tab(const gchar *url);
void create_incognito_tab(const gchar *url);
void close_tab(WebKitWebView *web_view);
void reopen_last_closed_tab();
