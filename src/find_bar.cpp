#include "find_bar.h"

#include "browser_window.h"

static bool syncing_find_entry = false;

void run_find(WebKitWebView *web_view, const gchar *text) {
    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    webkit_find_controller_search(
        fc, text, (WebKitFindOptions)(WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND),
        G_MAXUINT);
}

void sync_find_bar(WebKitWebView *web_view) {
    if (!web_view || !browser.find_entry)
        return;

    const gchar *query = (const gchar *)g_object_get_data(G_OBJECT(web_view), "find_query");
    const gchar *current = gtk_entry_get_text(GTK_ENTRY(browser.find_entry));
    if (g_strcmp0(query ? query : "", current) != 0) {
        syncing_find_entry = true;
        gtk_entry_set_text(GTK_ENTRY(browser.find_entry), query ? query : "");
        syncing_find_entry = false;
    }

    if (!browser.find_box || !gtk_widget_get_visible(browser.find_box))
        return;

    if (query && *query) {
        run_find(web_view, query);
    } else {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(web_view));
        if (browser.find_status)
            gtk_label_set_text(GTK_LABEL(browser.find_status), "");
    }
}

static void on_find_changed(GtkSearchEntry *entry, gpointer data) {
    if (syncing_find_entry)
        return;

    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;

    const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
    g_object_set_data_full(G_OBJECT(web_view), "find_query", g_strdup(text), g_free);

    if (!gtk_widget_get_visible(browser.find_box))
        return;

    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    if (*text) {
        run_find(web_view, text);
    } else {
        webkit_find_controller_search_finish(fc);
        if (browser.find_status)
            gtk_label_set_text(GTK_LABEL(browser.find_status), "");
    }
}

static void on_find_found(WebKitFindController *fc, guint match_count, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    if (web_view != get_active_webview() || !browser.find_box || !gtk_widget_get_visible(browser.find_box) ||
        !browser.find_status)
        return;

    gchar *text = g_strdup_printf("%u match%s", match_count, match_count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(browser.find_status), text);
    g_free(text);
}

static void on_find_failed(WebKitFindController *fc, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    if (web_view != get_active_webview() || !browser.find_box || !gtk_widget_get_visible(browser.find_box) ||
        !browser.find_status)
        return;

    gtk_label_set_text(GTK_LABEL(browser.find_status), "No matches");
}

static void on_find_activate(GtkEntry *entry, gpointer data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;

    GdkModifierType state = (GdkModifierType)0;
    gtk_get_current_event_state(&state);
    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    if (state & GDK_SHIFT_MASK)
        webkit_find_controller_search_previous(fc);
    else
        webkit_find_controller_search_next(fc);
}

void close_find_bar() {
    if (browser.find_box)
        gtk_widget_hide(browser.find_box);
    if (browser.find_status)
        gtk_label_set_text(GTK_LABEL(browser.find_status), "");
    if (get_active_webview())
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_next(GtkButton *btn, gpointer data) {
    if (get_active_webview())
        webkit_find_controller_search_next(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_prev(GtkButton *btn, gpointer data) {
    if (get_active_webview())
        webkit_find_controller_search_previous(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_close(GtkButton *btn, gpointer data) {
    close_find_bar();
}

static void on_find_stop_search(GtkSearchEntry *entry, gpointer data) {
    close_find_bar();
}

void connect_find_handlers(WebKitWebView *web_view) {
    WebKitFindController *find_controller = webkit_web_view_get_find_controller(web_view);
    g_signal_connect(find_controller, "found-text", G_CALLBACK(on_find_found), web_view);
    g_signal_connect(find_controller, "failed-to-find-text", G_CALLBACK(on_find_failed), web_view);
}

GtkWidget *create_find_bar() {
    browser.find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    browser.find_entry = gtk_search_entry_new();
    browser.find_status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(browser.find_status), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(browser.find_status), 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(browser.find_status), "dim-label");

    GtkWidget *find_prev = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);

    gtk_box_pack_start(GTK_BOX(browser.find_box), browser.find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(browser.find_box), browser.find_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(browser.find_box), find_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(browser.find_box), find_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(browser.find_box), find_close, FALSE, FALSE, 0);

    g_signal_connect(browser.find_entry, "search-changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(browser.find_entry, "activate", G_CALLBACK(on_find_activate), NULL);
    g_signal_connect(browser.find_entry, "stop-search", G_CALLBACK(on_find_stop_search), NULL);
    g_signal_connect(find_prev, "clicked", G_CALLBACK(on_find_prev), NULL);
    g_signal_connect(find_next, "clicked", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(find_close, "clicked", G_CALLBACK(on_find_close), NULL);

    return browser.find_box;
}
