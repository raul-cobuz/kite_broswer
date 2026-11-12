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
    bool pinned = false;
    bool muted = false;
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

// Tab context-menu operations.
bool tab_is_pinned(WebKitWebView *web_view);
void tab_set_pinned(WebKitWebView *web_view, bool pinned);
void tab_toggle_pinned(WebKitWebView *web_view);
void tab_duplicate(WebKitWebView *web_view);
void tab_copy_url(WebKitWebView *web_view);
void tab_toggle_muted(WebKitWebView *web_view);
void tab_close_others(WebKitWebView *web_view);
void tab_close_to_right(WebKitWebView *web_view);
void tabs_enforce_pinned_order(GtkNotebook *notebook);
void tabs_install_notebook_context_menu(GtkNotebook *notebook);
