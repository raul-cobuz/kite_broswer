#include "page_tools.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <string>

#include "browser_window.h"

namespace {

struct ScreenshotRequest {
    WebKitWebView *view = nullptr;
    gchar *path = nullptr;
    gboolean full_page = FALSE;
};

std::string screenshot_directory() {
    const gchar *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pictures && *pictures)
        return pictures;
    return g_get_home_dir();
}

std::string safe_filename(const gchar *title) {
    std::string name = title && *title ? title : "Screenshot";
    for (char &c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || (unsigned char)c < 32)
            c = '-';
    }
    if (name.size() > 90)
        name.resize(90);
    if (name.empty())
        name = "Screenshot";
    return name;
}

void show_result_dialog(GtkMessageType type, const gchar *message) {
    GtkWidget *dialog = gtk_message_dialog_new(
        browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr, GTK_DIALOG_DESTROY_WITH_PARENT, type,
        GTK_BUTTONS_OK, "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void on_snapshot_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
    ScreenshotRequest *request = static_cast<ScreenshotRequest *>(user_data);
    GError *error = nullptr;
    cairo_surface_t *surface =
        webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);

    bool saved = false;
    if (surface) {
        saved = cairo_surface_write_to_png(surface, request->path) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(surface);
    }

    if (saved) {
        gchar *message =
            g_strdup_printf("Screenshot saved to\n%s", request->path);
        show_result_dialog(GTK_MESSAGE_INFO, message);
        g_free(message);
    } else {
        gchar *message = g_strdup_printf("Could not save the screenshot%s%s.",
                                         error && error->message ? ": " : "",
                                         error && error->message ? error->message : "");
        show_result_dialog(GTK_MESSAGE_ERROR, message);
        g_free(message);
    }

    g_clear_error(&error);
    if (request->view)
        g_object_unref(request->view);
    g_free(request->path);
    g_free(request);
}

} // namespace

void print_active_page() {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view) {
        g_printerr("[Kite] There is no active page to print.\n");
        return;
    }

    WebKitPrintOperation *operation = webkit_print_operation_new(web_view);
    if (!operation)
        return;
    webkit_print_operation_run_dialog(operation, browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr);
    g_object_unref(operation);
}

void screenshot_active_page(bool full_page) {
    WebKitWebView *web_view = get_active_webview();
    if (!web_view) {
        show_result_dialog(GTK_MESSAGE_WARNING, "There is no active page to capture.");
        return;
    }

    GDateTime *now = g_date_time_new_now_local();
    gchar *stamp = now ? g_date_time_format(now, "%Y-%m-%d %H-%M-%S") : g_strdup("");
    if (now)
        g_date_time_unref(now);

    const gchar *title = webkit_web_view_get_title(web_view);
    std::string name = safe_filename(title);
    gchar *suggested = g_strdup_printf("%s %s.png", name.c_str(), stamp && *stamp ? stamp : "screenshot");
    g_free(stamp);

    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        full_page ? "Save full-page screenshot" : "Save screenshot",
        browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr, GTK_FILE_CHOOSER_ACTION_SAVE, "Cancel",
        GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    std::string directory = screenshot_directory();
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), directory.c_str());
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested);
    g_free(suggested);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PNG images");
    gtk_file_filter_add_pattern(filter, "*.png");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialog);
        return;
    }

    gchar *selected = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    gtk_widget_destroy(dialog);
    if (!selected || !*selected) {
        g_free(selected);
        return;
    }

    std::string path = selected;
    g_free(selected);
    if (path.size() < 4 || g_ascii_strcasecmp(path.c_str() + path.size() - 4, ".png") != 0)
        path += ".png";

    ScreenshotRequest *request = g_new0(ScreenshotRequest, 1);
    request->view = WEBKIT_WEB_VIEW(g_object_ref(web_view));
    request->path = g_strdup(path.c_str());
    request->full_page = full_page ? TRUE : FALSE;

    webkit_web_view_get_snapshot(
        web_view, full_page ? WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT : WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, on_snapshot_ready, request);
}
