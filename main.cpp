#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string>
#include <gdk/gdkkeysyms.h>

static GtkWidget *notebook;
static GtkWidget *url_entry;
static GtkWidget *btn_reload;
static WebKitUserContentManager *global_content_manager;
static bool global_dark_mode = false;
static GtkWidget *find_box;
static GtkWidget *find_entry;

static void create_tab(const gchar *url, gboolean incognito);
static void create_new_tab(const gchar *url);
static void create_incognito_tab(const gchar *url);
static void toggle_dark_mode();
static void set_webview_dark_mode(WebKitWebView *web_view, bool enable);
static void update_reload_button(WebKitWebView *web_view);
static void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data);

static gboolean webview_is_incognito(WebKitWebView *web_view) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "is_incognito"));
}

// Look for filters.json next to the binary, then CWD, then ~/.config/kite/
static gchar* find_filters_json_path() {
    gchar *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        gchar *exe_dir = g_path_get_dirname(exe_path);
        gchar *candidate = g_build_filename(exe_dir, "filters.json", NULL);
        g_free(exe_dir);
        g_free(exe_path);
        if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
            return candidate;
        g_free(candidate);
    }

    if (g_file_test("filters.json", G_FILE_TEST_IS_REGULAR))
        return g_strdup("filters.json");

    gchar *config_path = g_build_filename(g_get_user_config_dir(), "kite", "filters.json", NULL);
    if (g_file_test(config_path, G_FILE_TEST_IS_REGULAR))
        return config_path;
    g_free(config_path);

    return nullptr;
}

WebKitWebView* get_active_webview() {
    gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    if (page_num < 0) return nullptr;
    GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), page_num);
    return WEBKIT_WEB_VIEW(child);
}

static void on_find_changed(GtkSearchEntry* entry, gpointer data){
    WebKitWebView* web_view=get_active_webview();
    if(!web_view)
    {
        return;
    }
    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    const gchar *text=gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_utf8_strlen(text, -1) > 0) {
        webkit_find_controller_search(fc, text, (WebKitFindOptions)(WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND), G_MAXUINT);
    } else {
        webkit_find_controller_search_finish(fc);
    }

}

static void on_find_next(GtkButton *btn, gpointer data) {
    if (get_active_webview()) webkit_find_controller_search_next(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_prev(GtkButton *btn, gpointer data) {
    if (get_active_webview()) webkit_find_controller_search_previous(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_close(GtkButton *btn, gpointer data) {
    gtk_widget_hide(find_box);
    if (get_active_webview()) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(get_active_webview()));
}


static void on_filter_saved(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = nullptr;
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(store, res, &error);

    if (error) {
        g_printerr("[Kite] EROARE la compilarea filtrelor: %s\n", error->message);
        g_error_free(error);
    } else {
        webkit_user_content_manager_add_filter(global_content_manager, filter);
        webkit_user_content_filter_unref(filter);
        g_print("\n>>> [Kite] AD-BLOCKER ACTIVAT CU SUCCES! <<<\n\n");
    }
    g_object_unref(store);
}

void init_adblocker() {
    global_content_manager = webkit_user_content_manager_new();
    gchar *filter_cache_dir = g_build_filename(g_get_user_cache_dir(), "kite", "adblock", NULL);
    g_mkdir_with_parents(filter_cache_dir, 0700);

    WebKitUserContentFilterStore *store = webkit_user_content_filter_store_new(filter_cache_dir);
    g_free(filter_cache_dir);

    gchar *rules_content = nullptr;
    gsize length = 0;
    GError *file_error = nullptr;
    gchar *filters_path = find_filters_json_path();

    if (filters_path && g_file_get_contents(filters_path, &rules_content, &length, &file_error)) {
        g_print("[Kite] Se incarca %s (%lu bytes)...\n", filters_path, length);
        GBytes *bytes = g_bytes_new_take(rules_content, length);
        webkit_user_content_filter_store_save(store, "kite_rules", bytes, NULL, on_filter_saved, NULL);
        g_bytes_unref(bytes);
    } else {
        if (file_error) g_error_free(file_error);
        g_printerr("[Kite] EROARE: filters.json lipseste (cautat langa binar, CWD, ~/.config/kite/). Ruleaza: python3 update_filters.py\n");
        g_object_unref(store);
    }
    g_free(filters_path);
}

static void on_url_activate(GtkEntry *entry, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view) return;

    std::string text = gtk_entry_get_text(entry);
    if (text.empty()) return;

    std::string target_url;

    if (text.rfind("!yt ", 0) == 0) {
        gchar *escaped = g_uri_escape_string(text.substr(4).c_str(), NULL, TRUE);
        target_url = "https://www.youtube.com/results?search_query=" + std::string(escaped);
        g_free(escaped);
    } else if (text.rfind("!gh ", 0) == 0) {
        gchar *escaped = g_uri_escape_string(text.substr(4).c_str(), NULL, TRUE);
        target_url = "https://github.com/search?q=" + std::string(escaped);
        g_free(escaped);
    } else if (text.rfind("!w ", 0) == 0) {
        gchar *escaped = g_uri_escape_string(text.substr(3).c_str(), NULL, TRUE);
        target_url = "https://en.wikipedia.org/wiki/Special:Search?search=" + std::string(escaped);
        g_free(escaped);
    } else if (text.rfind("!r ", 0) == 0) {
        gchar *escaped = g_uri_escape_string(text.substr(3).c_str(), NULL, TRUE);
        target_url = "https://www.reddit.com/search/?q=" + std::string(escaped);
        g_free(escaped);
    } else if (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0) {
        target_url = text;
    } else if (text.find(' ') != std::string::npos || text.find('.') == std::string::npos) {
        gchar *escaped = g_uri_escape_string(text.c_str(), NULL, TRUE);
        target_url = std::string("https://duckduckgo.com/?q=") + escaped;
        g_free(escaped);
    } else {
        target_url = "https://" + text;
    }

    webkit_web_view_load_uri(web_view, target_url.c_str());
}

static void update_reload_button(WebKitWebView *web_view) {
    if (!btn_reload || !web_view) return;
    gboolean loading = webkit_web_view_is_loading(web_view);
    GtkWidget *image = gtk_image_new_from_icon_name(
        loading ? "process-stop-symbolic" : "view-refresh-symbolic",
        GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(btn_reload), image);
    gtk_widget_set_tooltip_text(btn_reload, loading ? "Stop loading" : "Reload");
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_label"));

    if (load_event == WEBKIT_LOAD_STARTED && label) {
        gtk_label_set_text(GTK_LABEL(label), "Loading...");
        gtk_widget_set_tooltip_text(label, "Loading...");
    }

    if (load_event == WEBKIT_LOAD_FINISHED && label) {
        const gchar *title = webkit_web_view_get_title(web_view);
        if (title && *title) {
            gtk_label_set_text(GTK_LABEL(label), title);
            gtk_widget_set_tooltip_text(label, title);
        }
    }

    if (web_view != get_active_webview()) return;

    if (load_event == WEBKIT_LOAD_COMMITTED) {
        if (global_dark_mode) set_webview_dark_mode(web_view, true);
        const gchar *uri = webkit_web_view_get_uri(web_view);
        gtk_entry_set_text(GTK_ENTRY(url_entry), uri ? uri : "");
    }

    if (load_event == WEBKIT_LOAD_STARTED ||
        load_event == WEBKIT_LOAD_FINISHED ||
        load_event == WEBKIT_LOAD_COMMITTED) {
        update_reload_button(web_view);
    }
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, GtkWidget *label) {
    const gchar *title = webkit_web_view_get_title(web_view);
    if (title) {
        gtk_label_set_text(GTK_LABEL(label), title);
        gtk_widget_set_tooltip_text(label, title);
    }
}

static void on_tab_close_clicked(GtkButton *btn, GtkWidget *child_page) {
    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), child_page);
    if (page_num >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0) create_new_tab("https://duckduckgo.com");
}

static GtkWidget* on_create_tab(WebKitWebView *web_view, WebKitNavigationAction *action, gpointer user_data) {
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = webkit_uri_request_get_uri(req);
    create_tab(uri ? uri : "https://duckduckgo.com", webview_is_incognito(web_view));
    return GTK_WIDGET(get_active_webview());
}

static void apply_webview_settings(WebKitWebView *web_view) {
    WebKitSettings *settings = webkit_web_view_get_settings(web_view);
    webkit_settings_set_enable_developer_extras(settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
    webkit_settings_set_enable_webgl(settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_settings_set_user_agent(settings,
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    webkit_web_view_set_settings(web_view, settings);
}

static void create_tab(const gchar *url, gboolean incognito) {
    WebKitWebContext *ephemeral_context = nullptr;
    WebKitWebView *web_view;

    if (incognito) {
        WebKitWebsiteDataManager *ephemeral_dm = webkit_website_data_manager_new_ephemeral();
        ephemeral_context = webkit_web_context_new_with_website_data_manager(ephemeral_dm);
        g_signal_connect(ephemeral_context, "download-started", G_CALLBACK(on_download_started), NULL);
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "web-context", ephemeral_context,
            "user-content-manager", global_content_manager,
            NULL));
        g_object_set_data(G_OBJECT(web_view), "is_incognito", GINT_TO_POINTER(1));
    } else {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "user-content-manager", global_content_manager, NULL));
    }

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
    g_signal_connect(web_view, "notify::title", G_CALLBACK(on_title_changed), tab_label);
    g_signal_connect(web_view, "create", G_CALLBACK(on_create_tab), NULL);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), GTK_WIDGET(web_view));

    gtk_widget_show_all(tab_box);
    gtk_widget_show(GTK_WIDGET(web_view));

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), GTK_WIDGET(web_view), tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), GTK_WIDGET(web_view), TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), index);
    webkit_web_view_load_uri(web_view, url);

    if (ephemeral_context)
        g_object_unref(ephemeral_context);
}

static void create_new_tab(const gchar *url) {
    create_tab(url, FALSE);
}

static void create_incognito_tab(const gchar *url) {
    create_tab(url, TRUE);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view) webkit_web_view_go_back(web_view);
}
static void on_forward_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (web_view) webkit_web_view_go_forward(web_view);
}
static void on_reload_clicked(GtkButton *btn, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view) return;
    if (webkit_web_view_is_loading(web_view))
        webkit_web_view_stop_loading(web_view);
    else
        webkit_web_view_reload(web_view);
}
static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    create_new_tab("https://duckduckgo.com");
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(page);

    const gchar *uri = webkit_web_view_get_uri(web_view);
    gtk_entry_set_text(GTK_ENTRY(url_entry), uri ? uri : "");

    GtkStyleContext *context = gtk_widget_get_style_context(url_entry);
    if (webview_is_incognito(web_view)) {
        gtk_style_context_add_class(context, "incognito-mode");
    } else {
        gtk_style_context_remove_class(context, "incognito-mode");
    }

    update_reload_button(web_view);
}
static void set_webview_dark_mode(WebKitWebView *web_view, bool enable) {
    gchar *js = g_strdup_printf(
        "var id = 'kite-dark-mode-style'; "
        "var el = document.getElementById(id); "
        "if (%d) { "
        "   if (!el) { "
        "       var style = document.createElement('style'); style.id = id; "
        "       style.textContent = 'html { filter: invert(90%%) hue-rotate(180deg) !important; background-color: #121212 !important; } img, video, canvas, [style*=\"background-image\"] { filter: invert(111%%) hue-rotate(180deg) !important; }'; "
        "       (document.head || document.documentElement).appendChild(style); "
        "   } "
        "} else { "
        "   if (el) el.remove(); "
        "}", enable ? 1 : 0);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
}

static void toggle_dark_mode() {
    global_dark_mode = !global_dark_mode; // Schimbăm starea globală
    
    // Aplicăm instant pe toate tab-urile deschise
    gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        set_webview_dark_mode(WEBKIT_WEB_VIEW(child), global_dark_mode);
    }
}
static void on_dark_mode_clicked(GtkButton *btn, gpointer user_data) {
    toggle_dark_mode();
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    // F12 sau Ctrl+Shift+I pentru Web Inspector
    if (event->keyval == GDK_KEY_F12 || ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_SHIFT_MASK) && (event->keyval == GDK_KEY_I || event->keyval == GDK_KEY_i))) {
        WebKitWebView *web_view = get_active_webview();
        if (web_view) webkit_web_inspector_show(webkit_web_view_get_inspector(web_view));
        return TRUE;
    }

    // Escape pentru ascunderea barei de Ctrl+F
    if (event->keyval == GDK_KEY_Escape && gtk_widget_get_visible(find_box)) {
        gtk_widget_hide(find_box);
        WebKitWebView *web_view = get_active_webview();
        if (web_view) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(web_view));
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
            gint n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
            if (n > 0) {
                gint cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
                gboolean backward = (event->state & GDK_SHIFT_MASK) ||
                                    event->keyval == GDK_KEY_ISO_Left_Tab;
                gint next = backward ? (cur - 1 + n) % n : (cur + 1) % n;
                gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), next);
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
            return TRUE;
        }

        switch (event->keyval) {
            case GDK_KEY_f:
            case GDK_KEY_F:
                gtk_widget_show(find_box);
                gtk_widget_grab_focus(find_entry);
                return TRUE;
            case GDK_KEY_t:
                create_new_tab("https://duckduckgo.com");
                return TRUE;
            case GDK_KEY_w: {
                gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
                if (page_num >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0) create_new_tab("https://duckduckgo.com");
                return TRUE;
            }
            case GDK_KEY_l:
                gtk_widget_grab_focus(url_entry);
                return TRUE;
            case GDK_KEY_r:
            case GDK_KEY_R: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view) webkit_web_view_reload(web_view);
                return TRUE;
            }
            case GDK_KEY_plus:
            case GDK_KEY_equal: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view) webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) + 0.1);
                return TRUE;
            }
            case GDK_KEY_minus: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view) webkit_web_view_set_zoom_level(web_view, webkit_web_view_get_zoom_level(web_view) - 0.1);
                return TRUE;
            }
            case GDK_KEY_0: {
                WebKitWebView *web_view = get_active_webview();
                if (web_view) webkit_web_view_set_zoom_level(web_view, 1.0);
                return TRUE;
            }
        }
    }
    return FALSE;
}
static gboolean on_download_decide_destination(WebKitDownload *download, const gchar *suggested_filename, gpointer user_data) {
   
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Salvează fișierul",
                                                    NULL,
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Anulează", GTK_RESPONSE_CANCEL,
                                                    "_Salvează", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested_filename);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gchar *uri = g_filename_to_uri(filename, NULL, NULL);
        

        webkit_download_set_destination(download, uri);
        
        g_free(filename);
        g_free(uri);
    } else {
        webkit_download_cancel(download);
    }
    
    gtk_widget_destroy(dialog);
    return TRUE;
}

static void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data) {
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), NULL);
}
int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, 
        ".incognito-mode { background: #1a0b2e !important; color: #b28ced !important; border-bottom: 2px solid #8e44ad !important; }", 
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), 
        GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    init_adblocker();

    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    g_signal_connect(context, "download-started", G_CALLBACK(on_download_started), NULL);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(window), "Kite Browser");
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 4);

    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    btn_reload = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_always_show_image(GTK_BUTTON(btn_reload), TRUE);
    GtkWidget *btn_new_tab = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_dark = gtk_button_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON);

    gtk_widget_set_tooltip_text(btn_reload, "Reload");
    gtk_widget_set_tooltip_text(btn_dark, "Toggle Dark Mode (F9 / Ctrl+Shift+D)");

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload_clicked), NULL);
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(btn_dark, "clicked", G_CALLBACK(on_dark_mode_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_dark, FALSE, FALSE, 0);

    url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(toolbar), url_entry, TRUE, TRUE, 2);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_activate), NULL);

    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_tab_switched), NULL);

  
    find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    
    
    find_entry = gtk_search_entry_new();
    GtkWidget *find_prev = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    
    gtk_box_pack_start(GTK_BOX(find_box), find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_close, FALSE, FALSE, 0);
    
    g_signal_connect(find_entry, "search-changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_prev, "clicked", G_CALLBACK(on_find_prev), NULL);
    g_signal_connect(find_next, "clicked", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(find_close, "clicked", G_CALLBACK(on_find_close), NULL);
    
    gtk_box_pack_start(GTK_BOX(vbox), find_box, FALSE, FALSE, 2);
    create_new_tab("https://duckduckgo.com");

    gtk_widget_show_all(window);
    gtk_widget_hide(find_box);
    gtk_main();

    return 0;
}