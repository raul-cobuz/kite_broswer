#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

// Widgets and transient UI state shared by the browser modules. The window is a
// single-instance application, so one process-wide state object keeps the
// refactor close to the original behavior.
struct BrowserState {
    GtkWidget *main_window = nullptr;
    GtkWidget *notebook = nullptr;
    GtkWidget *url_entry = nullptr;
    GtkWidget *btn_reload = nullptr;
    GtkWidget *btn_dark = nullptr;
    GtkWidget *progress_bar = nullptr;
    GtkWidget *find_box = nullptr;
    GtkWidget *find_entry = nullptr;
    GtkWidget *find_status = nullptr;

    bool url_entry_user_edited = false;
    bool updating_url_entry = false;
};

extern BrowserState browser;

WebKitWebView *get_active_webview();
gboolean webview_is_incognito(WebKitWebView *web_view);

void set_url_entry_text(const gchar *text);
void update_reload_button(WebKitWebView *web_view);
void update_progress(WebKitWebView *web_view);
void update_window_title(WebKitWebView *web_view);
void on_progress_changed(GObject *object, GParamSpec *pspec, gpointer user_data);

void focus_url_bar();
void close_current_tab();
void create_browser_window();
