#include "browser_window.h"

#include <gdk/gdkkeysyms.h>

#include <string>
#include <vector>

#include "dark_mode.h"
#include "downloads.h"
#include "find_bar.h"
#include "history.h"
#include "kite_pages.h"
#include "privacy.h"
#include "session.h"
#include "settings.h"
#include "tabs.h"
#include "url_utils.h"

BrowserState browser;

static GtkListStore *completion_store = nullptr;

static void clear_omnibox_completions() {
    if (completion_store)
        gtk_list_store_clear(completion_store);
}

WebKitWebView *get_active_webview() {
    if (!browser.notebook)
        return nullptr;

    gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(browser.notebook));
    if (page_num < 0)
        return nullptr;
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser.notebook), page_num);
    return child ? WEBKIT_WEB_VIEW(child) : nullptr;
}

gboolean webview_is_incognito(WebKitWebView *web_view) {
    if (!web_view)
        return FALSE;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "is_incognito"));
}

static void update_url_entry_icon(const gchar *uri) {
    if (!browser.url_entry)
        return;

    const gchar *icon = nullptr;
    const gchar *tooltip = nullptr;
    if (uri && g_str_has_prefix(uri, "https://")) {
        icon = "channel-secure-symbolic";
        tooltip = "Secure connection";
    } else if (uri && g_str_has_prefix(uri, "http://")) {
        icon = "channel-insecure-symbolic";
        tooltip = "Connection is not secure";
    } else if (uri && g_str_has_prefix(uri, "file://")) {
        icon = "text-x-generic-symbolic";
        tooltip = "Local file";
    } else if (uri && g_str_has_prefix(uri, "kite://")) {
        icon = "applications-internet-symbolic";
        tooltip = "Kite page";
    }

    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(browser.url_entry), GTK_ENTRY_ICON_PRIMARY, icon);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(browser.url_entry), GTK_ENTRY_ICON_PRIMARY, tooltip);
}

void set_url_entry_text(const gchar *text) {
    if (!browser.url_entry)
        return;
    const gchar *display = text;
    if (display && g_str_has_prefix(display, KITE_NEWTAB_URL))
        display = "";
    browser.updating_url_entry = true;
    gtk_entry_set_text(GTK_ENTRY(browser.url_entry), display ? display : "");
    browser.updating_url_entry = false;
    browser.url_entry_user_edited = false;
    update_url_entry_icon(text);
    clear_omnibox_completions();
}

static bool can_bookmark_uri(const gchar *uri) {
    return uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
                   g_str_has_prefix(uri, "file://"));
}

void update_bookmark_button(WebKitWebView *web_view) {
    if (!browser.btn_bookmark)
        return;
    if (web_view && web_view != get_active_webview())
        return;

    const gchar *uri = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    gboolean incognito = webview_is_incognito(web_view);
    gboolean can_bookmark = can_bookmark_uri(uri) && !incognito;
    gboolean marked = can_bookmark && bookmark_exists(uri);

    browser.updating_bookmark_button = true;
    gtk_widget_set_sensitive(browser.btn_bookmark, can_bookmark);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_bookmark), marked);
    browser.updating_bookmark_button = false;

    gtk_button_set_image(GTK_BUTTON(browser.btn_bookmark),
                         gtk_image_new_from_icon_name(marked ? "starred-symbolic" : "non-starred-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    if (incognito)
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmarks are not saved in incognito");
    else if (marked)
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Remove bookmark (Ctrl+D)");
    else
        gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmark this page (Ctrl+D)");
}

static bool is_startup_newtab(WebKitWebView *web_view) {
    if (!web_view || !g_object_get_data(G_OBJECT(web_view), "kite_startup_tab"))
        return false;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    return uri && g_str_has_prefix(uri, KITE_NEWTAB_URL);
}

void save_browser_session() {
    if (!browser.notebook || browser.restoring_session)
        return;

    SessionState state;
    gint current = gtk_notebook_get_current_page(GTK_NOTEBOOK(browser.notebook));
    gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));

    for (gint i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(browser.notebook), i);
        if (!child || !WEBKIT_IS_WEB_VIEW(child))
            continue;
        WebKitWebView *web_view = WEBKIT_WEB_VIEW(child);
        if (webview_is_incognito(web_view))
            continue;
        // The fresh start page opened at launch is not part of the restored
        // session; otherwise every launch would accumulate another blank tab.
        if (is_startup_newtab(web_view))
            continue;

        if (i == current)
            state.active_index = (int)state.urls.size();

        const gchar *uri = webkit_web_view_get_uri(web_view);
        const gchar *requested = (const gchar *)g_object_get_data(G_OBJECT(web_view), "kite_requested_url");
        if (uri && *uri)
            state.urls.push_back(uri);
        else if (requested && *requested)
            state.urls.push_back(requested);
        else
            state.urls.push_back(KITE_NEWTAB_URL);
    }

    if (state.active_index >= (int)state.urls.size())
        state.active_index = 0;
    session_save(state);
}

static void on_url_entry_changed(GtkEditable *editable, gpointer user_data) {
    if (browser.updating_url_entry)
        return;

    browser.url_entry_user_edited = true;
    if (!completion_store)
        return;

    if (webview_is_incognito(get_active_webview())) {
        clear_omnibox_completions();
        return;
    }

    const gchar *text = gtk_entry_get_text(GTK_ENTRY(browser.url_entry));
    gtk_list_store_clear(completion_store);
    if (!text || !*text)
        return;

    std::vector<HistoryEntry> matches = history_completions(text, 12);
    for (const auto &entry : matches) {
        GtkTreeIter iter;
        std::string display = entry.title.empty() ? entry.url : entry.title + " \xE2\x80\x94 " + entry.url;
        gtk_list_store_append(completion_store, &iter);
        gtk_list_store_set(completion_store, &iter, 0, display.c_str(), 1, entry.url.c_str(), -1);
    }
}

static void navigate_url_entry(const gchar *text) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view || !text || !*text)
        return;

    std::string target_url = resolve_url_input(text);
    g_printerr("[Kite DEBUG] navigate_url_entry text=%s -> target=%s\n", text, target_url.c_str());
    webkit_web_view_load_uri(web_view, target_url.c_str());
    gtk_widget_grab_focus(GTK_WIDGET(web_view));
}

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    navigate_url_entry(gtk_entry_get_text(entry));
}

static gboolean on_completion_match(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter,
                                    gpointer user_data) {
    gchar *url = nullptr;
    gtk_tree_model_get(model, iter, 1, &url, -1);
    if (url && *url) {
        set_url_entry_text(url);
        WebKitWebView *web_view = get_active_webview();
        if (web_view) {
            webkit_web_view_load_uri(web_view, url);
            gtk_widget_grab_focus(GTK_WIDGET(web_view));
        }
    }
    g_free(url);
    return TRUE;
}

static gboolean completion_match_always(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                        gpointer user_data) {
    return TRUE;
}

static void setup_omnibox_completion() {
    completion_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(completion_store));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_popup_completion(completion, TRUE);
    gtk_entry_completion_set_inline_completion(completion, FALSE);
    gtk_entry_completion_set_popup_set_width(completion, TRUE);
    gtk_entry_completion_set_minimum_key_length(completion, 1);
    gtk_entry_completion_set_match_func(completion, completion_match_always, nullptr, nullptr);
    g_signal_connect(completion, "match-selected", G_CALLBACK(on_completion_match), nullptr);
    gtk_entry_set_completion(GTK_ENTRY(browser.url_entry), completion);
}

void update_reload_button(WebKitWebView *web_view) {
    if (!browser.btn_reload || !web_view)
        return;
    gboolean loading = webkit_web_view_is_loading(web_view);
    GtkWidget *image = gtk_image_new_from_icon_name(loading ? "process-stop-symbolic" : "view-refresh-symbolic",
                                                   GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(browser.btn_reload), image);
    gtk_widget_set_tooltip_text(browser.btn_reload, loading ? "Stop loading" : "Reload");
}

void update_window_title(WebKitWebView *web_view) {
    if (!browser.main_window)
        return;
    const gchar *title = web_view ? webkit_web_view_get_title(web_view) : nullptr;
    if (!title || !*title)
        title = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    gtk_window_set_title(GTK_WINDOW(browser.main_window), (title && *title) ? title : "Kite Browser");
}

void update_progress(WebKitWebView *web_view) {
    if (!browser.progress_bar || !web_view)
        return;

    gboolean loading = webkit_web_view_is_loading(web_view);
    gtk_widget_set_visible(browser.progress_bar, loading);
    if (loading)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(browser.progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

void on_progress_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(object);
    if (web_view != get_active_webview())
        return;
    if (browser.progress_bar && webkit_web_view_is_loading(web_view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(browser.progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

void focus_url_bar() {
    if (!browser.url_entry)
        return;
    gtk_widget_grab_focus(browser.url_entry);
    gtk_editable_select_region(GTK_EDITABLE(browser.url_entry), 0, -1);
}

void close_current_tab() {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        close_tab(web_view);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_go_back(web_view);
}

static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_go_forward(web_view);
}

static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;
    if (webkit_web_view_is_loading(web_view))
        webkit_web_view_stop_loading(web_view);
    else
        webkit_web_view_reload(web_view);
}

static void on_home_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, settings_homepage());
}

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    create_new_tab(KITE_NEWTAB_URL);
    focus_url_bar();
}

static void on_extensions_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view)
        webkit_web_view_load_uri(web_view, KITE_EXTENSIONS_URL);
    else
        create_new_tab(KITE_EXTENSIONS_URL);
}

static void on_bookmark_toggled(GtkToggleButton *button, gpointer user_data) {
    if (browser.updating_bookmark_button)
        return;

    WebKitWebView *web_view = get_active_webview();
    const gchar *uri = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    if (!web_view || webview_is_incognito(web_view) || !can_bookmark_uri(uri)) {
        update_bookmark_button(web_view);
        return;
    }

    const gchar *title = webkit_web_view_get_title(web_view);
    if (gtk_toggle_button_get_active(button))
        bookmark_add(uri, title ? title : "");
    else
        bookmark_remove(uri);
    update_bookmark_button(web_view);
}

static void set_active_zoom(double level) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;

    level = CLAMP(level, 0.25, 5.0);
    webkit_web_view_set_zoom_level(web_view, level);
    if (webview_is_incognito(web_view))
        return;

    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!can_bookmark_uri(uri))
        return;
    std::string host = url_host(uri);
    if (host.empty())
        return;

    if (level > 0.999 && level < 1.001)
        settings_clear_site_zoom(host.c_str());
    else
        settings_set_site_zoom(host.c_str(), level);
    save_settings();
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
    save_browser_session();
    return FALSE;
}

static void on_page_reordered(GtkNotebook *notebook, GtkWidget *child, guint new_page_num, gpointer user_data) {
    save_browser_session();
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer user_data) {
    if (!page || !WEBKIT_IS_WEB_VIEW(page))
        return;
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(page);

    const gchar *uri = webkit_web_view_get_uri(web_view);
    set_url_entry_text(uri);
    clear_omnibox_completions();

    GtkStyleContext *context = gtk_widget_get_style_context(browser.url_entry);
    if (webview_is_incognito(web_view)) {
        gtk_style_context_add_class(context, "incognito-mode");
    } else {
        gtk_style_context_remove_class(context, "incognito-mode");
    }

    update_reload_button(web_view);
    update_progress(web_view);
    update_window_title(web_view);
    update_bookmark_button(web_view);
    update_privacy_button(web_view);
    sync_find_bar(web_view);
    save_browser_session();
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    // F12 sau Ctrl+Shift+I pentru Web Inspector
    if (event->keyval == GDK_KEY_F12 ||
        ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) &&
         (event->keyval == GDK_KEY_I || event->keyval == GDK_KEY_i))) {
        WebKitWebView *web_view = get_active_webview();
        if (web_view)
            webkit_web_inspector_show(webkit_web_view_get_inspector(web_view));
        return TRUE;
    }

    // Escape pentru ascunderea barei de Ctrl+F
    if (event->keyval == GDK_KEY_Escape && browser.find_box && gtk_widget_get_visible(browser.find_box)) {
        close_find_bar();
        return TRUE;
    }

    // F9 pentru Dark Mode
    if (event->keyval == GDK_KEY_F9) {
        toggle_dark_mode();
        return TRUE;
    }

    if (event->state & GDK_CONTROL_MASK) {
        // Ctrl+Tab / Ctrl+Shift+Tab — cycle tabs
        if (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab) {
            gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));
            if (n > 0) {
                gint cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(browser.notebook));
                gboolean backward =
                    (event->state & GDK_SHIFT_MASK) || event->keyval == GDK_KEY_ISO_Left_Tab;
                gint next = backward ? (cur - 1 + n) % n : (cur + 1) % n;
                gtk_notebook_set_current_page(GTK_NOTEBOOK(browser.notebook), next);
            }
            return TRUE;
        }

        // Ctrl + Shift + D pentru Dark Mode
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_D || event->keyval == GDK_KEY_d)) {
            toggle_dark_mode();
            return TRUE;
        }

        // Ctrl + Shift + P pentru Incognito
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_P || event->keyval == GDK_KEY_p)) {
            create_incognito_tab(KITE_NEWTAB_URL);
            focus_url_bar();
            return TRUE;
        }

        // Ctrl + Shift + T — reopen the most recently closed tab
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_T || event->keyval == GDK_KEY_t)) {
            reopen_last_closed_tab();
            focus_url_bar();
            return TRUE;
        }

        // Ctrl + Shift + O — bookmarks manager
        if ((event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_O || event->keyval == GDK_KEY_o)) {
            create_new_tab(KITE_BOOKMARKS_URL);
            focus_url_bar();
            return TRUE;
        }

        switch (event->keyval) {
            case GDK_KEY_f:
            case GDK_KEY_F:
                sync_find_bar(get_active_webview());
                gtk_widget_show(browser.find_box);
                gtk_widget_grab_focus(browser.find_entry);
                gtk_editable_select_region(GTK_EDITABLE(browser.find_entry), 0, -1);
                if (*gtk_entry_get_text(GTK_ENTRY(browser.find_entry)) && get_active_webview())
                    run_find(get_active_webview(), gtk_entry_get_text(GTK_ENTRY(browser.find_entry)));
                return TRUE;
            case GDK_KEY_t:
                create_new_tab(KITE_NEWTAB_URL);
                focus_url_bar();
                return TRUE;
            case GDK_KEY_h:
            case GDK_KEY_H:
                create_new_tab(KITE_HISTORY_URL);
                focus_url_bar();
                return TRUE;
            case GDK_KEY_d:
            case GDK_KEY_D:
                if (browser.btn_bookmark && gtk_widget_get_sensitive(browser.btn_bookmark))
                    gtk_button_clicked(GTK_BUTTON(browser.btn_bookmark));
                return TRUE;
            case GDK_KEY_w:
                close_current_tab();
                return TRUE;
            case GDK_KEY_l:
                focus_url_bar();
                return TRUE;
            case GDK_KEY_r:
            case GDK_KEY_R: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    webkit_web_view_reload(web_view);
                return TRUE;
            }
            case GDK_KEY_plus:
            case GDK_KEY_equal: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    set_active_zoom(webkit_web_view_get_zoom_level(web_view) + 0.1);
                return TRUE;
            }
            case GDK_KEY_minus: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    set_active_zoom(webkit_web_view_get_zoom_level(web_view) - 0.1);
                return TRUE;
            }
            case GDK_KEY_0:
                set_active_zoom(1.0);
                return TRUE;
        }
    }
    return FALSE;
}

void create_browser_window() {
    browser.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.main_window), "kite-window");
    if (settings_dark_mode())
        gtk_style_context_add_class(gtk_widget_get_style_context(browser.main_window), "kite-dark");
    g_signal_connect(browser.main_window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(browser.main_window, "delete-event", G_CALLBACK(on_window_delete), NULL);
    gtk_window_set_default_size(GTK_WINDOW(browser.main_window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(browser.main_window), "Kite Browser");
    g_signal_connect(browser.main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(browser.main_window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "kite-toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_reload = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_always_show_image(GTK_BUTTON(browser.btn_reload), TRUE);
    GtkWidget *btn_home = gtk_button_new_from_icon_name("go-home-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_new_tab = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_extensions =
        gtk_button_new_from_icon_name("applications-other-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_bookmark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_bookmark),
                         gtk_image_new_from_icon_name("non-starred-symbolic", GTK_ICON_SIZE_BUTTON));
    browser.btn_dark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_dark),
                         gtk_image_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_dark), settings_dark_mode());

    gtk_widget_set_tooltip_text(browser.btn_reload, "Reload");
    gtk_widget_set_tooltip_text(btn_home, "Home");
    gtk_widget_set_tooltip_text(btn_new_tab, "New tab (Ctrl+T)");
    gtk_widget_set_tooltip_text(btn_extensions, "Extensions");
    gtk_widget_set_tooltip_text(browser.btn_bookmark, "Bookmark this page (Ctrl+D)");
    gtk_widget_set_tooltip_text(browser.btn_dark, "Toggle Dark Mode (F9 / Ctrl+Shift+D)");

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(browser.btn_reload, "clicked", G_CALLBACK(on_reload_clicked), NULL);
    g_signal_connect(btn_home, "clicked", G_CALLBACK(on_home_clicked), NULL);
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(btn_extensions, "clicked", G_CALLBACK(on_extensions_clicked), NULL);
    g_signal_connect(browser.btn_bookmark, "toggled", G_CALLBACK(on_bookmark_toggled), NULL);
    g_signal_connect(browser.btn_dark, "toggled", G_CALLBACK(on_dark_mode_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_home, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_extensions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_bookmark, FALSE, FALSE, 0);

    browser.url_entry = gtk_entry_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.url_entry), "kite-omnibox");
    gtk_entry_set_placeholder_text(GTK_ENTRY(browser.url_entry), "Search with Google or type a URL");
    gtk_box_pack_start(GTK_BOX(toolbar), browser.url_entry, TRUE, TRUE, 2);
    g_signal_connect(browser.url_entry, "activate", G_CALLBACK(on_url_activate), NULL);
    g_signal_connect(browser.url_entry, "changed", G_CALLBACK(on_url_entry_changed), NULL);
    setup_omnibox_completion();

    GtkWidget *btn_privacy = create_privacy_button();
    if (btn_privacy)
        gtk_box_pack_start(GTK_BOX(toolbar), btn_privacy, FALSE, FALSE, 0);

    GtkWidget *btn_downloads = create_downloads_button();
    if (btn_downloads)
        gtk_box_pack_start(GTK_BOX(toolbar), btn_downloads, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_dark, FALSE, FALSE, 0);

    browser.progress_bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.progress_bar), "kite-progress");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(browser.progress_bar), FALSE);
    gtk_widget_set_size_request(browser.progress_bar, -1, 3);
    gtk_box_pack_start(GTK_BOX(vbox), browser.progress_bar, FALSE, FALSE, 0);

    browser.notebook = gtk_notebook_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.notebook), "kite-notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(browser.notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), browser.notebook, TRUE, TRUE, 0);
    g_signal_connect(browser.notebook, "switch-page", G_CALLBACK(on_tab_switched), NULL);
    g_signal_connect(browser.notebook, "page-reordered", G_CALLBACK(on_page_reordered), NULL);

    GtkWidget *find_box = create_find_bar();
    gtk_box_pack_start(GTK_BOX(vbox), find_box, FALSE, FALSE, 2);

    browser.restoring_session = true;
    SessionState saved;
    bool restored = settings_restore_session() && session_load(&saved);
    if (restored) {
        for (const auto &url : saved.urls)
            create_new_tab(url.c_str());
    }

    // Always open the Kite start page. When a session was restored it becomes
    // the first, active tab, so the browser opens with a proper start page
    // instead of dropping straight into the last visited site.
    create_new_tab(KITE_NEWTAB_URL);
    WebKitWebView *start_view = get_active_webview();
    if (start_view) {
        g_object_set_data(G_OBJECT(start_view), "kite_startup_tab", GINT_TO_POINTER(1));
        if (restored) {
            gint start_page = gtk_notebook_page_num(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(start_view));
            if (start_page > 0)
                gtk_notebook_reorder_child(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(start_view), 0);
            gtk_notebook_set_current_page(GTK_NOTEBOOK(browser.notebook), 0);
        }
    }
    browser.restoring_session = false;

    gtk_widget_show_all(browser.main_window);
    gtk_widget_hide(browser.find_box);
    gtk_widget_hide(browser.progress_bar);
    focus_url_bar();
}
