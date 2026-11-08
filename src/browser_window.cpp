#include "browser_window.h"

#include <gdk/gdkkeysyms.h>

#include <string>

#include "dark_mode.h"
#include "find_bar.h"
#include "settings.h"
#include "tabs.h"
#include "url_utils.h"

BrowserState browser;

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

void set_url_entry_text(const gchar *text) {
    if (!browser.url_entry)
        return;
    browser.updating_url_entry = true;
    gtk_entry_set_text(GTK_ENTRY(browser.url_entry), text ? text : "");
    browser.updating_url_entry = false;
    browser.url_entry_user_edited = false;
}

static void on_url_entry_changed(GtkEditable *editable, gpointer user_data) {
    if (!browser.updating_url_entry)
        browser.url_entry_user_edited = true;
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
    if (!browser.notebook)
        return;
    gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(browser.notebook));
    if (page_num >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(browser.notebook), page_num);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(browser.notebook)) == 0 && browser.main_window)
        gtk_widget_destroy(browser.main_window);
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

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    create_new_tab("https://duckduckgo.com");
    focus_url_bar();
}

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view)
        return;

    std::string text = gtk_entry_get_text(entry);
    if (text.empty())
        return;

    std::string target_url = resolve_url_input(text);
    webkit_web_view_load_uri(web_view, target_url.c_str());
    gtk_widget_grab_focus(GTK_WIDGET(web_view));
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer user_data) {
    if (!page || !WEBKIT_IS_WEB_VIEW(page))
        return;
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(page);

    const gchar *uri = webkit_web_view_get_uri(web_view);
    set_url_entry_text(uri);

    GtkStyleContext *context = gtk_widget_get_style_context(browser.url_entry);
    if (webview_is_incognito(web_view)) {
        gtk_style_context_add_class(context, "incognito-mode");
    } else {
        gtk_style_context_remove_class(context, "incognito-mode");
    }

    update_reload_button(web_view);
    update_progress(web_view);
    update_window_title(web_view);
    sync_find_bar(web_view);
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
            create_incognito_tab("https://duckduckgo.com");
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
                create_new_tab("https://duckduckgo.com");
                focus_url_bar();
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
                    webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) + 0.1);
                return TRUE;
            }
            case GDK_KEY_minus: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) - 0.1);
                return TRUE;
            }
            case GDK_KEY_0: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view)
                    webkit_web_view_set_zoom_level(web_view, 1.0);
                return TRUE;
            }
        }
    }
    return FALSE;
}

void create_browser_window() {
    browser.main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(browser.main_window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    gtk_window_set_default_size(GTK_WINDOW(browser.main_window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(browser.main_window), "Kite Browser");
    g_signal_connect(browser.main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(browser.main_window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 4);

    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_reload = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_always_show_image(GTK_BUTTON(browser.btn_reload), TRUE);
    GtkWidget *btn_new_tab = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    browser.btn_dark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(browser.btn_dark),
                         gtk_image_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(browser.btn_dark), settings_dark_mode());

    gtk_widget_set_tooltip_text(browser.btn_reload, "Reload");
    gtk_widget_set_tooltip_text(browser.btn_dark, "Toggle Dark Mode (F9 / Ctrl+Shift+D)");

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(browser.btn_reload, "clicked", G_CALLBACK(on_reload_clicked), NULL);
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(browser.btn_dark, "toggled", G_CALLBACK(on_dark_mode_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), browser.btn_dark, FALSE, FALSE, 0);

    browser.url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(toolbar), browser.url_entry, TRUE, TRUE, 2);
    g_signal_connect(browser.url_entry, "activate", G_CALLBACK(on_url_activate), NULL);
    g_signal_connect(browser.url_entry, "changed", G_CALLBACK(on_url_entry_changed), NULL);

    browser.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(browser.progress_bar), FALSE);
    gtk_widget_set_size_request(browser.progress_bar, -1, 4);
    gtk_box_pack_start(GTK_BOX(vbox), browser.progress_bar, FALSE, FALSE, 0);

    browser.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(browser.notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), browser.notebook, TRUE, TRUE, 0);
    g_signal_connect(browser.notebook, "switch-page", G_CALLBACK(on_tab_switched), NULL);

    GtkWidget *find_box = create_find_bar();
    gtk_box_pack_start(GTK_BOX(vbox), find_box, FALSE, FALSE, 2);

    create_new_tab("https://duckduckgo.com");

    gtk_widget_show_all(browser.main_window);
    gtk_widget_hide(browser.find_box);
    gtk_widget_hide(browser.progress_bar);
    focus_url_bar();
}
