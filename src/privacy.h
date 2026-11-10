#pragma once

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

// Applies cookie and tracking-protection policies to a WebKit context.
void privacy_apply_context(WebKitWebContext *context);

// Clears cookies, caches and site storage at shutdown when enabled in settings.
void privacy_clear_on_exit();

// Returns the toolbar privacy button; creates the popover on first call.
GtkWidget *create_privacy_button();
void update_privacy_button(WebKitWebView *web_view);
