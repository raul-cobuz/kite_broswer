#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

gboolean on_permission_request(WebKitWebView *web_view, WebKitPermissionRequest *request, gpointer user_data);
