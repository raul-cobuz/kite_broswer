#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <string>
#include <gdk/gdkkeysyms.h>
#include <arpa/inet.h>

static GtkWidget *main_window;
static GtkWidget *notebook;
static GtkWidget *url_entry;
static GtkWidget *btn_reload;
static GtkWidget *btn_dark;
static GtkWidget *progress_bar;
static WebKitUserContentManager *global_content_manager;
static bool global_dark_mode = false;
static GtkWidget *find_box;
static GtkWidget *find_entry;
static GtkWidget *find_status;
static bool syncing_find_entry = false;
static WebKitUserScript *dark_mode_script = nullptr;
static WebKitWebContext *incognito_context = nullptr;
static GKeyFile *settings = nullptr;
static gchar *settings_file = nullptr;
static GHashTable *permission_decisions = nullptr;

static void create_tab(const gchar *url, gboolean incognito);
static void create_new_tab(const gchar *url);
static void create_incognito_tab(const gchar *url);
static void toggle_dark_mode();
static void apply_dark_mode(bool enable);
static void set_webview_dark_mode(WebKitWebView *web_view, bool enable);
static void update_reload_button(WebKitWebView *web_view);
static void update_progress(WebKitWebView *web_view);
static void update_window_title(WebKitWebView *web_view);
static void sync_find_bar(WebKitWebView *web_view);
static void save_settings();
static void close_find_bar();
static void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data);

static gboolean webview_is_incognito(WebKitWebView *web_view) {
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(web_view), "is_incognito"));
}

// The URL bar is focused when a new tab is opened. Page commits must not
// overwrite what the user is typing, so track programmatic vs user edits.
static bool url_entry_user_edited = false;
static bool updating_url_entry = false;

static void set_url_entry_text(const gchar *text) {
    if (!url_entry)
        return;
    updating_url_entry = true;
    gtk_entry_set_text(GTK_ENTRY(url_entry), text ? text : "");
    updating_url_entry = false;
    url_entry_user_edited = false;
}

static void on_url_entry_changed(GtkEditable *editable, gpointer user_data) {
    if (!updating_url_entry)
        url_entry_user_edited = true;
}

// ---------------------------------------------------------------------------
// Settings (minimal for now: dark mode + remembered permission decisions)
// ---------------------------------------------------------------------------

static void load_settings() {
    permission_decisions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    settings = g_key_file_new();
    settings_file = g_build_filename(g_get_user_config_dir(), "kite", "settings.ini", NULL);

    GError *error = nullptr;
    if (!g_key_file_load_from_file(settings, settings_file, G_KEY_FILE_NONE, &error)) {
        if (error && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("[Kite] Could not load settings: %s\n", error->message);
        g_clear_error(&error);
    }

    gboolean dark = g_key_file_get_boolean(settings, "appearance", "dark_mode", &error);
    if (error) {
        g_clear_error(&error);
    } else {
        global_dark_mode = dark;
    }

    gsize n_keys = 0;
    gchar **keys = g_key_file_get_keys(settings, "permissions", &n_keys, nullptr);
    if (keys) {
        for (gsize i = 0; i < n_keys; i++) {
            gchar *value = g_key_file_get_string(settings, "permissions", keys[i], nullptr);
            if (!value)
                continue;
            g_hash_table_replace(permission_decisions, g_strdup(keys[i]),
                                 GINT_TO_POINTER(g_strcmp0(value, "allow") == 0 ? 1 : 0));
            g_free(value);
        }
        g_strfreev(keys);
    }
}

static void save_settings() {
    if (!settings || !settings_file)
        return;

    g_key_file_set_boolean(settings, "appearance", "dark_mode", global_dark_mode);
    g_key_file_remove_group(settings, "permissions", nullptr);

    GHashTableIter iter;
    gpointer key = nullptr;
    gpointer value = nullptr;
    g_hash_table_iter_init(&iter, permission_decisions);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_key_file_set_string(settings, "permissions", (const gchar *)key,
                              GPOINTER_TO_INT(value) ? "allow" : "deny");

    gchar *dir = g_path_get_dirname(settings_file);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gsize length = 0;
    gchar *data = g_key_file_to_data(settings, &length, nullptr);
    GError *error = nullptr;
    if (!g_file_set_contents(settings_file, data, (gssize)length, &error)) {
        g_printerr("[Kite] Could not save settings: %s\n", error->message);
        g_clear_error(&error);
    }
    g_free(data);
}

// GKeyFile keys only allow alphanumerics, '-', '_', '.', '|' and ':'. Origins can
// contain characters outside that set (IPv6 brackets, spaces, zone ids), so escape
// them. '_' is escaped too, keeping the mapping unambiguous.
static gchar* permission_key(const gchar *type, const gchar *origin) {
    GString *escaped = g_string_new(type);
    g_string_append_c(escaped, '|');
    for (const guchar *p = (const guchar *)origin; *p; p++) {
        guchar c = *p;
        if (g_ascii_isalnum(c) || c == '-' || c == '.' || c == '|' || c == ':')
            g_string_append_c(escaped, (gchar)c);
        else
            g_string_append_printf(escaped, "_%02X", c);
    }
    return g_string_free(escaped, FALSE);
}

static gint get_permission_decision(const gchar *type, const gchar *origin) {
    if (!permission_decisions || !type || !origin)
        return -1;

    gchar *key = permission_key(type, origin);
    gpointer value = nullptr;
    gboolean found = g_hash_table_lookup_extended(permission_decisions, key, nullptr, &value);
    g_free(key);
    return found ? (GPOINTER_TO_INT(value) ? 1 : 0) : -1;
}

static void remember_permission_decision(const gchar *type, const gchar *origin, gboolean allow) {
    if (!permission_decisions || !type || !origin)
        return;

    gchar *key = permission_key(type, origin);
    g_hash_table_replace(permission_decisions, key, GINT_TO_POINTER(allow ? 1 : 0));
    save_settings();
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

static void run_find(WebKitWebView *web_view, const gchar *text) {
    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    webkit_find_controller_search(fc, text,
        (WebKitFindOptions)(WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND),
        G_MAXUINT);
}

static void sync_find_bar(WebKitWebView *web_view) {
    if (!web_view || !find_entry)
        return;

    const gchar *query = (const gchar *)g_object_get_data(G_OBJECT(web_view), "find_query");
    const gchar *current = gtk_entry_get_text(GTK_ENTRY(find_entry));
    if (g_strcmp0(query ? query : "", current) != 0) {
        syncing_find_entry = true;
        gtk_entry_set_text(GTK_ENTRY(find_entry), query ? query : "");
        syncing_find_entry = false;
    }

    if (!find_box || !gtk_widget_get_visible(find_box))
        return;

    if (query && *query) {
        run_find(web_view, query);
    } else {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(web_view));
        if (find_status)
            gtk_label_set_text(GTK_LABEL(find_status), "");
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

    if (!gtk_widget_get_visible(find_box))
        return;

    WebKitFindController *fc = webkit_web_view_get_find_controller(web_view);
    if (*text) {
        run_find(web_view, text);
    } else {
        webkit_find_controller_search_finish(fc);
        if (find_status)
            gtk_label_set_text(GTK_LABEL(find_status), "");
    }
}

static void on_find_found(WebKitFindController *fc, guint match_count, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    if (web_view != get_active_webview() || !find_box || !gtk_widget_get_visible(find_box) || !find_status)
        return;

    gchar *text = g_strdup_printf("%u match%s", match_count, match_count == 1 ? "" : "es");
    gtk_label_set_text(GTK_LABEL(find_status), text);
    g_free(text);
}

static void on_find_failed(WebKitFindController *fc, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(user_data);
    if (web_view != get_active_webview() || !find_box || !gtk_widget_get_visible(find_box) || !find_status)
        return;

    gtk_label_set_text(GTK_LABEL(find_status), "No matches");
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

static void close_find_bar() {
    if (find_box)
        gtk_widget_hide(find_box);
    if (find_status)
        gtk_label_set_text(GTK_LABEL(find_status), "");
    if (get_active_webview())
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_next(GtkButton *btn, gpointer data) {
    if (get_active_webview()) webkit_find_controller_search_next(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_prev(GtkButton *btn, gpointer data) {
    if (get_active_webview()) webkit_find_controller_search_previous(webkit_web_view_get_find_controller(get_active_webview()));
}

static void on_find_close(GtkButton *btn, gpointer data) {
    close_find_bar();
}

static void on_find_stop_search(GtkSearchEntry *entry, gpointer data) {
    close_find_bar();
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

static bool starts_with(const std::string &text, const gchar *prefix) {
    return text.rfind(prefix, 0) == 0;
}

static bool has_known_scheme(const std::string &text) {
    return starts_with(text, "http://") || starts_with(text, "https://") ||
           starts_with(text, "file://") || starts_with(text, "about:") ||
           starts_with(text, "view-source:");
}

static bool is_local_host(const std::string &text) {
    if (starts_with(text, "localhost")) {
        if (text.size() == 9 || text[9] == ':' || text[9] == '/')
            return true;
    }
    if (!text.empty() && text[0] == '[')
        return true; // bracketed IPv6

    std::string host = text.substr(0, text.find_first_of(":/"));
    struct in_addr addr;
    return inet_pton(AF_INET, host.c_str(), &addr) == 1;
}

static std::string search_url(const std::string &text) {
    gchar *escaped = g_uri_escape_string(text.c_str(), NULL, TRUE);
    std::string url = std::string("https://duckduckgo.com/?q=") + escaped;
    g_free(escaped);
    return url;
}

static std::string normalize_url(const std::string &text) {
    if (text.empty())
        return text;
    if (has_known_scheme(text))
        return text;

    if (text[0] == '/' || starts_with(text, "~/") || starts_with(text, "./") || starts_with(text, "../")) {
        std::string candidate = text;
        if (starts_with(text, "~/"))
            candidate = std::string(g_get_home_dir()) + text.substr(1);

        gchar *canonical = g_canonicalize_filename(candidate.c_str(), NULL);
        if (g_file_test(canonical, G_FILE_TEST_EXISTS)) {
            gchar *uri = g_filename_to_uri(canonical, NULL, NULL);
            std::string result = uri ? uri : text;
            g_free(uri);
            g_free(canonical);
            return result;
        }
        g_free(canonical);
    }

    if (is_local_host(text))
        return "http://" + text;

    if (text.find(' ') == std::string::npos && text.find('.') != std::string::npos)
        return "https://" + text;

    return search_url(text);
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
    } else {
        target_url = normalize_url(text);
    }

    webkit_web_view_load_uri(web_view, target_url.c_str());
    gtk_widget_grab_focus(GTK_WIDGET(web_view));
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

static void update_window_title(WebKitWebView *web_view) {
    if (!main_window)
        return;
    const gchar *title = web_view ? webkit_web_view_get_title(web_view) : nullptr;
    if (!title || !*title)
        title = web_view ? webkit_web_view_get_uri(web_view) : nullptr;
    gtk_window_set_title(GTK_WINDOW(main_window), (title && *title) ? title : "Kite Browser");
}

static void update_progress(WebKitWebView *web_view) {
    if (!progress_bar || !web_view)
        return;

    gboolean loading = webkit_web_view_is_loading(web_view);
    gtk_widget_set_visible(progress_bar, loading);
    if (loading)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

static void on_progress_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(object);
    if (web_view != get_active_webview())
        return;
    if (progress_bar && webkit_web_view_is_loading(web_view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar),
                                      webkit_web_view_get_estimated_load_progress(web_view));
}

static void show_error_page(WebKitWebView *web_view, const gchar *uri,
                            const gchar *heading, const gchar *details) {
    gchar *safe_uri = g_markup_escape_text(uri ? uri : "", -1);
    gchar *safe_heading = g_markup_escape_text(heading, -1);
    gchar *safe_details = g_markup_escape_text(details ? details : "", -1);
    gchar *retry = (uri && *uri)
        ? g_strdup_printf("<p><a href=\"%s\">Try again</a></p>", safe_uri)
        : g_strdup("");

    gchar *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title>"
        "<style>"
        "body{font-family:sans-serif;background:#fafafa;color:#202124;margin:0;"
        "display:flex;min-height:100vh;align-items:center;justify-content:center}"
        ".box{max-width:640px;padding:32px}h1{font-size:24px;margin:0 0 12px}"
        "p{line-height:1.5;color:#5f6368}.url{font-family:monospace;word-break:break-all;"
        "background:#eee;padding:10px;border-radius:4px;color:#333}a{color:#1a73e8}"
        "</style></head><body><div class=\"box\"><h1>%s</h1><p>%s</p>"
        "<p class=\"url\">%s</p>%s</div></body></html>",
        safe_heading, safe_heading, safe_details, safe_uri, retry);

    webkit_web_view_load_alternate_html(web_view, html,
        (uri && *uri) ? uri : "about:blank", (uri && *uri) ? uri : "about:blank");

    g_free(html);
    g_free(retry);
    g_free(safe_uri);
    g_free(safe_heading);
    g_free(safe_details);
}

static void set_tab_error_label(WebKitWebView *web_view, const gchar *text, const gchar *tooltip) {
    GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_label"));
    if (!label)
        return;
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_set_tooltip_text(label, tooltip ? tooltip : text);
}

static gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event,
                               const gchar *failing_uri, GError *error, gpointer user_data) {
    if (error->domain == WEBKIT_NETWORK_ERROR && error->code == WEBKIT_NETWORK_ERROR_CANCELLED)
        return FALSE;
    if (error->domain == WEBKIT_POLICY_ERROR &&
        error->code == WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)
        return FALSE;

    gchar *details = g_strdup_printf("Error: %s", error->message);
    show_error_page(web_view, failing_uri, "This page could not be loaded", details);
    g_free(details);

    set_tab_error_label(web_view, "Failed to load", failing_uri);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(main_window), "Failed to load");
        update_reload_button(web_view);
        update_progress(web_view);
    }
    return TRUE;
}

static gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view, const gchar *failing_uri,
                                               GTlsCertificate *certificate, GTlsCertificateFlags errors,
                                               gpointer user_data) {
    show_error_page(web_view, failing_uri, "Insecure connection",
                    "The certificate presented by this site could not be verified, "
                    "so the connection was not established.");

    set_tab_error_label(web_view, "Insecure connection", failing_uri);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(main_window), "Insecure connection");
        update_reload_button(web_view);
        update_progress(web_view);
    }
    return TRUE;
}

static void on_web_process_terminated(WebKitWebView *web_view,
                                      WebKitWebProcessTerminationReason reason, gpointer user_data) {
    const gchar *message = "The web process crashed while loading this page.";
    if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
        message = "The web process ran out of memory while loading this page.";
    else if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
        message = "The web process was terminated.";

    show_error_page(web_view, webkit_web_view_get_uri(web_view), "Page process stopped", message);

    set_tab_error_label(web_view, "Crashed", message);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(main_window), "Page crashed");
        update_reload_button(web_view);
        update_progress(web_view);
    }
}

static void on_load_changed(WebKitWebView *web_view, WebKitLoadEvent load_event, gpointer user_data) {
    GtkWidget *label = GTK_WIDGET(g_object_get_data(G_OBJECT(web_view), "tab_label"));

    if (load_event == WEBKIT_LOAD_STARTED && label) {
        gtk_label_set_text(GTK_LABEL(label), "Loading...");
        gtk_widget_set_tooltip_text(label, "Loading...");
    }

    if (load_event == WEBKIT_LOAD_FINISHED && label) {
        const gchar *title = webkit_web_view_get_title(web_view);
        if (!title || !*title)
            title = webkit_web_view_get_uri(web_view);
        if (!title || !*title)
            title = "New Tab";
        gtk_label_set_text(GTK_LABEL(label), title);
        gtk_widget_set_tooltip_text(label, title);
    }

    if (web_view != get_active_webview()) return;

    if (load_event == WEBKIT_LOAD_COMMITTED) {
        const gchar *uri = webkit_web_view_get_uri(web_view);
        gboolean entry_focused = gtk_widget_has_focus(url_entry);
        if (!url_entry_user_edited) {
            set_url_entry_text(uri);
            if (entry_focused)
                gtk_editable_select_region(GTK_EDITABLE(url_entry), 0, -1);
        } else if (!entry_focused) {
            // A user-typed navigation committed: from here on the URL bar
            // can follow redirects and subsequent loads again.
            set_url_entry_text(uri);
        }
    }

    if (load_event == WEBKIT_LOAD_FINISHED && find_box && gtk_widget_get_visible(find_box)) {
        const gchar *query = (const gchar *)g_object_get_data(G_OBJECT(web_view), "find_query");
        if (query && *query) {
            run_find(web_view, query);
        } else if (find_status) {
            gtk_label_set_text(GTK_LABEL(find_status), "");
        }
    }

    if (load_event == WEBKIT_LOAD_STARTED ||
        load_event == WEBKIT_LOAD_FINISHED ||
        load_event == WEBKIT_LOAD_COMMITTED) {
        update_reload_button(web_view);
        update_progress(web_view);
        update_window_title(web_view);
    }
}

static void on_title_changed(WebKitWebView *web_view, GParamSpec *pspec, GtkWidget *label) {
    const gchar *title = webkit_web_view_get_title(web_view);
    if (title) {
        gtk_label_set_text(GTK_LABEL(label), title);
        gtk_widget_set_tooltip_text(label, title);
        if (web_view == get_active_webview())
            update_window_title(web_view);
    }
}

static void on_tab_close_clicked(GtkButton *btn, GtkWidget *child_page) {
    gint page_num = gtk_notebook_page_num(GTK_NOTEBOOK(notebook), child_page);
    if (page_num >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0)
        gtk_widget_destroy(main_window);
}

static GtkWidget* on_create_tab(WebKitWebView *web_view, WebKitNavigationAction *action, gpointer user_data) {
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const gchar *uri = webkit_uri_request_get_uri(req);
    create_tab(uri ? uri : "https://duckduckgo.com", webview_is_incognito(web_view));
    return GTK_WIDGET(get_active_webview());
}

static void apply_webview_settings(WebKitWebView *web_view) {
    WebKitSettings *web_settings = webkit_web_view_get_settings(web_view);
    webkit_settings_set_enable_developer_extras(web_settings, TRUE);
    webkit_settings_set_hardware_acceleration_policy(web_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
    webkit_settings_set_enable_webgl(web_settings, TRUE);
    webkit_settings_set_enable_smooth_scrolling(web_settings, TRUE);
    webkit_web_view_set_settings(web_view, web_settings);
}

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------

static gchar* permission_origin(WebKitWebView *web_view) {
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri)
        return g_strdup("this page");
    if (g_str_has_prefix(uri, "file://"))
        return g_strdup("local files");

    GError *error = nullptr;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        if (error) g_error_free(error);
        return g_strdup("this page");
    }

    const gchar *host = g_uri_get_host(parsed);
    gchar *origin = g_strdup((host && *host) ? host : "this page");
    g_uri_unref(parsed);
    return origin;
}

static gchar* permission_description(WebKitPermissionRequest *request, const gchar **key) {
    *key = nullptr;

    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        WebKitUserMediaPermissionRequest *media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
        gboolean audio = webkit_user_media_permission_is_for_audio_device(media);
        gboolean video = webkit_user_media_permission_is_for_video_device(media);
        *key = "media";
        if (audio && video) return g_strdup("use your camera and microphone");
        if (video) return g_strdup("use your camera");
        if (audio) return g_strdup("use your microphone");
        return g_strdup("use your media devices");
    }
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request)) {
        *key = "geolocation";
        return g_strdup("know your location");
    }
    if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request)) {
        *key = "notifications";
        return g_strdup("show notifications");
    }
    if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(request)) {
        *key = "clipboard";
        return g_strdup("read from or write to your clipboard");
    }
    if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST(request)) {
        *key = "pointer-lock";
        return g_strdup("lock your mouse pointer");
    }
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request)) {
        *key = "device-info";
        return g_strdup("access information about your devices");
    }
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request)) {
        *key = "drm";
        return g_strdup("use a DRM system");
    }
    if (WEBKIT_IS_XR_PERMISSION_REQUEST(request)) {
        *key = "xr";
        return g_strdup("use extended reality features");
    }
    if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request)) {
        *key = "website-data";
        return g_strdup("access data from another website");
    }
    return nullptr;
}

typedef struct {
    WebKitPermissionRequest *request;
    GtkWidget *remember;
    gchar *key;
    gchar *origin;
    gboolean incognito;
} PermissionPrompt;

static void on_permission_dialog_response(GtkDialog *dialog, gint response, gpointer user_data) {
    PermissionPrompt *prompt = (PermissionPrompt *)user_data;
    gboolean allow = response == GTK_RESPONSE_ACCEPT;

    if (allow)
        webkit_permission_request_allow(prompt->request);
    else
        webkit_permission_request_deny(prompt->request);

    if (!prompt->incognito && prompt->remember &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prompt->remember)))
        remember_permission_decision(prompt->key, prompt->origin, allow);

    g_object_unref(prompt->request);
    g_free(prompt->key);
    g_free(prompt->origin);
    g_free(prompt);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static gboolean on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request,
                                      gpointer user_data) {
    const gchar *key = nullptr;
    gchar *description = permission_description(request, &key);
    if (!description) {
        webkit_permission_request_deny(request);
        return TRUE;
    }

    gchar *origin = permission_origin(web_view);
    gboolean incognito = webview_is_incognito(web_view);

    if (!incognito) {
        gint decision = get_permission_decision(key, origin);
        if (decision >= 0) {
            if (decision)
                webkit_permission_request_allow(request);
            else
                webkit_permission_request_deny(request);
            g_free(description);
            g_free(origin);
            return TRUE;
        }
    }

    PermissionPrompt *prompt = g_new0(PermissionPrompt, 1);
    prompt->request = (WebKitPermissionRequest *)g_object_ref(request);
    prompt->key = g_strdup(key);
    prompt->origin = origin;
    prompt->incognito = incognito;

    gchar *message = g_strdup_printf("%s wants to %s.", origin, description);
    GtkWidget *dialog = gtk_message_dialog_new(
        main_window ? GTK_WINDOW(main_window) : nullptr,
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "%s", message);
    g_free(message);
    g_free(description);

    gtk_window_set_title(GTK_WINDOW(dialog), "Permission request");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Block", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Allow", GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);

    GtkWidget *remember = gtk_check_button_new_with_label("Remember this decision for this site");
    if (incognito) {
        gtk_button_set_label(GTK_BUTTON(remember), "Not remembered in incognito");
        gtk_widget_set_sensitive(remember, FALSE);
    } else if (g_strcmp0(origin, "this page") == 0) {
        gtk_button_set_label(GTK_BUTTON(remember), "Not remembered for this page");
        gtk_widget_set_sensitive(remember, FALSE);
    }
    gtk_widget_set_margin_start(remember, 12);
    gtk_widget_set_margin_end(remember, 12);
    gtk_widget_set_margin_bottom(remember, 8);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                       remember, FALSE, FALSE, 0);
    prompt->remember = remember;

    g_signal_connect(dialog, "response", G_CALLBACK(on_permission_dialog_response), prompt);
    gtk_widget_show_all(dialog);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

static WebKitWebContext* get_incognito_context() {
    if (!incognito_context) {
        incognito_context = webkit_web_context_new_ephemeral();
        g_signal_connect(incognito_context, "download-started", G_CALLBACK(on_download_started), NULL);
    }
    return incognito_context;
}

static void create_tab(const gchar *url, gboolean incognito) {
    WebKitWebView *web_view;

    if (incognito) {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "web-context", get_incognito_context(),
            "user-content-manager", global_content_manager,
            NULL));
    } else {
        web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "user-content-manager", global_content_manager, NULL));
    }
    g_object_set_data(G_OBJECT(web_view), "is_incognito", GINT_TO_POINTER(incognito ? 1 : 0));

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

    WebKitFindController *find_controller = webkit_web_view_get_find_controller(web_view);
    g_signal_connect(find_controller, "found-text", G_CALLBACK(on_find_found), web_view);
    g_signal_connect(find_controller, "failed-to-find-text", G_CALLBACK(on_find_failed), web_view);

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), GTK_WIDGET(web_view));

    gtk_widget_show_all(tab_box);
    gtk_widget_show(GTK_WIDGET(web_view));

    gint index = gtk_notebook_append_page(GTK_NOTEBOOK(notebook), GTK_WIDGET(web_view), tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook), GTK_WIDGET(web_view), TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), index);
    webkit_web_view_load_uri(web_view, url);
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
static void focus_url_bar() {
    gtk_widget_grab_focus(url_entry);
    gtk_editable_select_region(GTK_EDITABLE(url_entry), 0, -1);
}

static void on_new_tab_clicked(GtkButton *btn, gpointer user_data) {
    create_new_tab("https://duckduckgo.com");
    focus_url_bar();
}

static void on_tab_switched(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer user_data) {
    if (!page || !WEBKIT_IS_WEB_VIEW(page))
        return;
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(page);

    const gchar *uri = webkit_web_view_get_uri(web_view);
    set_url_entry_text(uri);

    GtkStyleContext *context = gtk_widget_get_style_context(url_entry);
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

static const gchar *DARK_MODE_STYLE_JS =
    "(function(){var id='kite-dark-mode-style';"
    "if(document.getElementById(id))return;"
    "var style=document.createElement('style');style.id=id;"
    "style.textContent='html { filter: invert(90%) hue-rotate(180deg) !important; background-color: #121212 !important; } "
    "img, video, canvas, [style*=\"background-image\"] { filter: invert(111%) hue-rotate(180deg) !important; }';"
    "(document.head||document.documentElement).appendChild(style);})();";

static const gchar *DARK_MODE_REMOVE_JS =
    "var el=document.getElementById('kite-dark-mode-style'); if (el) el.remove();";

static bool dark_script_added = false;

static void set_webview_dark_mode(WebKitWebView *web_view, bool enable) {
    webkit_web_view_evaluate_javascript(web_view,
        enable ? DARK_MODE_STYLE_JS : DARK_MODE_REMOVE_JS,
        -1, NULL, NULL, NULL, NULL, NULL);
}

static void set_dark_mode_script(bool enable) {
    if (!global_content_manager || dark_script_added == enable)
        return;

    if (enable) {
        if (!dark_mode_script)
            dark_mode_script = webkit_user_script_new(
                DARK_MODE_STYLE_JS,
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
                NULL, NULL);
        webkit_user_content_manager_add_script(global_content_manager, dark_mode_script);
    } else if (dark_mode_script) {
        webkit_user_content_manager_remove_script(global_content_manager, dark_mode_script);
    }
    dark_script_added = enable;
}

static void apply_dark_mode(bool enable) {
    global_dark_mode = enable;
    set_dark_mode_script(enable);

    gint n_pages = gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook));
    for (gint i = 0; i < n_pages; i++) {
        GtkWidget *child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(notebook), i);
        set_webview_dark_mode(WEBKIT_WEB_VIEW(child), enable);
    }
    save_settings();
}

static void init_dark_mode() {
    if (!dark_mode_script)
        dark_mode_script = webkit_user_script_new(
            DARK_MODE_STYLE_JS,
            WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
            NULL, NULL);
    if (global_dark_mode)
        set_dark_mode_script(true);
}

static void on_dark_mode_toggled(GtkToggleButton *button, gpointer user_data) {
    apply_dark_mode(gtk_toggle_button_get_active(button));
}

static void toggle_dark_mode() {
    if (!btn_dark) {
        apply_dark_mode(!global_dark_mode);
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_dark),
                                 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn_dark)));
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
            focus_url_bar();
            return TRUE;
        }

        switch (event->keyval) {
            case GDK_KEY_f:
            case GDK_KEY_F:
                sync_find_bar(get_active_webview());
                gtk_widget_show(find_box);
                gtk_widget_grab_focus(find_entry);
                gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1);
                if (*gtk_entry_get_text(GTK_ENTRY(find_entry)) && get_active_webview())
                    run_find(get_active_webview(), gtk_entry_get_text(GTK_ENTRY(find_entry)));
                return TRUE;
            case GDK_KEY_t:
                create_new_tab("https://duckduckgo.com");
                focus_url_bar();
                return TRUE;
            case GDK_KEY_w: {
                gint page_num = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
                if (page_num >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(notebook), page_num);
                if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)) == 0)
                    gtk_widget_destroy(main_window);
                return TRUE;
            }
            case GDK_KEY_l:
                focus_url_bar();
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
    load_settings();

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, 
        ".incognito-mode { background: #1a0b2e; color: #b28ced; border-bottom: 2px solid #8e44ad; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), 
        GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    init_adblocker();
    init_dark_mode();

    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    g_signal_connect(context, "download-started", G_CALLBACK(on_download_started), NULL);

    main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_signal_connect(main_window, "key-press-event", G_CALLBACK(on_key_press), NULL);
    gtk_window_set_default_size(GTK_WINDOW(main_window), 1200, 800);
    gtk_window_set_title(GTK_WINDOW(main_window), "Kite Browser");
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(main_window), vbox);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 4);

    GtkWidget *btn_back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *btn_forward = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_BUTTON);
    btn_reload = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_always_show_image(GTK_BUTTON(btn_reload), TRUE);
    GtkWidget *btn_new_tab = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
    btn_dark = gtk_toggle_button_new();
    gtk_button_set_image(GTK_BUTTON(btn_dark),
        gtk_image_new_from_icon_name("weather-clear-night-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_dark), global_dark_mode);

    gtk_widget_set_tooltip_text(btn_reload, "Reload");
    gtk_widget_set_tooltip_text(btn_dark, "Toggle Dark Mode (F9 / Ctrl+Shift+D)");

    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_clicked), NULL);
    g_signal_connect(btn_forward, "clicked", G_CALLBACK(on_forward_clicked), NULL);
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload_clicked), NULL);
    g_signal_connect(btn_new_tab, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(btn_dark, "toggled", G_CALLBACK(on_dark_mode_toggled), NULL);

    gtk_box_pack_start(GTK_BOX(toolbar), btn_back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_forward, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_reload, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_new_tab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), btn_dark, FALSE, FALSE, 0);

    url_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(toolbar), url_entry, TRUE, TRUE, 2);
    g_signal_connect(url_entry, "activate", G_CALLBACK(on_url_activate), NULL);
    g_signal_connect(url_entry, "changed", G_CALLBACK(on_url_entry_changed), NULL);

    progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress_bar), FALSE);
    gtk_widget_set_size_request(progress_bar, -1, 4);
    gtk_box_pack_start(GTK_BOX(vbox), progress_bar, FALSE, FALSE, 0);

    notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_tab_switched), NULL);

  
    find_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    
    
    find_entry = gtk_search_entry_new();
    find_status = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(find_status), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(find_status), 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(find_status), "dim-label");

    GtkWidget *find_prev = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_next = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
    GtkWidget *find_close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
    
    gtk_box_pack_start(GTK_BOX(find_box), find_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(find_box), find_close, FALSE, FALSE, 0);
    
    g_signal_connect(find_entry, "search-changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_activate), NULL);
    g_signal_connect(find_entry, "stop-search", G_CALLBACK(on_find_stop_search), NULL);
    g_signal_connect(find_prev, "clicked", G_CALLBACK(on_find_prev), NULL);
    g_signal_connect(find_next, "clicked", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(find_close, "clicked", G_CALLBACK(on_find_close), NULL);
    
    gtk_box_pack_start(GTK_BOX(vbox), find_box, FALSE, FALSE, 2);
    create_new_tab("https://duckduckgo.com");

    gtk_widget_show_all(main_window);
    gtk_widget_hide(find_box);
    gtk_widget_hide(progress_bar);
    focus_url_bar();
    gtk_main();

    return 0;
}