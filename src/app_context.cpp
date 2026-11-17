#include "app_context.h"

#include <glib.h>

#include "profile.h"

static WebKitWebContext *context = nullptr;

void app_context_init() {
    if (context)
        return;

    if (profile_is_default()) {
        context = webkit_web_context_get_default();
        return;
    }

    const gchar *data_dir = profile_webkit_data_dir();
    const gchar *cache_dir = profile_webkit_cache_dir();
    WebKitWebsiteDataManager *manager =
        webkit_website_data_manager_new("base-data-directory", data_dir, "base-cache-directory", cache_dir, nullptr);
    if (!manager) {
        g_printerr("[Kite] Could not create the WebKit data manager for profile %s; using the default context.\n",
                   profile_current_id());
        context = webkit_web_context_get_default();
        return;
    }

    context = webkit_web_context_new_with_website_data_manager(manager);
    g_object_unref(manager);
}

WebKitWebContext *app_webkit_context() {
    if (!context)
        app_context_init();
    return context ? context : webkit_web_context_get_default();
}
