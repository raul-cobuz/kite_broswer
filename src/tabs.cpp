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
static void update_tab_visuals(WebKitWebView *web_view);
static void show_tab_context_menu(WebKitWebView *web_view, GdkEventButton *event);

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

static void update_tab_visuals(WebKitWebView *web_view) {
    if (!web_view)
        return;

    GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_label"));
    GtkWidget *close_btn = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_close"));
    GtkWidget *favicon = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_favicon"));
    GtkWidget *mute = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_mute"));
    bool pinned = tab_is_pinned(web_view);
    bool muted = webkit_web_view_get_is_muted(web_view) != FALSE;

    if (pinned) {
        if (label)
            gtk_widget_hide(label);
        if (close_btn)
            gtk_widget_hide(close_btn);
        if (favicon) {
            cairo_surface_t *surface = webkit_web_view_get_favicon(web_view);
            bool set = false;
            if (surface) {
                int width = cairo_image_surface_get_width(surface);
                int height = cairo_image_surface_get_height(surface);
                if (width > 0 && height > 0) {
                    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
                    if (pixbuf) {
                        GdkPixbuf *scaled =
                            gdk_pixbuf_scale_simple(pixbuf, 16, 16, GDK_INTERP_BILINEAR);
                        if (scaled) {
                            gtk_image_set_from_pixbuf(GTK_IMAGE(favicon), scaled);
                            g_object_unref(scaled);
                            set = true;
                        }
                        g_object_unref(pixbuf);
                    }
                }
            }
            if (!set)
                gtk_image_set_from_icon_name(GTK_IMAGE(favicon), "text-html-symbolic", GTK_ICON_SIZE_MENU);
            gtk_widget_show(favicon);
        }
    } else {
        if (favicon)
            gtk_widget_hide(favicon);
        if (label)
            gtk_widget_show(label);
        if (close_btn)
            gtk_widget_show(close_btn);
    }

    if (mute) {
        if (muted)
            gtk_widget_show(mute);
        else
            gtk_widget_hide(mute);
    }

    const gchar *title = webkit_web_view_get_title(web_view);
    const gchar *uri = webkit_web_view_get_uri(web_view);
    const gchar *label_text = title && *title ? title : (uri && *uri ? uri : "New Tab");
    gchar *tooltip = pinned ? g_strdup_printf("%s — pinned", label_text) : g_strdup(label_text);
    if (label)
        gtk_widget_set_tooltip_text(label, tooltip);
    GtkWidget *tab_box = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_box"));
    if (tab_box)
        gtk_widget_set_tooltip_text(tab_box, tooltip);
    g_free(tooltip);
}

static void on_favicon_changed(WebKitWebView *web_view, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    update_tab_visuals(web_view);
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
        update_tab_visuals(web_view);
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
        update_tab_visuals(web_view);
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

    if (!tab_is_pinned(web_view)) {
        gtk_label_set_text(GTK_LABEL(label), title);
        gtk_widget_set_tooltip_text(label, title);
    }

    if (!webview_is_incognito(web_view)) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        if (is_trackable_url(uri) && *title)
            history_update_title(uri, title);
    }

    update_tab_visuals(web_view);

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

static void menu_unref_view(gpointer data, GClosure *closure) {
    (void)closure;
    if (data)
        g_object_unref(data);
}

static void on_menu_duplicate(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_duplicate(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_copy_url(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_copy_url(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_mute(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_toggle_muted(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_pin(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_toggle_pinned(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_close_others(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_close_others(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_close_right(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    tab_close_to_right(WEBKIT_WEB_VIEW(user_data));
}

static void on_menu_move_split(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    move_tab_to_split(WEBKIT_WEB_VIEW(user_data));
}

static GtkWidget *add_menu_item(GtkWidget *menu, const char *label, GCallback callback,
                                WebKitWebView *web_view) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect_data(item, "activate", callback, g_object_ref(web_view), menu_unref_view,
                          (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static void show_tab_context_menu(WebKitWebView *web_view, GdkEventButton *event) {
    if (!web_view)
        return;

    GtkWidget *menu = gtk_menu_new();
    bool pinned = tab_is_pinned(web_view);
    bool muted = webkit_web_view_get_is_muted(web_view) != FALSE;
    bool in_split = browser.split_notebook &&
                    gtk_notebook_page_num(GTK_NOTEBOOK(browser.split_notebook), GTK_WIDGET(web_view)) >= 0;

    add_menu_item(menu, "Duplicate", G_CALLBACK(on_menu_duplicate), web_view);
    add_menu_item(menu, "Copy URL", G_CALLBACK(on_menu_copy_url), web_view);
    add_menu_item(menu, muted ? "Unmute tab" : "Mute tab", G_CALLBACK(on_menu_mute), web_view);
    add_menu_item(menu, pinned ? "Unpin tab" : "Pin tab", G_CALLBACK(on_menu_pin), web_view);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_menu_item(menu, "Close other tabs", G_CALLBACK(on_menu_close_others), web_view);
    GtkWidget *close_right = add_menu_item(menu, "Close tabs to the right", G_CALLBACK(on_menu_close_right),
                                           web_view);

    gint page = -1;
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (notebook)
        page = gtk_notebook_page_num(notebook, GTK_WIDGET(web_view));
    bool has_right = false;
    if (notebook) {
        gint pages = gtk_notebook_get_n_pages(notebook);
        for (gint i = page + 1; i < pages; i++) {
            GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
            if (child && WEBKIT_IS_WEB_VIEW(child) && !tab_is_pinned(WEBKIT_WEB_VIEW(child)))
                has_right = true;
        }
    }
    gtk_widget_set_sensitive(close_right, has_right);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_menu_item(menu, in_split ? "Move out of split pane" : "Move to split pane",
                  G_CALLBACK(on_menu_move_split), web_view);

    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(web_view), nullptr);
    g_signal_connect_swapped(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static gboolean on_tab_box_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget;
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    if (event->button == 1)
        split_view_note_focus(web_view);
    else if (event->button == 2)
        close_tab(web_view);
    else if (event->button == 3) {
        show_tab_context_menu(web_view, event);
        return TRUE;
    }
    return FALSE;
}

// GtkNotebook owns its tab strip and can consume right-clicks before they
// reach the tab label. Hit-test the visible labels here as well so the
// context menu works anywhere on a tab.
static WebKitWebView *tab_at_position(GtkNotebook *notebook, gdouble x, gdouble y) {
    gint pages = gtk_notebook_get_n_pages(notebook);
    for (gint i = 0; i < pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
        GtkWidget *label = child ? gtk_notebook_get_tab_label(notebook, child) : nullptr;
        if (!label || !gtk_widget_get_visible(label))
            continue;

        GtkAllocation allocation;
        gtk_widget_get_allocation(label, &allocation);
        gint label_x = 0;
        gint label_y = 0;
        gtk_widget_translate_coordinates(label, GTK_WIDGET(notebook), 0, 0, &label_x, &label_y);
        if (x >= label_x && x < label_x + allocation.width && y >= label_y && y < label_y + allocation.height)
            return WEBKIT_IS_WEB_VIEW(child) ? WEBKIT_WEB_VIEW(child) : nullptr;
    }
    return nullptr;
}

static gboolean on_notebook_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)user_data;
    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return FALSE;

    gdouble x = event->x;
    gdouble y = event->y;
    GdkWindow *event_window = event->window;
    GdkWindow *notebook_window = gtk_widget_get_window(widget);
    if (event_window && notebook_window && event_window != notebook_window) {
        gint event_x = 0, event_y = 0, notebook_x = 0, notebook_y = 0;
        gdk_window_get_origin(event_window, &event_x, &event_y);
        gdk_window_get_origin(notebook_window, &notebook_x, &notebook_y);
        x += event_x - notebook_x;
        y += event_y - notebook_y;
    }

    WebKitWebView *web_view = tab_at_position(GTK_NOTEBOOK(widget), x, y);
    if (!web_view)
        return FALSE;
    show_tab_context_menu(web_view, event);
    return TRUE;
}

void tabs_install_notebook_context_menu(GtkNotebook *notebook) {
    if (notebook)
        g_signal_connect(notebook, "button-press-event", G_CALLBACK(on_notebook_button_press), nullptr);
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
    GtkWidget *tab_favicon = gtk_image_new_from_icon_name("web-browser-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_no_show_all(tab_favicon, TRUE);
    gtk_widget_set_valign(tab_favicon, GTK_ALIGN_CENTER);
    GtkWidget *tab_mute = gtk_image_new_from_icon_name("audio-volume-muted-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_no_show_all(tab_mute, TRUE);
    gtk_widget_set_valign(tab_mute, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(tab_mute, "Tab is muted");

    GtkWidget *tab_label = gtk_label_new(incognito ? "Incognito" : "Loading...");
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 18);
    gtk_label_set_width_chars(GTK_LABEL(tab_label), 10);
    g_object_set_data(G_OBJECT(web_view), "tab_label", tab_label);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);

    g_object_set_data(G_OBJECT(web_view), "tab_box", tab_box);
    g_object_set_data(G_OBJECT(web_view), "tab_favicon", tab_favicon);
    g_object_set_data(G_OBJECT(web_view), "tab_mute", tab_mute);
    g_object_set_data(G_OBJECT(web_view), "tab_close", close_btn);

    gtk_box_pack_start(GTK_BOX(tab_box), tab_favicon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), tab_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), tab_mute, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_box), close_btn, FALSE, FALSE, 0);

    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(web_view, "load-failed-with-tls-errors", G_CALLBACK(on_load_failed_with_tls_errors), NULL);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(web_view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(on_progress_changed), NULL);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title_changed), tab_label);
    g_signal_connect(web_view, "notify::favicon", G_CALLBACK(on_favicon_changed), NULL);
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

bool tab_is_pinned(WebKitWebView *web_view) {
    if (!web_view)
        return false;
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "kite_pinned")) != 0;
}

void tabs_enforce_pinned_order(GtkNotebook *notebook) {
    if (!notebook)
        return;
    std::vector<WebKitWebView *> pinned;
    gint pages = gtk_notebook_get_n_pages(notebook);
    for (gint i = 0; i < pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
        if (child && WEBKIT_IS_WEB_VIEW(child) && tab_is_pinned(WEBKIT_WEB_VIEW(child)))
            pinned.push_back(WEBKIT_WEB_VIEW(child));
    }
    for (size_t i = 0; i < pinned.size(); i++) {
        if (gtk_notebook_page_num(notebook, GTK_WIDGET(pinned[i])) != (gint)i)
            gtk_notebook_reorder_child(notebook, GTK_WIDGET(pinned[i]), (gint)i);
    }
}

void tab_set_pinned(WebKitWebView *web_view, bool pinned) {
    if (!web_view)
        return;
    bool was_pinned = tab_is_pinned(web_view);
    g_object_set_data(G_OBJECT(web_view), "kite_pinned", GINT_TO_POINTER(pinned ? 1 : 0));

    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (notebook) {
        gtk_notebook_set_tab_reorderable(notebook, GTK_WIDGET(web_view),
                                         !pinned && notebook == GTK_NOTEBOOK(browser.notebook));
        if (pinned) {
            gint target = 0;
            gint pages = gtk_notebook_get_n_pages(notebook);
            for (gint i = 0; i < pages; i++) {
                GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
                if (child && child != GTK_WIDGET(web_view) && WEBKIT_IS_WEB_VIEW(child) &&
                    tab_is_pinned(WEBKIT_WEB_VIEW(child)))
                    target++;
            }
            gtk_notebook_reorder_child(notebook, GTK_WIDGET(web_view), target);
        } else {
            gint target = 0;
            gint pages = gtk_notebook_get_n_pages(notebook);
            for (gint i = 0; i < pages; i++) {
                GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
                if (child && child != GTK_WIDGET(web_view) && WEBKIT_IS_WEB_VIEW(child) &&
                    tab_is_pinned(WEBKIT_WEB_VIEW(child)))
                    target++;
            }
            gtk_notebook_reorder_child(notebook, GTK_WIDGET(web_view), target);
        }
    }
    update_tab_visuals(web_view);
    if (was_pinned != pinned)
        save_browser_session();
}

void tab_toggle_pinned(WebKitWebView *web_view) {
    if (web_view)
        tab_set_pinned(web_view, !tab_is_pinned(web_view));
}

void tab_duplicate(WebKitWebView *web_view) {
    if (!web_view)
        return;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    create_tab_view(uri && *uri ? uri : KITE_NEWTAB_URL, webview_is_incognito(web_view), nullptr, TRUE);
}

void tab_copy_url(WebKitWebView *web_view) {
    if (!web_view)
        return;
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri || !*uri)
        return;
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    if (clipboard)
        gtk_clipboard_set_text(clipboard, uri, -1);
}

void tab_toggle_muted(WebKitWebView *web_view) {
    if (!web_view)
        return;
    webkit_web_view_set_is_muted(web_view, !webkit_web_view_get_is_muted(web_view));
    update_tab_visuals(web_view);
}

static std::vector<WebKitWebView *> notebook_views(GtkNotebook *notebook) {
    std::vector<WebKitWebView *> views;
    if (!notebook)
        return views;
    gint pages = gtk_notebook_get_n_pages(notebook);
    for (gint i = 0; i < pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
        if (child && WEBKIT_IS_WEB_VIEW(child))
            views.push_back(WEBKIT_WEB_VIEW(child));
    }
    return views;
}

void tab_close_others(WebKitWebView *web_view) {
    if (!web_view)
        return;
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return;

    std::vector<WebKitWebView *> targets;
    for (WebKitWebView *view : notebook_views(notebook)) {
        if (view != web_view && !tab_is_pinned(view))
            targets.push_back(view);
    }
    for (WebKitWebView *view : targets)
        close_tab(view);
    activate_webview(web_view);
}

void tab_close_to_right(WebKitWebView *web_view) {
    if (!web_view)
        return;
    GtkNotebook *notebook = notebook_for_webview(web_view);
    if (!notebook)
        return;

    gint page = gtk_notebook_page_num(notebook, GTK_WIDGET(web_view));
    if (page < 0)
        return;

    std::vector<WebKitWebView *> targets;
    gint pages = gtk_notebook_get_n_pages(notebook);
    for (gint i = page + 1; i < pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(notebook, i);
        if (child && WEBKIT_IS_WEB_VIEW(child) && !tab_is_pinned(WEBKIT_WEB_VIEW(child)))
            targets.push_back(WEBKIT_WEB_VIEW(child));
    }
    for (auto it = targets.rbegin(); it != targets.rend(); ++it)
        close_tab(*it);
    activate_webview(web_view);
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
            info.pinned = tab_is_pinned(web_view);
            info.muted = webkit_web_view_get_is_muted(web_view) != FALSE;
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
