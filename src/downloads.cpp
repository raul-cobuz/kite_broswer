#include "downloads.h"

#include <glib/gstdio.h>

#include <string>

#include "browser_window.h"
#include "settings.h"

typedef struct {
    GtkWidget *row;
    GtkWidget *name_label;
    GtkWidget *progress;
    GtkWidget *status;
    GtkWidget *open_button;
    GtkWidget *cancel_button;
    WebKitDownload *download;
    gchar *filename;
    gchar *destination;
    gulong progress_id;
    gulong finished_id;
    gulong failed_id;
    gulong decide_id;
    gboolean terminal;
    gboolean succeeded;
} DownloadItem;

static GtkWidget *downloads_button = nullptr;
static GtkWidget *downloads_box = nullptr;
static GtkWidget *downloads_empty = nullptr;
static GtkWidget *downloads_scroller = nullptr;

static void download_item_free(gpointer data) {
    DownloadItem *item = (DownloadItem *)data;
    if (!item)
        return;
    if (item->download) {
        if (item->progress_id)
            g_signal_handler_disconnect(item->download, item->progress_id);
        if (item->finished_id)
            g_signal_handler_disconnect(item->download, item->finished_id);
        if (item->failed_id)
            g_signal_handler_disconnect(item->download, item->failed_id);
        if (item->decide_id)
            g_signal_handler_disconnect(item->download, item->decide_id);
        g_object_unref(item->download);
    }
    g_free(item->filename);
    g_free(item->destination);
    g_free(item);
}

static void update_downloads_empty_state() {
    if (!downloads_box || !downloads_empty || !downloads_scroller)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(downloads_box));
    gboolean empty = children == nullptr;
    g_list_free(children);
    gtk_widget_set_visible(downloads_empty, empty);
    gtk_widget_set_visible(downloads_scroller, !empty);
}

static gchar *download_suggested_filename(WebKitDownload *download) {
    const gchar *suggested = nullptr;
    WebKitURIResponse *response = webkit_download_get_response(download);
    if (response)
        suggested = webkit_uri_response_get_suggested_filename(response);

    gchar *name = nullptr;
    if (suggested && *suggested) {
        gchar *decoded = g_uri_unescape_string(suggested, nullptr);
        name = g_path_get_basename(decoded ? decoded : suggested);
        g_free(decoded);
    } else {
        WebKitURIRequest *request = webkit_download_get_request(download);
        const gchar *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
        GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr) : nullptr;
        if (parsed) {
            const gchar *path = g_uri_get_path(parsed);
            if (path && *path) {
                gchar *decoded = g_uri_unescape_string(path, nullptr);
                name = g_path_get_basename(decoded ? decoded : path);
                g_free(decoded);
            }
            g_uri_unref(parsed);
        }
    }

    if (!name || !*name || g_strcmp0(name, "/") == 0 || g_strcmp0(name, ".") == 0) {
        g_free(name);
        name = g_strdup("download");
    }
    return name;
}

static void on_open_clicked(GtkButton *button, gpointer user_data) {
    DownloadItem *item = (DownloadItem *)user_data;
    const gchar *destination = item->destination;
    if (!destination && item->download)
        destination = webkit_download_get_destination(item->download);
    if (!destination || !*destination)
        return;

    gchar *uri = g_strdup(destination);
    if (g_path_is_absolute(uri)) {
        gchar *file_uri = g_filename_to_uri(uri, nullptr, nullptr);
        if (file_uri) {
            g_free(uri);
            uri = file_uri;
        }
    }

    GError *error = nullptr;
    gtk_show_uri_on_window(browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr, uri, GDK_CURRENT_TIME,
                           &error);
    if (error) {
        g_printerr("[Kite] Could not open download: %s\n", error->message);
        g_error_free(error);
    }
    g_free(uri);
}

static void on_cancel_clicked(GtkButton *button, gpointer user_data) {
    DownloadItem *item = (DownloadItem *)user_data;
    if (!item || item->terminal || !item->download)
        return;
    webkit_download_cancel(item->download);
    gtk_label_set_text(GTK_LABEL(item->status), "Cancelling...");
    gtk_widget_set_sensitive(item->cancel_button, FALSE);
}

static void on_download_progress_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    DownloadItem *item = (DownloadItem *)user_data;
    if (!item || item->terminal || !item->download)
        return;
    gdouble progress = webkit_download_get_estimated_progress(item->download);
    progress = CLAMP(progress, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(item->progress), progress);
    gchar *text = g_strdup_printf("Downloading... %d%%", (int)(progress * 100.0));
    gtk_label_set_text(GTK_LABEL(item->status), text);
    g_free(text);
}

static void on_download_finished(WebKitDownload *download, gpointer user_data) {
    DownloadItem *item = (DownloadItem *)user_data;
    if (!item)
        return;
    item->terminal = TRUE;
    item->succeeded = TRUE;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(item->progress), 1.0);
    gtk_label_set_text(GTK_LABEL(item->status), "Completed");
    gtk_widget_set_visible(item->cancel_button, FALSE);
    gtk_widget_set_visible(item->open_button, TRUE);
    const gchar *destination = webkit_download_get_destination(download);
    if (destination) {
        g_free(item->destination);
        item->destination = g_strdup(destination);
    }
}

static void on_download_failed(WebKitDownload *download, GError *error, gpointer user_data) {
    g_printerr("[Kite DEBUG] download-failed download=%p error=%s\n", (void *)download, error ? error->message : "(null)");
    DownloadItem *item = (DownloadItem *)user_data;
    if (!item)
        return;
    item->terminal = TRUE;
    item->succeeded = FALSE;
    gtk_widget_set_visible(item->cancel_button, FALSE);
    gtk_widget_set_visible(item->open_button, FALSE);
    if (error && error->domain == WEBKIT_DOWNLOAD_ERROR && error->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER) {
        gtk_label_set_text(GTK_LABEL(item->status), "Cancelled");
    } else {
        gchar *text = g_strdup_printf("Failed: %s", error ? error->message : "unknown error");
        gtk_label_set_text(GTK_LABEL(item->status), text);
        g_free(text);
    }
}

static DownloadItem *create_download_row(WebKitDownload *download, const gchar *filename) {
    DownloadItem *item = g_new0(DownloadItem, 1);
    item->download = (WebKitDownload *)g_object_ref(download);
    item->filename = g_strdup(filename);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    item->row = row;

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(filename);
    item->name_label = label;
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 38);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_pack_start(GTK_BOX(top), label, TRUE, TRUE, 0);

    item->cancel_button = gtk_button_new_from_icon_name("process-stop-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(item->cancel_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(item->cancel_button, "Cancel download");
    gtk_box_pack_start(GTK_BOX(top), item->cancel_button, FALSE, FALSE, 0);

    item->open_button = gtk_button_new_with_label("Open");
    gtk_button_set_relief(GTK_BUTTON(item->open_button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(item->open_button, "Open downloaded file");
    gtk_box_pack_start(GTK_BOX(top), item->open_button, FALSE, FALSE, 0);

    item->progress = gtk_progress_bar_new();
    item->status = gtk_label_new("Downloading...");
    gtk_label_set_xalign(GTK_LABEL(item->status), 0.0f);
    gtk_style_context_add_class(gtk_widget_get_style_context(item->status), "dim-label");

    gtk_box_pack_start(GTK_BOX(row), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), item->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), item->status, FALSE, FALSE, 0);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);

    g_object_set_data_full(G_OBJECT(row), "kite-download-item", item, download_item_free);

    g_signal_connect(item->cancel_button, "clicked", G_CALLBACK(on_cancel_clicked), item);
    g_signal_connect(item->open_button, "clicked", G_CALLBACK(on_open_clicked), item);
    item->progress_id =
        g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress_changed), item);
    item->finished_id = g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), item);
    item->failed_id = g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), item);

    gtk_box_pack_start(GTK_BOX(downloads_box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(row);
    gtk_widget_set_visible(item->open_button, FALSE);
    update_downloads_empty_state();
    return item;
}

static gboolean on_decide_destination(WebKitDownload *download, const gchar *suggested_filename,
                                      gpointer user_data) {
    g_printerr("[Kite DEBUG] decide-destination fired download=%p suggested=%s\n", (void *)download, suggested_filename ? suggested_filename : "(null)");
    DownloadItem *item = (DownloadItem *)user_data;
    const gchar *filename = suggested_filename && *suggested_filename ? suggested_filename : nullptr;
    if (!filename && item)
        filename = item->filename;
    if (!filename || !*filename)
        filename = "download";

    gchar *safe_filename = g_path_get_basename(filename);
    filename = safe_filename;

    if (item && item->name_label && g_strcmp0(item->filename, filename) != 0) {
        g_free(item->filename);
        item->filename = g_strdup(filename);
        gtk_label_set_text(GTK_LABEL(item->name_label), filename);
    }

    // Every download asks for a destination. The configured download
    // directory is only the folder the chooser starts in.
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Save file", browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr,
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), filename);
    const gchar *directory = settings_download_directory();
    if (directory && *directory && g_file_test(directory, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), directory);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        gchar *uri = path ? g_filename_to_uri(path, nullptr, nullptr) : nullptr;
        if (uri)
            webkit_download_set_destination(download, uri);
        else
            webkit_download_cancel(download);
        if (item && path) {
            g_free(item->destination);
            item->destination = g_strdup(path);
        }
        g_free(uri);
        g_free(path);
    } else {
        webkit_download_cancel(download);
    }
    gtk_widget_destroy(dialog);
    g_free(safe_filename);
    return TRUE;
}

void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data) {
    g_printerr("[Kite DEBUG] download-started fired context=%p download=%p\n", (void *)context, (void *)download);
    if (!download)
        return;

    gchar *filename = download_suggested_filename(download);
    DownloadItem *item = downloads_box ? create_download_row(download, filename) : nullptr;
    gulong decide_id = g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), item);
    if (item)
        item->decide_id = decide_id;
    g_free(filename);
}

static void on_clear_clicked(GtkButton *button, gpointer user_data) {
    if (!downloads_box)
        return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(downloads_box));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *row = GTK_WIDGET(l->data);
        DownloadItem *item = (DownloadItem *)g_object_get_data(G_OBJECT(row), "kite-download-item");
        if (item && item->terminal)
            gtk_widget_destroy(row);
    }
    g_list_free(children);
    update_downloads_empty_state();
}

GtkWidget *create_downloads_button() {
    if (downloads_button)
        return downloads_button;

    downloads_button = gtk_menu_button_new();
    gtk_button_set_image(GTK_BUTTON(downloads_button),
                         gtk_image_new_from_icon_name("folder-download-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(downloads_button), TRUE);
    gtk_widget_set_tooltip_text(downloads_button, "Downloads");

    GtkWidget *popover = gtk_popover_new(downloads_button);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(downloads_button), popover);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(content, 8);
    gtk_widget_set_margin_end(content, 8);
    gtk_widget_set_margin_top(content, 8);
    gtk_widget_set_margin_bottom(content, 8);
    gtk_container_add(GTK_CONTAINER(popover), content);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Downloads</b>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    GtkWidget *clear = gtk_button_new_with_label("Clear finished");
    gtk_button_set_relief(GTK_BUTTON(clear), GTK_RELIEF_NONE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(header), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), header, FALSE, FALSE, 0);

    downloads_empty = gtk_label_new("No downloads yet");
    gtk_style_context_add_class(gtk_widget_get_style_context(downloads_empty), "dim-label");
    gtk_widget_set_margin_top(downloads_empty, 16);
    gtk_widget_set_margin_bottom(downloads_empty, 16);
    gtk_box_pack_start(GTK_BOX(content), downloads_empty, FALSE, FALSE, 0);

    downloads_scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(downloads_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(downloads_scroller, 400, 260);
    downloads_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(downloads_scroller), downloads_box);
    gtk_box_pack_start(GTK_BOX(content), downloads_scroller, TRUE, TRUE, 0);

    gtk_widget_show_all(content);
    update_downloads_empty_state();
    return downloads_button;
}

void init_downloads(WebKitWebContext *context) {
    if (!context)
        context = webkit_web_context_get_default();
    g_printerr("[Kite DEBUG] init_downloads context=%p\n", (void *)context);
    g_signal_connect(context, "download-started", G_CALLBACK(on_download_started), nullptr);
}
