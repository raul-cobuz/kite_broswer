#include "errors.h"

#include "browser_window.h"

static void show_error_page(WebKitWebView *web_view, const gchar *uri, const gchar *heading, const gchar *details) {
    gchar *safe_uri = g_markup_escape_text(uri ? uri : "", -1);
    gchar *safe_heading = g_markup_escape_text(heading, -1);
    gchar *safe_details = g_markup_escape_text(details ? details : "", -1);
    gchar *retry =
        (uri && *uri) ? g_strdup_printf("<p><a href=\"%s\">Try again</a></p>", safe_uri) : g_strdup("");

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

    webkit_web_view_load_alternate_html(web_view, html, (uri && *uri) ? uri : "about:blank",
                                        (uri && *uri) ? uri : "about:blank");

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

gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event, const gchar *failing_uri, GError *error,
                        gpointer user_data) {
    if (error->domain == WEBKIT_NETWORK_ERROR && error->code == WEBKIT_NETWORK_ERROR_CANCELLED)
        return FALSE;
    if (error->domain == WEBKIT_POLICY_ERROR && error->code == WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)
        return FALSE;

    gchar *details = g_strdup_printf("Error: %s", error->message);
    show_error_page(web_view, failing_uri, "This page could not be loaded", details);
    g_free(details);

    set_tab_error_label(web_view, "Failed to load", failing_uri);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(browser.main_window), "Failed to load");
        update_reload_button(web_view);
        update_progress(web_view);
    }
    return TRUE;
}

gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view, const gchar *failing_uri, GTlsCertificate *certificate,
                                        GTlsCertificateFlags errors, gpointer user_data) {
    show_error_page(web_view, failing_uri, "Insecure connection",
                    "The certificate presented by this site could not be verified, "
                    "so the connection was not established.");

    set_tab_error_label(web_view, "Insecure connection", failing_uri);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(browser.main_window), "Insecure connection");
        update_reload_button(web_view);
        update_progress(web_view);
    }
    return TRUE;
}

void on_web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data) {
    const gchar *message = "The web process crashed while loading this page.";
    if (reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT)
        message = "The web process ran out of memory while loading this page.";
    else if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
        message = "The web process was terminated.";

    show_error_page(web_view, webkit_web_view_get_uri(web_view), "Page process stopped", message);

    set_tab_error_label(web_view, "Crashed", message);
    if (web_view == get_active_webview()) {
        gtk_window_set_title(GTK_WINDOW(browser.main_window), "Page crashed");
        update_reload_button(web_view);
        update_progress(web_view);
    }
}
