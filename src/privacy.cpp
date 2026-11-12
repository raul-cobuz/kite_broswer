#include "privacy.h"

#include <glib.h>

#include <string>

#include "adblock.h"
#include "app_context.h"
#include "browser_window.h"
#include "profile.h"
#include "settings.h"
#include "tracking_protection.h"
#include "url_utils.h"

static GtkWidget *privacy_button = nullptr;
static GtkWidget *global_switch = nullptr;
static GtkWidget *site_switch = nullptr;
static GtkWidget *site_label = nullptr;
static GtkWidget *clear_button = nullptr;
static GtkWidget *status_label = nullptr;
static GtkWidget *strip_check = nullptr;
static GtkWidget *clear_on_exit_check = nullptr;
static bool updating_controls = false;
static std::string shown_host;

typedef struct {
    gchar *host;
    GList *fetched;
} SiteClearRequest;

static std::string view_host(WebKitWebView *web_view) {
    if (!web_view)
        return "";
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
        return "";
    return url_host(uri);
}

static void set_status(const gchar *text) {
    if (status_label)
        gtk_label_set_text(GTK_LABEL(status_label), text ? text : "");
}

static void free_website_data(gpointer data) {
    webkit_website_data_unref((WebKitWebsiteData *)data);
}

static WebKitWebsiteDataTypes persisted_types() {
    return (WebKitWebsiteDataTypes)(WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE |
                                    WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE |
                                    WEBKIT_WEBSITE_DATA_SESSION_STORAGE | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE |
                                    WEBKIT_WEBSITE_DATA_WEBSQL_DATABASES |
                                    WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | WEBKIT_WEBSITE_DATA_COOKIES |
                                    WEBKIT_WEBSITE_DATA_ITP | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS |
                                    WEBKIT_WEBSITE_DATA_DOM_CACHE);
}

void privacy_apply_context(WebKitWebContext *context) {
    if (!context)
        return;

    WebKitWebsiteDataManager *manager = webkit_web_context_get_website_data_manager(context);
    WebKitCookieManager *cookies = webkit_web_context_get_cookie_manager(context);
    if (cookies) {
        // WebKitGTK keeps cookies in memory unless persistent storage is
        // configured. Without this every launch looks like a brand-new browser
        // to sites, which makes Google in particular treat searches as bot
        // traffic. Ephemeral (incognito) contexts keep their in-memory store.
        if (!manager || !webkit_website_data_manager_is_ephemeral(manager)) {
            gchar *path = g_build_filename(profile_data_dir(), "cookies.sqlite", nullptr);
            gchar *dir = g_path_get_dirname(path);
            g_mkdir_with_parents(dir, 0700);
            g_free(dir);
            webkit_cookie_manager_set_persistent_storage(cookies, path, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
            g_printerr("[Kite DEBUG] persistent cookies=%s\n", path);
            g_free(path);
        }
        webkit_cookie_manager_set_accept_policy(cookies, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
    }

    if (manager)
        webkit_website_data_manager_set_itp_enabled(manager, TRUE);
}

static void on_clear_on_exit_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *error = nullptr;
    webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, &error);
    if (error) {
        g_printerr("[Kite] Could not clear site data on exit: %s\n", error->message);
        g_error_free(error);
    } else {
        g_print("[Kite] Cookies and site data cleared.\n");
    }
    g_main_loop_quit((GMainLoop *)user_data);
}

void privacy_clear_on_exit() {
    if (!settings_clear_on_exit())
        return;

    WebKitWebsiteDataManager *manager =
        webkit_web_context_get_website_data_manager(app_webkit_context());
    if (!manager)
        return;

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    webkit_website_data_manager_clear(manager, persisted_types(), 0, nullptr, on_clear_on_exit_finished, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

static bool name_matches_site(const gchar *name, const gchar *host) {
    if (!name || !*name || !host || !*host)
        return false;

    std::string candidate = name;
    if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0) {
        std::string parsed = url_host(candidate);
        if (!parsed.empty())
            candidate = parsed;
    }
    while (!candidate.empty() && candidate[0] == '.')
        candidate.erase(0, 1);

    if (candidate == host)
        return true;
    if (candidate.size() <= std::string(host).size())
        return false;
    size_t offset = candidate.size() - std::string(host).size();
    return candidate.compare(offset, std::string::npos, host) == 0 && candidate[offset - 1] == '.';
}

static void on_site_data_removed(GObject *source, GAsyncResult *res, gpointer user_data) {
    SiteClearRequest *request = (SiteClearRequest *)user_data;
    GError *error = nullptr;
    gboolean ok = webkit_website_data_manager_remove_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, &error);

    if (ok) {
        gchar *text = g_strdup_printf("Cookies and site data cleared for %s.", request->host);
        set_status(text);
        g_free(text);
    } else {
        gchar *text = g_strdup_printf("Could not clear data for %s: %s", request->host,
                                      error ? error->message : "unknown error");
        set_status(text);
        g_free(text);
        if (error)
            g_error_free(error);
    }

    g_list_free_full(request->fetched, free_website_data);
    g_free(request->host);
    g_free(request);
}

static void on_site_data_fetched(GObject *source, GAsyncResult *res, gpointer user_data) {
    SiteClearRequest *request = (SiteClearRequest *)user_data;
    WebKitWebsiteDataManager *manager = WEBKIT_WEBSITE_DATA_MANAGER(source);
    GError *error = nullptr;
    GList *fetched = webkit_website_data_manager_fetch_finish(manager, res, &error);

    if (error) {
        gchar *text = g_strdup_printf("Could not read site data for %s.", request->host);
        set_status(text);
        g_free(text);
        g_error_free(error);
        g_free(request->host);
        g_free(request);
        return;
    }

    GList *matches = nullptr;
    for (GList *item = fetched; item; item = item->next) {
        WebKitWebsiteData *data = (WebKitWebsiteData *)item->data;
        if (name_matches_site(webkit_website_data_get_name(data), request->host))
            matches = g_list_prepend(matches, data);
    }

    if (!matches) {
        gchar *text = g_strdup_printf("No cookies or site data found for %s.", request->host);
        set_status(text);
        g_free(text);
        g_list_free_full(fetched, free_website_data);
        g_free(request->host);
        g_free(request);
        return;
    }

    request->fetched = fetched;
    webkit_website_data_manager_remove(manager, WEBKIT_WEBSITE_DATA_ALL, matches, nullptr, on_site_data_removed,
                                       request);
    g_list_free(matches);
}

static void on_clear_site_data_clicked(GtkButton *button, gpointer user_data) {
    WebKitWebView *web_view = get_active_webview();
    std::string host = view_host(web_view);
    if (host.empty())
        return;

    GtkWidget *parent = browser.main_window ? browser.main_window : nullptr;
    GtkWidget *dialog = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : nullptr,
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Clear cookies and site data for %s?", host.c_str());
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL, "_Clear", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT)
        return;

    WebKitWebsiteDataManager *manager =
        webkit_web_context_get_website_data_manager(webkit_web_view_get_context(web_view));
    if (!manager) {
        set_status("Site data is unavailable for this page.");
        return;
    }

    gchar *text = g_strdup_printf("Clearing cookies and site data for %s...", host.c_str());
    set_status(text);
    g_free(text);

    SiteClearRequest *request = g_new0(SiteClearRequest, 1);
    request->host = g_strdup(host.c_str());
    webkit_website_data_manager_fetch(manager, WEBKIT_WEBSITE_DATA_ALL, nullptr, on_site_data_fetched, request);
}

void update_privacy_button(WebKitWebView *web_view) {
    if (!privacy_button || !global_switch || !site_switch)
        return;

    WebKitWebView *view = web_view ? web_view : get_active_webview();
    gboolean incognito = webview_is_incognito(view);
    std::string host = view_host(view);

    if (shown_host != host) {
        shown_host = host;
        set_status("");
    }

    gboolean global = settings_block_ads();
    gboolean site_allowed = !host.empty() && settings_site_adblock_enabled(host.c_str());

    updating_controls = true;
    gtk_switch_set_active(GTK_SWITCH(global_switch), global);
    if (incognito)
        gtk_switch_set_active(GTK_SWITCH(site_switch), global);
    else
        gtk_switch_set_active(GTK_SWITCH(site_switch), site_allowed);
    if (strip_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(strip_check), settings_strip_tracking_params());
    if (clear_on_exit_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clear_on_exit_check), settings_clear_on_exit());
    updating_controls = false;

    if (incognito) {
        gtk_label_set_text(GTK_LABEL(site_label), "Incognito tab");
        gtk_widget_set_sensitive(site_switch, FALSE);
        gtk_widget_set_tooltip_text(site_switch, "Site controls are not saved in incognito");
    } else if (host.empty()) {
        gtk_label_set_text(GTK_LABEL(site_label), "This page");
        gtk_widget_set_sensitive(site_switch, FALSE);
        gtk_widget_set_tooltip_text(site_switch, "Open an http or https page to control ads");
    } else {
        gtk_label_set_text(GTK_LABEL(site_label), host.c_str());
        gtk_widget_set_sensitive(site_switch, global);
        gtk_widget_set_tooltip_text(site_switch, NULL);
    }

    gtk_widget_set_sensitive(clear_button, !host.empty() && !incognito);
}

static void on_global_switch_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
    if (updating_controls)
        return;

    gboolean enabled = gtk_switch_get_active(GTK_SWITCH(object));
    settings_set_block_ads(enabled);
    save_settings();
    adblock_set_enabled(enabled);
    update_privacy_button(get_active_webview());
    set_status(enabled ? "Ad and tracker blocking enabled. Reload pages to apply."
                       : "Ad and tracker blocking disabled.");
}

static void on_site_switch_notify(GObject *object, GParamSpec *pspec, gpointer user_data) {
    if (updating_controls)
        return;

    WebKitWebView *web_view = get_active_webview();
    if (!web_view || webview_is_incognito(web_view))
        return;

    std::string host = view_host(web_view);
    if (host.empty())
        return;

    gboolean enabled = gtk_switch_get_active(GTK_SWITCH(object));
    settings_set_site_adblock_enabled(host.c_str(), enabled);
    save_settings();
    adblock_reload_exceptions();
    set_status("Reload the page to apply the site setting.");
}

static void on_strip_check_toggled(GtkToggleButton *button, gpointer user_data) {
    if (updating_controls)
        return;
    bool enabled = gtk_toggle_button_get_active(button);
    settings_set_strip_tracking_params(enabled);
    save_settings();
    tracking_protection_set_enabled(enabled);
    set_status(enabled ? "Tracking parameters will be stripped from links. Reload pages to apply."
                       : "Tracking parameters are no longer stripped from links.");
}

static void on_clear_on_exit_toggled(GtkToggleButton *button, gpointer user_data) {
    if (updating_controls)
        return;
    bool enabled = gtk_toggle_button_get_active(button);
    settings_set_clear_on_exit(enabled);
    save_settings();
    set_status(enabled ? "Cookies and site data will be cleared when Kite closes."
                       : "Cookies and site data will be kept when Kite closes.");
}

static GtkWidget *make_switch_row(const gchar *text, GtkWidget **out_switch, GtkWidget **out_label) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);

    GtkWidget *toggle = gtk_switch_new();
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), toggle, FALSE, FALSE, 0);

    if (out_switch)
        *out_switch = toggle;
    if (out_label)
        *out_label = label;
    return row;
}

GtkWidget *create_privacy_button() {
    if (privacy_button)
        return privacy_button;

    privacy_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(privacy_button),
                         gtk_image_new_from_icon_name("security-high-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(privacy_button), TRUE);
    gtk_widget_set_tooltip_text(privacy_button, "Privacy controls");

    GtkWidget *popover = gtk_popover_new(privacy_button);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(privacy_button), popover);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(content, 330, -1);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_container_add(GTK_CONTAINER(popover), content);

    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Privacy</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), make_switch_row("Block ads and trackers", &global_switch, nullptr), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    site_label = gtk_label_new("This page");
    gtk_label_set_xalign(GTK_LABEL(site_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(site_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(site_label), 38);
    gtk_style_context_add_class(gtk_widget_get_style_context(site_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(content), site_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), make_switch_row("Block ads on this site", &site_switch, nullptr), FALSE,
                       FALSE, 0);

    clear_button = gtk_button_new_with_label("Clear cookies and site data");
    gtk_style_context_add_class(gtk_widget_get_style_context(clear_button), "destructive-action");
    gtk_box_pack_start(GTK_BOX(content), clear_button, FALSE, FALSE, 0);

    status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(status_label), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_label), "dim-label");
    gtk_box_pack_start(GTK_BOX(content), status_label, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    strip_check = gtk_check_button_new_with_label("Strip tracking parameters from links");
    gtk_box_pack_start(GTK_BOX(content), strip_check, FALSE, FALSE, 0);
    clear_on_exit_check = gtk_check_button_new_with_label("Clear cookies and site data on exit");
    gtk_box_pack_start(GTK_BOX(content), clear_on_exit_check, FALSE, FALSE, 0);

    GtkWidget *note = gtk_label_new("Third-party cookies and cross-site tracking are blocked automatically.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(content), note, FALSE, FALSE, 0);

    g_signal_connect(global_switch, "notify::active", G_CALLBACK(on_global_switch_notify), nullptr);
    g_signal_connect(site_switch, "notify::active", G_CALLBACK(on_site_switch_notify), nullptr);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_site_data_clicked), nullptr);
    g_signal_connect(strip_check, "toggled", G_CALLBACK(on_strip_check_toggled), nullptr);
    g_signal_connect(clear_on_exit_check, "toggled", G_CALLBACK(on_clear_on_exit_toggled), nullptr);

    gtk_widget_show_all(content);
    update_privacy_button(nullptr);
    return privacy_button;
}
