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
    GtkWidget *btn_bookmark = nullptr;
    GtkWidget *btn_split = nullptr;
    GtkWidget *split_pane = nullptr;
    GtkWidget *split_notebook = nullptr;
    GtkWidget *progress_bar = nullptr;
    GtkWidget *find_box = nullptr;
    GtkWidget *find_entry = nullptr;
    GtkWidget *find_status = nullptr;
    GtkWidget *search_notice = nullptr;

    bool url_entry_user_edited = false;
    bool updating_url_entry = false;
    bool updating_bookmark_button = false;
    bool restoring_session = false;
    bool split_focus_secondary = false;
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
void update_bookmark_button(WebKitWebView *web_view);
void save_browser_session();
void switch_profile_and_restart(const gchar *id);
void show_google_fallback_notice(const gchar *query);
void refresh_search_placeholder();

// Two tabs can share the window side by side. The split view keeps the current
// tab in the primary notebook and moves one other tab to the secondary pane.
bool split_view_active();
void toggle_split_view();
bool activate_webview(WebKitWebView *web_view);
GtkNotebook *notebook_for_webview(WebKitWebView *web_view);
void split_view_note_focus(WebKitWebView *web_view);
void split_view_after_tab_removed(GtkNotebook *notebook);

void create_browser_window(const gchar *startup_url = nullptr);
