#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include "adblock.h"
#include "browser_window.h"
#include "dark_mode.h"
#include "downloads.h"
#include "history.h"
#include "kite_pages.h"
#include "settings.h"

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    g_set_application_name("Kite Browser");

    load_settings();
    save_settings();
    history_init();

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        css_provider, ".incognito-mode { background: #1a0b2e; color: #b28ced; border-bottom: 2px solid #8e44ad; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    init_adblocker();
    init_dark_mode();

    WebKitWebContext *context = webkit_web_context_get_default();
    webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    init_kite_pages(context);
    init_downloads();

    create_browser_window();
    gtk_main();

    history_close();
    return 0;
}
