#include "downloads.h"

#include <gtk/gtk.h>

static gboolean on_download_decide_destination(WebKitDownload *download, const gchar *suggested_filename,
                                               gpointer user_data) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Salvează fișierul", NULL, GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Anulează", GTK_RESPONSE_CANCEL, "_Salvează",
                                                    GTK_RESPONSE_ACCEPT, NULL);

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

void on_download_started(WebKitWebContext *context, WebKitDownload *download, gpointer user_data) {
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_decide_destination), NULL);
}

void init_downloads() {
    WebKitWebContext *context = webkit_web_context_get_default();
    g_signal_connect(context, "download-started", G_CALLBACK(on_download_started), NULL);
}
