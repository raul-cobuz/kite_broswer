#include "tabs.h"

#include <string>
#include <vector>

#include "adblock.h"
#include "browser_window.h"
#include "downloads.h"
#include "errors.h"
#include "find_bar.h"
#include "history.h"
#include "kite_pages.h"
#include "permissions.h"
#include "settings.h"
#include "url_utils.h"

static WebKitWebContext *incognito_context = nullptr;
static std::vector<std::string> closed_tabs;
static const size_t max_closed_tabs = 10;

static void create_tab(const gchar *url, gboolean incognito);

static bool is_trackable_url(const gchar *uri) {
    return uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
                   g_str_has_prefix(uri, "file://"));
}

static WebKitWebContext *get_incognito_context() {
    if (!incognito_context) {
        incognito_context = webkit_web_context_new_ephemeral();
        init_kite_pages(incognito_context);
        g_signal_connect(incognito_context, "download-started", G_CALLBACK(on_download_started), NULL);
    }
    return incognito_context;
}

static void apply_webview_settings(WebKitWebView *web_view) {
    WebKitSettings *web_settings = webkit_web_view_get_settings(web_view);
    webkit_settings_set_enable_developer_extras(web_settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(web_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
    webkit_settings_set_enable_webgl(web_settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(web_settings, TRUE);
    webkit_web_view_set_settings(web_view, web_settings);
}

static void apply_site_zoom(WebKitWebView *web_view) {
    if (webview_is_incognito(web_view))
        return;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!is_trackable_url(uri))
        return;

    std::string host = url_host(uri);
    if (host.empty())
        return;

    double zoom = settings_site_zoom(host.c_str());
    if (zoom != webkit_web_view_get_zoom_level(web_view))
        webkit_web_view_set_zoom_level(web_view, zoom);
}

static void remember_closed_tab(WebKitWebView *web_view) {
    if (!web_view || webview_is_incognito(web_view))
        return;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri || !*uri || g_str_has_prefix(uri, "kite://newtab") || g_str_has_prefix(uri, "about:"))
        return;

    closed_tabs.push_back(uri);
    if (closed_tabs.size() > max_closed_tabs)
        closed_tabs.erase(closed_tabs.begin());
}

void reopen_last_closed_tab() {
    if (closed_tabs.empty())
        return;
    std::string url = closed_tabs.back();
    closed_tabs.pop_back();
    create_new_tab(url.c_str());
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_label"));

    if (load_event == WEBKIT_LOAD_STARTED && label) {
        gtk_label_set_text(GTK_LABEL(label), "Loading...");
        gtk_widget_set_tooltip_text(label, "Loading...");
    }

    if (load_event == WEBKIT_LOAD_COMMITTED) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        if (uri)
            g_object_set_data_full(G_OBJECT(web_view), "kite_requested_url", g_strdup(uri), g_free);

        if (!webview_is_incognito(web_view) && is_trackable_url(uri))
            history_record_visit(uri, "");

        apply_site_zoom(web_view);
        update_bookmark_button(web_view);
        save_browser_session();
    }

    if (load_event == WEBKIT_LOAD_FINISHED) {
        const gchar *title = webkit_web_view_get_title(web_view);
        if (!title || !*title)
            title = webkit_web_view_get_uri(web_view);
        if (!title || !*title)
            title = "New Tab";
        if (label) {
            gtk_label_set_text(GTK_LABEL(label), title);
            gtk_widget_set_tooltip_text(label, title);
        }

        const gchar *uri = webkit_web_view_get_uri(web_view);
        if (!webview_is_incognito(web_view) && is_trackable_url(uri)) {
            const gchar *page_title = webkit_web_view_get_title(web_view);
            if (page_title && *page_title)
                history_update_title(uri, page_title);
        }

        update_bookmark_button(web_view);
        save_browser_session();
    }

    if (web_view != get_active_webview())
        return;

    if (load_event == WEBKIT_LOAD_COMMITTED && browser.url_entry) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        gboolean entry_focused = gtk_widget_has_focus(browser.url_entry);
        if (!browser.url_entry_user_edited) {
            set_url_entry_text(uri);
            if (entry_focused)
                gtk_editable_select_region(GTK_EDITABLE(browser.url_entry), 0, -1);
        } else if (!entry_focused) {
            // A user-typed navigation committed: from here on the URL bar
            // can follow redirects and subsequent loads again.
            set_url_entry_text(uri);
        }
    }

    if (load_event == WEBKIT_LOAD_FINISHED && browser.find_box && gtk_widget_get_visible(browser.find_box)) {
        const gchar *query = (const gchar *)g_object_get_data(G_OBJECT(web_view), "find_query");
        if (query && *query) {
            run_find(web_view, query);
        } else if (browser.find_status) {
            gtk_label_set_text(GTK_LABEL(browser.find_status), "");
        }
    }

    if (load_event == WEBKIT_LOAD_STARTED || load_event == WEBKIT_LOAD_FINISHED ||
        load_event == WEBKIT_LOAD_COMMITTED) {
        update_reload_button(web_view);
        update_progress(web_view);
        update_window_title(web_view);
    }
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, GtkWidget *label) {
    const gchar *title = webkit_web_view_get_title(web_view);
    if (!title)
        return;

    gtk_label_set_text(GTK_LABEL(label), title);
    gtk_widget_set_tooltip_text(label, title);

    if (!webview_is_incognito(web_view)) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        if (is_trackable_url(uri) && *title)
            history_update_title(uri, title);
    }

    if (web_view == get_active_webview()) {
        update_window_title(web_view);
        update_bookmark_button(web_view);
    }
}

static void on_tab_close_clicked(GtkButton *btn, GtkWidget *child_page) {
    close_tab(WEBKIT_WEB_VIEW(child_page));
}

static GtkWidget *on_create_tab(WebKitWebView *web_view, WebKitNavigationAction *action, gpointer user_data) {
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = webkit_uri_request_get_uri(req);
    create_tab(uri && *uri ? uri : KITE_NEWTAB_URL, webview_is_incognito(web_view));
    return GTK_WIDGET(get_active_webview());
}

static void create_tab(const gchar *url, gboolean incognito) {
    if (!url || !*url)
        url = KITE_NEWTAB_URL;

    WebKitWebView *web_view;
    if (incognito) {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", get_incognito_context(),
                                                "user-content-manager", adblock_content_manager(), NULL));
    } else {
        web_view = WEBKIT_WEB_VIEW(
            g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", adblock_content_manager(), NULL));
    }
    g_object_set_data(G_OBJECT(web_view), "is_incognito", GINT_TO_POINTER(incognito ? 1 : 0));
    g_object_set_data_full(G_OBJECT(web_view), "kite_requested_url", g_strdup(url), g_free);

    apply_webview_settings(web_view);

    GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *tab_label = gtk_label_new(incognito ? "Incognito" : "Loading...");
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 18);
    gtk_label_set_width_chars(GTK_LABEL(tab_label), 10);
    g_object_set_data(G_OBJECT(web_view), "tab_label", tab_label);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);

    gtk_box_pack_start(GTK_BOX(tab_box), tab_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);

    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(web_view, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_with_tls_errors), NULL);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(web_view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(on_progress_changed), NULL);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title_changed), tab_label);
    g_signal_connect(web_view, "create", G_CALLBACK(on_create_tab), NULL);

    connect_find_handlers(web_view);

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), GTK_WIDGET(web_view));

    gtk_widget_show_all(tab_box);
    gtk_widget_show(GTK_WIDGET(web_view));

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view), tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view), TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(browser.notebook), index);
    webkit_web_view_load_uri(web_view, url);
}

void create_new_tab(const gchar *url) {
    create_tab(url, FALSE);
}

void create_incognito_tab(const gchar *url) {
    create_tab(url, TRUE);
}

void close_tab(WebKitWebView *web_view) {
    if (!web_view || !browser.notebook)
        return;

    remember_closed_tab(web_view);

    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view));
    gboolean removed = FALSE;
    if (page_num >= 0) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(browser.notebook), page_num);
        removed = TRUE;
    }

    save_browser_session();
    if (removed && gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook)) == 0 && browser.main_window)
        gtk_widget_destroy(browser.main_window);
}
