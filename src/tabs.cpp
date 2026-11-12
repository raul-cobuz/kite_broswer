#include "tabs.h"

#include <string>
#include <vector>

#include "adblock.h"
#include "app_context.h"
#include "browser_window.h"
#include "downloads.h"
#include "errors.h"
#include "extensions.h"
#include "find_bar.h"
#include "history.h"
#include "kite_pages.h"
#include "permissions.h"
#include "privacy.h"
#include "settings.h"
#include "url_utils.h"

static WebKitWebContext *incognito_context = nullptr;
static std::vector<std::string> closed_tabs;
static const size_t max_closed_tabs = 10;
static int next_tab_id = 1;

static WebKitWebView *create_tab_view(const gchar *url, gboolean incognito, WebKitWebView *related,
                                      gboolean load_url);

static bool is_trackable_url(const gchar *uri) {
    return uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") ||
                   g_str_has_prefix(uri, "file://"));
}

static WebKitWebContext *get_incognito_context() {
    if (!incognito_context) {
        incognito_context = webkit_web_context_new_ephemeral();
        privacy_apply_context(incognito_context);
        init_kite_pages(incognito_context);
        extensions_register_scheme(incognito_context);
        g_signal_connect(incognito_context, "download-started", G_CALLBACK(on_download_started), NULL);
    }
    return incognito_context;
}

static void apply_webview_settings(WebKitWebView *web_view) {
    WebKitSettings *web_settings = webkit_web_view_get_settings(web_view);
    // WebKitGTK's stock UA advertises "Version/60.5 Safari/605.1.15", which is
    // not a real Safari version and makes some sites (notably Google) treat
    // the browser as automated. Keep the WebKit engine token but report a
    // current Safari version on the same platform.
    static const char *KITE_USER_AGENT =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/18.3 Safari/605.1.15";
    webkit_settings_set_user_agent(web_settings, KITE_USER_AGENT);
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

typedef struct {
    WebKitWebView *view;
    gchar *query;
} GoogleFallbackRequest;

static gboolean run_google_fallback(gpointer data) {
    GoogleFallbackRequest *request = (GoogleFallbackRequest *)data;
    g_object_set_data(G_OBJECT(request->view), "kite_google_fallback_pending", nullptr);

    if (request->query && *request->query) {
        std::string fallback = search_url_for_engine(request->query, "duckduckgo");
        g_printerr("[Kite] Google verification page detected; retrying search with DuckDuckGo: %s\n",
                   request->query);
        webkit_web_view_load_uri(request->view, fallback.c_str());
        show_google_fallback_notice(request->query);
    }

    g_object_unref(request->view);
    g_free(request->query);
    g_free(request);
    return G_SOURCE_REMOVE;
}

static void schedule_google_fallback(WebKitWebView *web_view, const std::string &query) {
    if (!web_view || query.empty() || !settings_google_fallback())
        return;
    if (g_object_get_data(G_OBJECT(web_view), "kite_google_fallback_pending"))
        return;

    g_object_set_data(G_OBJECT(web_view), "kite_google_fallback_pending", GINT_TO_POINTER(1));
    GoogleFallbackRequest *request = g_new0(GoogleFallbackRequest, 1);
    request->view = WEBKIT_WEB_VIEW(g_object_ref(web_view));
    request->query = g_strdup(query.c_str());
    g_idle_add(run_google_fallback, request);
}

// Google serves a /sorry/ challenge page when it decides a search is automated.
// Intercept the navigation before it commits, recover the query from the
// continue= parameter, and retry it on DuckDuckGo instead.
static gboolean on_decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType decision_type, gpointer user_data) {
    (void)user_data;
    if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        decision_type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;

    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    if (!action)
        return FALSE;
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    const gchar *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (!uri || !is_google_challenge_url(uri))
        return FALSE;

    std::string query = google_challenge_query(uri);
    if (query.empty() || !settings_google_fallback())
        return FALSE;

    webkit_policy_decision_ignore(decision);
    schedule_google_fallback(web_view, query);
    return TRUE;
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    const gchar *uri = webkit_web_view_get_uri(web_view);
    g_printerr("[Kite DEBUG] load-changed view=%p event=%d uri=%s\n", (void *)web_view, (int)load_event, uri ? uri : "(null)");

    if (uri && is_google_challenge_url(uri) && settings_google_fallback()) {
        std::string query = google_challenge_query(uri);
        if (!query.empty()) {
            if (webkit_web_view_is_loading(web_view))
                webkit_web_view_stop_loading(web_view);
            schedule_google_fallback(web_view, query);
            return;
        }
    }

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
        if (web_view == get_active_webview())
            update_privacy_button(web_view);
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

    if (web_view == get_active_webview() || split_view_active()) {
        update_window_title(get_active_webview());
        update_bookmark_button(get_active_webview());
    }
}

static gboolean on_webview_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
    (void)event;
    (void)user_data;
    split_view_note_focus(WEBKIT_WEB_VIEW(widget));
    return FALSE;
}

static gboolean on_tab_box_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget;
    if (event->button == 1)
        split_view_note_focus(WEBKIT_WEB_VIEW(user_data));
    return FALSE;
}

static void on_tab_close_clicked(GtkButton *btn, GtkWidget *child_page) {
    close_tab(WEBKIT_WEB_VIEW(child_page));
}

static GtkWidget *on_create_tab(WebKitWebView *web_view, WebKitNavigationAction *action, gpointer user_data) {
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = req ? webkit_uri_request_get_uri(req) : nullptr;

    // WebKit starts the navigation itself in the view returned from this
    // signal, so don't load the URL a second time here. The view must also be
    // related to the one that requested the new window, and the return value
    // is transfer-full: the notebook already owns a reference, so hand WebKit
    // an extra one.
    WebKitWebView *created = create_tab_view(uri && *uri ? uri : KITE_NEWTAB_URL,
                                             webview_is_incognito(web_view), web_view, FALSE);
    return GTK_WIDGET(g_object_ref(created));
}

static WebKitWebView *create_tab_view(const gchar *url, gboolean incognito, WebKitWebView *related,
                                      gboolean load_url) {
    if (!url || !*url)
        url = KITE_NEWTAB_URL;

    WebKitWebView *web_view;
    if (related) {
        web_view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_related_view(related));
    } else if (incognito) {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", get_incognito_context(),
                                                "user-content-manager", adblock_content_manager(), NULL));
    } else {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", app_webkit_context(),
                                                "user-content-manager", adblock_content_manager(), NULL));
    }
    g_object_set_data(G_OBJECT(web_view), "is_incognito", GINT_TO_POINTER(incognito ? 1 : 0));
    g_object_set_data(G_OBJECT(web_view), "kite_tab_id", GINT_TO_POINTER(next_tab_id++));
    g_object_set_data_full(G_OBJECT(web_view), "kite_requested_url", g_strdup(url), g_free);
    g_printerr("[Kite DEBUG] create_tab url=%s incognito=%d view_ctx=%p app_ctx=%p\n", url ? url : "(null)", (int)incognito, (void *)webkit_web_view_get_context(web_view), (void *)app_webkit_context());

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
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(web_view, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_with_tls_errors), NULL);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(web_view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(on_progress_changed), NULL);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title_changed), tab_label);
    g_signal_connect(web_view, "create", G_CALLBACK(on_create_tab), NULL);
    g_signal_connect(web_view, "focus-in-event", G_CALLBACK(on_webview_focus_in), NULL);
    g_signal_connect(tab_box, "button-press-event", G_CALLBACK(on_tab_box_button_press), web_view);

    connect_find_handlers(web_view);

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), GTK_WIDGET(web_view));
    gtk_widget_show_all(tab_box);
    gtk_widget_show(GTK_WIDGET(web_view));

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view), tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(browser.notebook), GTK_WIDGET(web_view), TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(browser.notebook), index);
    if (load_url)
        webkit_web_view_load_uri(web_view, url);
    return web_view;
}

static void create_tab(const gchar *url, gboolean incognito) {
    create_tab_view(url, incognito, nullptr, TRUE);
}

void create_new_tab(const gchar *url) {
    create_tab(url, FALSE);
}

void create_incognito_tab(const gchar *url) {
    create_tab(url, TRUE);
}

void close_tab(WebKitWebView *web_view) {
    if (!web_view)
        return;

    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return;

    remember_closed_tab(web_view);

    gint page_num = gtk_notebook_page_num(notebook, GTK_WIDGET(web_view));
    gboolean removed = FALSE;
    if (page_num >= 0) {
        gtk_notebook_remove_page(notebook, page_num);
        removed = TRUE;
    }

    split_view_after_tab_removed(notebook);
    save_browser_session();

    gint remaining = 0;
    if (browser.notebook)
        remaining += gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook));
    if (browser.split_notebook)
        remaining += gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.split_notebook));
    if (removed && remaining == 0 && browser.main_window)
        gtk_widget_destroy(browser.main_window);
}

int tab_id_for_webview(WebKitWebView *web_view) {
    if (!web_view)
        return 0;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "kite_tab_id"));
}

WebKitWebView *tab_webview_for_id(int id) {
    if (!browser.notebook || id <= 0)
        return nullptr;

    auto find_in = [&](GtkWidget *notebook_widget) -> WebKitWebView * {
        if (!notebook_widget)
            return nullptr;
        gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_widget));
        for (gint i = 0; i < pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_widget), i);
            if (child && WEBKIT_IS_WEB_VIEW(child) && tab_id_for_webview(WEBKIT_WEB_VIEW(child)) == id)
                return WEBKIT_WEB_VIEW(child);
        }
        return nullptr;
    };

    if (WebKitWebView *view = find_in(browser.notebook))
        return view;
    return find_in(browser.split_notebook);
}

std::vector<KiteTabInfo> tabs_list() {
    std::vector<KiteTabInfo> tabs;
    if (!browser.notebook)
        return tabs;

    WebKitWebView *active = get_active_webview();
    int index = 0;

    auto append_notebook = [&](GtkWidget *notebook_widget) {
        if (!notebook_widget)
            return;
        gint pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook_widget));
        for (gint i = 0; i < pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook_widget), i);
            if (!child || !WEBKIT_IS_WEB_VIEW(child))
                continue;
            WebKitWebView *web_view = WEBKIT_WEB_VIEW(child);
            KiteTabInfo info;
            info.id = tab_id_for_webview(web_view);
            info.index = index++;
            info.active = web_view == active;
            info.incognito = webview_is_incognito(web_view) != FALSE;
            const gchar *uri = webkit_web_view_get_uri(web_view);
            const gchar *title = webkit_web_view_get_title(web_view);
            info.url = uri ? uri : "";
            info.title = title ? title : "";
            tabs.push_back(info);
        }
    };

    append_notebook(browser.notebook);
    append_notebook(browser.split_notebook);
    return tabs;
}

bool tab_activate_id(int id) {
    WebKitWebView *web_view = tab_webview_for_id(id);
    if (!web_view)
        return false;
    return activate_webview(web_view);
}

bool tab_close_id(int id) {
    WebKitWebView *web_view = tab_webview_for_id(id);
    if (!web_view)
        return false;
    close_tab(web_view);
    return true;
}
