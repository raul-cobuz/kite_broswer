#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

gboolean on_load_failed(WebKitWebView *web_view, WebKitLoadEvent load_event, const gchar *failing_uri, GError *error,
                        gpointer user_data);
gboolean on_load_failed_with_tls_errors(WebKitWebView *web_view, const gchar *failing_uri, GTlsCertificate *certificate,
                                        GTlsCertificateFlags errors, gpointer user_data);
void on_web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, gpointer user_data);
