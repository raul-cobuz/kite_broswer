#pragma once

#include <glib.h>
#include <webkit2/webkit2.h>

#include <string>
#include <vector>

struct KiteTabInfo {
    int id = 0;
    int index = 0;
    bool active = false;
    bool incognito = false;
    std::string url;
    std::string title;
};

void create_new_tab(const gchar *url);
void create_incognito_tab(const gchar *url);
void close_tab(WebKitWebView *web_view);
void reopen_last_closed_tab();

int tab_id_for_webview(WebKitWebView *web_view);
WebKitWebView *tab_webview_for_id(int id);
std::vector<KiteTabInfo> tabs_list();
bool tab_activate_id(int id);
bool tab_close_id(int id);
