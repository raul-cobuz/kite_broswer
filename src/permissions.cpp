#include "permissions.h"

#include "browser_window.h"
#include "settings.h"

static gchar *permission_origin(WebKitWebView *web_view) {
    const gchar *uri = webkit_web_view_get_uri(web_view);
    if (!uri)
        return g_strdup("this page");
    if (g_str_has_prefix(uri, "file://"))
        return g_strdup("local files");

    GError *error = nullptr;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    if (!parsed) {
        if (error)
            g_error_free(error);
        return g_strdup("this page");
    }

    const gchar *host = g_uri_get_host(parsed);
    gchar *origin = g_strdup((host && *host) ? host : "this page");
    g_uri_unref(parsed);
    return origin;
}

static gchar *permission_description(WebKitPermissionRequest *request, const gchar **key) {
    *key = nullptr;

    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        WebKitUserMediaPermissionRequest *media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
        gboolean audio = webkit_user_media_permission_is_for_audio_device(media);
        gboolean video = webkit_user_media_permission_is_for_video_device(media);
        *key = "media";
        if (audio && video)
            return g_strdup("use your camera and microphone");
        if (video)
            return g_strdup("use your camera");
        if (audio)
            return g_strdup("use your microphone");
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

gboolean on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request, gpointer user_data) {
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
        browser.main_window ? GTK_WINDOW(browser.main_window) : nullptr,
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
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
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), remember, FALSE, FALSE, 0);
    prompt->remember = remember;

    g_signal_connect(dialog, "response", G_CALLBACK(on_permission_dialog_response), prompt);
    gtk_widget_show_all(dialog);
    return TRUE;
}
